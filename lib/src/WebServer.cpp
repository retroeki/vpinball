// license:GPLv3+

#include "core/stdafx.h"
#include "WebServer.h"

#include "core/FileLocator.h"
#include "core/VPApp.h"
#include "core/vpversion.h"
#include "parts/pintable.h"
#include "ui/live/LiveUI.h"

#include "VPinballLib.h"
#include "ZipUtils.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <filesystem>
#include <map>
#include <algorithm>

using json = nlohmann::json;

namespace {
   constexpr const char* HEADER_JSON = "Content-Type: application/json\r\n";

   constexpr const char* RESPONSE_OK = "OK";
   constexpr const char* RESPONSE_BAD_REQUEST = "Bad request";
   constexpr const char* RESPONSE_NOT_FOUND = "File not found";
   constexpr const char* RESPONSE_METHOD_NOT_ALLOWED = "Method Not Allowed";
   constexpr const char* RESPONSE_CONFLICT = "Conflict";
   constexpr const char* RESPONSE_INTERNAL_SERVER_ERROR = "Server error";

   constexpr int STATUS_OK = 200;
   constexpr int STATUS_BAD_REQUEST = 400;
   constexpr int STATUS_NOT_FOUND = 404;
   constexpr int STATUS_METHOD_NOT_ALLOWED = 405;
   constexpr int STATUS_CONFLICT = 409;
   constexpr int STATUS_INTERNAL_SERVER_ERROR = 500;

   constexpr size_t MAX_UPLOAD_SIZE = size_t(2) * 1024 * 1024 * 1024; // 2GB
}

std::mutex WebServer::s_logMutex;
vector<unsigned long> WebServer::s_logConnections;
vector<unsigned long> WebServer::s_statusConnections;
std::deque<string> WebServer::s_recentLogs;
WebServer* WebServer::s_instance = nullptr;
int64_t WebServer::s_lastUpdateTimestamp = 0;

void WebServer::EventHandler(struct mg_connection *c, int ev, void *ev_data)
{
   WebServer* webServer = (WebServer*)c->fn_data;

   if (ev == MG_EV_HTTP_MSG) {
      struct mg_http_message *hm = (struct mg_http_message *) ev_data;

      if (mg_match(hm->uri, mg_str("/info"), NULL))
         webServer->Info(c, hm);
      else if (mg_match(hm->uri, mg_str("/status"), NULL))
         webServer->Status(c, hm);
      else if (mg_match(hm->uri, mg_str("/assets/*"), NULL))
         webServer->Assets(c, hm);
      else if (mg_match(hm->uri, mg_str("/files"), NULL))
         webServer->Files(c, hm);
      else if (mg_match(hm->uri, mg_str("/download"), NULL))
         webServer->Download(c, hm);
      else if (mg_match(hm->uri, mg_str("/upload"), NULL))
         webServer->Upload(c, hm);
      else if (mg_match(hm->uri, mg_str("/delete"), NULL))
         webServer->Delete(c, hm);
      else if (mg_match(hm->uri, mg_str("/folder"), NULL))
         webServer->Folder(c, hm);
      else if (mg_match(hm->uri, mg_str("/extract"), NULL))
         webServer->Extract(c, hm);
      else if (mg_match(hm->uri, mg_str("/command"), NULL))
         webServer->Command(c, hm);
      else if (mg_match(hm->uri, mg_str("/log-stream"), NULL))
         webServer->LogStream(c, hm);
      else if (mg_match(hm->uri, mg_str("/rename"), NULL))
         webServer->Rename(c, hm);
      else if (mg_match(hm->uri, mg_str("/move"), NULL))
         webServer->Move(c, hm);
      else if (mg_match(hm->uri, mg_str("/setroot"), NULL))
         webServer->SetRoot(c, hm);
      else if (mg_match(hm->uri, mg_str("/getroot"), NULL))
         webServer->GetRoot(c, hm);
      else {
         struct mg_http_serve_opts opts = {};

         string uri(hm->uri.buf, hm->uri.len);
         if (!uri.empty() && uri.front() == '/') uri.erase(0, 1);

         std::filesystem::path webBase = std::filesystem::path(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets)) / "web";
         std::filesystem::path asset = uri.empty() ? webBase / "vpx.html" : webBase / uri;

         std::error_code ec;
         if (!uri.empty() && std::filesystem::exists(asset, ec))
            mg_http_serve_file(c, hm, asset.string().c_str(), &opts);
         else
            mg_http_serve_file(c, hm, (webBase / "vpx.html").string().c_str(), &opts);
      }
   }
   else if (ev == MG_EV_WAKEUP) {
      const struct mg_str* data = (struct mg_str*)ev_data;
      if (c->is_websocket)
         mg_ws_send(c, data->buf, data->len, WEBSOCKET_OP_TEXT);
      else
         mg_send(c, data->buf, data->len);
   }
   else if (ev == MG_EV_CLOSE) {
      std::lock_guard<std::mutex> lock(s_logMutex);
      auto logIt = std::find(s_logConnections.begin(), s_logConnections.end(), c->id);
      if (logIt != s_logConnections.end())
         s_logConnections.erase(logIt);

      auto statusIt = std::find(s_statusConnections.begin(), s_statusConnections.end(), c->id);
      if (statusIt != s_statusConnections.end())
         s_statusConnections.erase(statusIt);
   }
}

void WebServer::LogAppender(const string& formattedLog)
{
   static std::queue<string> pendingLogs;
   static std::mutex pendingMutex;
   static bool processingPending = false;

   if (!s_instance) {
      std::lock_guard<std::mutex> lock(pendingMutex);
      pendingLogs.push(formattedLog);

      while (pendingLogs.size() > 100)
         pendingLogs.pop();

      return;
   }

   s_instance->AddLogEntry(formattedLog);

   if (!processingPending) {
      std::lock_guard<std::mutex> lock(pendingMutex);
      processingPending = true;
      while (!pendingLogs.empty()) {
         s_instance->AddLogEntry(pendingLogs.front());
         pendingLogs.pop();
      }
      processingPending = false;
   }
}

void WebServer::SetLastUpdate()
{
   s_lastUpdateTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()
   ).count();
   PLOGD.printf("Web interface last update timestamp set to: %lld", (long long)s_lastUpdateTimestamp);

   BroadcastStatus();
}

WebServer::WebServer()
{
   m_run = false;
   s_instance = this;
}

WebServer::~WebServer()
{
   m_run = false;
   s_instance = nullptr;

   {
      std::lock_guard<std::mutex> lock(s_logMutex);
      s_logConnections.clear();
      s_statusConnections.clear();
      s_recentLogs.clear();
   }

   if (m_pThread && m_pThread->joinable())
      m_pThread->join();
}

void WebServer::Start()
{
   if (m_run) {
      PLOGE.printf("Web server already running");
      return;
   }

   // mg_log_set(MG_LL_DEBUG);

   // Clear any half-finished uploads from a previous session. Staged uploads
   // live under <webRoot>/.uploading and are only promoted to their real
   // location once complete, so anything left here is from an interrupted
   // transfer and must not linger as a hidden orphan.
   std::error_code sweepEc;
   std::filesystem::remove_all(BuildTablePath(".uploading"), sweepEc);

   const auto addrPropId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::StringPropertyDef>("Standalone"s, "WebServerAddr"s, ""s, ""s, false, "0.0.0.0"s));
   const auto portPropId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::IntPropertyDef>("Standalone"s, "WebServerPort"s, ""s, ""s, false, INT_MIN, INT_MAX, 2112));
   const string addr = g_settingsService.GetAppSettings().GetString(addrPropId);
   const int port = g_settingsService.GetAppSettings().GetInt(portPropId);

   string bindUrl = "http://" + addr + ':' + std::to_string(port);

   PLOGI.printf("Starting web server at %s", bindUrl.c_str());

   mg_mgr_init(&m_mgr);
   if (!mg_wakeup_init(&m_mgr)) {
      PLOGE.printf("Unable to create the web server wakeup pipe, log and status streaming will not be delivered");
   }

   SetLastUpdate();

   if (mg_http_listen(&m_mgr, bindUrl.c_str(), &WebServer::EventHandler, this)) {
      m_run = true;

      PLOGI.printf("Web server started");

      string ip = GetIPAddress();

      if (!ip.empty()) {
         m_url = "http://" + ip + ':' + std::to_string(port);

         PLOGI.printf("To access the web server, in a browser go to: %s", m_url.c_str());
      }
      else
         m_url.clear();

      VPinballLib::WebServerData webServerData = { m_url };
      VPinballLib::VPinballLib::SendEvent(VPINBALL_EVENT_WEB_SERVER, &webServerData);

      m_pThread = std::make_unique<std::thread>([this]() {
         while (m_run)
            mg_mgr_poll(&m_mgr, 100);

         mg_mgr_free(&m_mgr);

         PLOGI.printf("Web server closed");
      });
   }
   else {
      PLOGE.printf("Unable to start web server");

      VPinballLib::VPinballLib::SendEvent(VPINBALL_EVENT_WEB_SERVER, nullptr);
   }
}

void WebServer::Stop()
{
   if (!m_run) {
      PLOGE.printf("Web server is not running");
      return;
   }

   m_run = false;
   m_url.clear();

   if (m_pThread && m_pThread->joinable())
      m_pThread->join();

   VPinballLib::VPinballLib::SendEvent(VPINBALL_EVENT_WEB_SERVER, nullptr);
}

void WebServer::Update()
{
   const auto serverPropId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::BoolPropertyDef>("Standalone"s, "WebServer"s, ""s, ""s, false, false));
   bool enabled = g_settingsService.GetAppSettings().GetBool(serverPropId);

   if (enabled && !m_run)
      Start();
   else if (!enabled && m_run)
      Stop();
}

// Re-query the active Wi-Fi/ethernet IP and re-emit VPINBALL_EVENT_WEB_SERVER so
// the host UI + foreground notification pick up the new URL. The listening
// socket is bound to 0.0.0.0 so it normally survives a Wi-Fi reconnect or DHCP
// renewal — only m_url goes stale. The host pings /info and calls this when the
// probe fails; if the socket really is dead the listener is unchanged and the
// host falls back to a manual disable/enable toggle.
void WebServer::RefreshUrl()
{
   if (!m_run)
      return;

   const auto portPropId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::IntPropertyDef>("Standalone"s, "WebServerPort"s, ""s, ""s, false, INT_MIN, INT_MAX, 2112));
   const int port = g_settingsService.GetAppSettings().GetInt(portPropId);

   const string ip = GetIPAddress();
   if (!ip.empty())
      m_url = "http://" + ip + ':' + std::to_string(port);
   else
      m_url.clear();

   PLOGI.printf("Web server URL refreshed: %s", m_url.c_str());

   VPinballLib::WebServerData webServerData = { m_url };
   VPinballLib::VPinballLib::SendEvent(VPINBALL_EVENT_WEB_SERVER, &webServerData);
}

string WebServer::GetUrl()
{
   return m_run ? m_url : string();
}

void WebServer::Info(struct mg_connection *c, struct mg_http_message* hm)
{
   json j = {{"version", VP_VERSION_STRING_FULL_LITERAL}};
   string response = j.dump();
   mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
}

void WebServer::Status(struct mg_connection *c, struct mg_http_message* hm)
{
   mg_ws_upgrade(c, hm, NULL);

   {
      std::lock_guard<std::mutex> lock(s_logMutex);
      s_statusConnections.push_back(c->id);
   }

   BroadcastStatus();
}

void WebServer::Assets(struct mg_connection *c, struct mg_http_message* hm)
{
   string uri(hm->uri.buf, hm->uri.len);

   if (uri.length() > 8 && uri.substr(0, 8) == "/assets/") {
      string assetPath = uri.substr(8);

      if (!mg_path_is_sane(mg_str(assetPath.c_str()))) {
         mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
         return;
      }

      std::filesystem::path fullPath = std::filesystem::path(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets)) / assetPath;

      std::error_code ec;
      if (std::filesystem::is_regular_file(fullPath, ec)) {
         struct mg_http_serve_opts opts = {};
         mg_http_serve_file(c, hm, fullPath.string().c_str(), &opts);
      }
      else
         mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
   }
   else
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
}

void WebServer::Files(struct mg_connection *c, struct mg_http_message* hm)
{
   char buffer[1024];
   mg_http_get_var(&hm->query, "q", buffer, sizeof(buffer));

   string q = buffer;
   if (!q.empty() && !mg_path_is_sane(mg_str(buffer))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   PLOGD.printf("Retrieving file list: q=%s", q.c_str());

   string path = BuildTablePath(q.c_str());
   if (!q.empty())
      path += PATH_SEPARATOR_CHAR;

   DIR* dir = opendir(path.c_str());
   if (!dir) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   json files = json::array();
   struct dirent *entry;

   while ((entry = readdir(dir)) != NULL) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
         continue;

      string file = path + entry->d_name;
      string ext;
      if (entry->d_type != DT_DIR) ext = extension_from_path(file);

      struct stat st;
      if (stat(file.c_str(), &st) == 0) {
         char datebuf[32];
         struct tm tm;
         gmtime_r(&st.st_mtime, &tm);
         strftime(datebuf, sizeof(datebuf), "%Y-%m-%dT%H:%M:%SZ", &tm);

         json fileEntry = {
            {"name", entry->d_name},
            {"ext", ext},
            {"isDir", entry->d_type == DT_DIR},
            {"size", (long long)st.st_size},
            {"date", datebuf}
         };

         files.push_back(fileEntry);
      }
   }

   closedir(dir);

   string response = files.dump();
   mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
}

void WebServer::Download(struct mg_connection *c, struct mg_http_message* hm)
{
   string q;
   if (!ValidatePathParameter(c, hm, "q", q))
      return;

   PLOGI.printf("Downloading file: q=%s", q.c_str());

   string path = BuildTablePath(q.c_str());

   struct mg_http_serve_opts opts = {};
   mg_http_serve_file(c, hm, path.c_str(), &opts);
}

void WebServer::Upload(struct mg_connection *c, struct mg_http_message* hm)
{
   char q[1024];
   mg_http_get_var(&hm->query, "q", q, sizeof(q));
   if (*q != '\0' && !mg_path_is_sane(mg_str(q))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   string file;
   if (!ValidatePathParameter(c, hm, "file", file))
      return;

   char offsetStr[32];
   mg_http_get_var(&hm->query, "offset", offsetStr, sizeof(offsetStr));
   long offset = offsetStr[0] ? strtol(offsetStr, nullptr, 10) : 0;
   if (offset <= 0) {
      PLOGI.printf("Uploading file: file=%s", file.c_str());
   }

   char lengthStr[32];
   mg_http_get_var(&hm->query, "length", lengthStr, sizeof(lengthStr));
   long length = lengthStr[0] ? strtol(lengthStr, nullptr, 10) : 0;

   // Upload into a hidden staging dir and promote to the real location only
   // once the whole file has arrived. Writing straight to the final name left
   // a truncated, unloadable table in the library whenever a transfer dropped
   // mid-way. The staging tree mirrors the target path (both under the same
   // web root) so the promote is a same-filesystem atomic rename; stragglers
   // are swept on server start (see Start()).
   const std::filesystem::path targetDir = BuildTablePath(q);
   const std::filesystem::path stagingDir = BuildTablePath("") / ".uploading" / q;
   std::error_code ec;
   std::filesystem::create_directories(stagingDir, ec);   // mg_http_upload won't create dirs

   const long bytesWritten = mg_http_upload(c, hm, &mg_fs_posix, stagingDir.string().c_str(), MAX_UPLOAD_SIZE);

   // File complete: promote it before signalling progress, so the host sees
   // the finished table at its real path the instant it reaches 100%.
   if (bytesWritten == length && length > 0) {
      const std::filesystem::path stagedFile = stagingDir / file;
      const std::filesystem::path finalFile = targetDir / file;
      std::filesystem::create_directories(targetDir, ec);
      std::filesystem::rename(stagedFile, finalFile, ec);
      if (ec)
         PLOGE.printf("Upload finalize failed: %s -> %s (%s)", stagedFile.string().c_str(),
                      finalFile.string().c_str(), ec.message().c_str());
      else {
         if (*q == '\0' && file == "VPinballX.ini") {
            g_settingsService.GetAppSettings().SetIniPath(finalFile.string());
            g_settingsService.GetAppSettings().Load(true);
            g_settingsService.GetAppSettings().Save();
         }
         SetLastUpdate();
      }
   }

   // Per-chunk progress event so the host can render an upload indicator on
   // the matching library row. mg_http_upload returns the cumulative size of
   // the file on disk after this chunk; pair that with the client-reported
   // ?length=... total to derive a percentage. The web client also sends an
   // empty-body tail POST after the last data chunk to flush — mg_http_upload
   // returns 0 for that one, which would yank the UI back to 0%, so skip it.
   if (bytesWritten > 0 && length > 0) {
      VPinballLib::WebUploadData uploadData;
      uploadData.folder = q;
      uploadData.file = file;
      uploadData.bytesWritten = static_cast<uint64_t>(bytesWritten);
      uploadData.totalBytes = static_cast<uint64_t>(length);
      VPinballLib::VPinballLib::SendEvent(VPINBALL_EVENT_WEB_UPLOAD, &uploadData);
   }
}

void WebServer::Delete(struct mg_connection *c, struct mg_http_message* hm)
{
   string q;
   if (!ValidatePathParameter(c, hm, "q", q))
      return;

   string path = BuildTablePath(q.c_str());

   std::error_code ec;
   if (std::filesystem::is_regular_file(path, ec)) {
      if (std::filesystem::remove(path, ec)) {
         SetLastUpdate();
         mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
      }
      else {
         PLOGE.printf("Failed to delete file: q=%s, error=%s", q.c_str(), ec.message().c_str());
         mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
      }
   }
   else if (std::filesystem::is_directory(path, ec)) {
      const std::uintmax_t removed = std::filesystem::remove_all(path, ec);
      if (!ec && removed != 0) {
         SetLastUpdate();
         mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
      }
      else {
         PLOGE.printf("Failed to delete directory: q=%s, error=%s", q.c_str(), ec.message().c_str());
         mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
      }
   }
   else
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
}

void WebServer::Rename(struct mg_connection *c, struct mg_http_message* hm)
{
   string q;
   if (!ValidatePathParameter(c, hm, "q", q))
      return;

   string newName;
   if (!ValidatePathParameter(c, hm, "name", newName))
      return;

   string oldPath = BuildTablePath(q.c_str());
   std::filesystem::path oldFile(oldPath);

   std::error_code ec;
   if (!std::filesystem::exists(oldFile, ec)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }

   std::filesystem::path newFile = oldFile.parent_path() / newName;
   if (std::filesystem::exists(newFile, ec)) {
      mg_http_reply(c, STATUS_CONFLICT, "", RESPONSE_CONFLICT);
      return;
   }

   std::filesystem::rename(oldFile, newFile, ec);
   if (ec) {
      PLOGE.printf("Failed to rename: q=%s, name=%s, error=%s", q.c_str(), newName.c_str(), ec.message().c_str());
      mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
   }
   else {
      SetLastUpdate();
      mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
   }
}

void WebServer::Move(struct mg_connection *c, struct mg_http_message* hm)
{
   string q;
   if (!ValidatePathParameter(c, hm, "q", q))
      return;

   string dest;
   if (!ValidatePathParameter(c, hm, "dest", dest))
      return;

   string oldPath = BuildTablePath(q.c_str());
   string newPath = BuildTablePath(dest.c_str());

   std::filesystem::path oldFile(oldPath);
   std::filesystem::path newFile(newPath);

   std::error_code ec;
   if (!std::filesystem::exists(oldFile, ec)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }

   if (std::filesystem::exists(newFile, ec)) {
      mg_http_reply(c, STATUS_CONFLICT, "", RESPONSE_CONFLICT);
      return;
   }

   if (!newFile.parent_path().empty() && !std::filesystem::exists(newFile.parent_path(), ec)) {
      std::filesystem::create_directories(newFile.parent_path(), ec);
      if (ec) {
         PLOGE.printf("Failed to create directory: dest=%s, error=%s", dest.c_str(), ec.message().c_str());
         mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
         return;
      }
   }

   std::filesystem::rename(oldFile, newFile, ec);
   if (ec) {
      PLOGE.printf("Failed to move: q=%s, dest=%s, error=%s", q.c_str(), dest.c_str(), ec.message().c_str());
      mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
   }
   else {
      SetLastUpdate();
      mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
   }
}

// Two stored web roots (library / advanced) registered by the host. Implemented
// in VPinballLib.cpp. SetActiveWebRoot selects the path now serving as the
// browse root, so the file-browser ops use it instead of the app Tables folder
// (the preference folder stays anchored at the internal app dir for INI/log/user/ writes).
extern "C" void VPinballSetActiveWebRoot(int type);
extern "C" const char* VPinballGetActiveWebRoot();
extern "C" const char* VPinballGetWebLibraryPath();
extern "C" const char* VPinballGetWebAdvancedPath();

void WebServer::SetRoot(struct mg_connection *c, struct mg_http_message* hm)
{
   char buffer[16];
   mg_http_get_var(&hm->query, "type", buffer, sizeof(buffer));
   string type = buffer;

   int t;
   if (type == "library") t = 0;
   else if (type == "advanced") t = 1;
   else {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   VPinballSetActiveWebRoot(t);
   const char* resolved = VPinballGetActiveWebRoot();
   if (resolved == nullptr || resolved[0] == '\0') {
      mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
      return;
   }

   nlohmann::json j;
   j["type"] = type;
   j["path"] = resolved;
   string body = j.dump();
   mg_http_reply(c, STATUS_OK, "Content-Type: application/json\r\n", "%s", body.c_str());
}

void WebServer::GetRoot(struct mg_connection *c, struct mg_http_message* hm)
{
   const char* libPath = VPinballGetWebLibraryPath();
   const char* advPath = VPinballGetWebAdvancedPath();
   const char* active = VPinballGetActiveWebRoot();
   const string current = active ? active : (libPath ? libPath : "");

   string activeType;
   if (libPath && current.find(libPath) == 0) activeType = "library";
   else if (advPath && current.find(advPath) == 0) activeType = "advanced";
   else activeType = "library"; // default fallback

   nlohmann::json j;
   j["active"] = activeType;
   j["library_available"] = (libPath != nullptr && libPath[0] != '\0');
   j["advanced_available"] = (advPath != nullptr && advPath[0] != '\0');
   j["library_path"] = libPath ? libPath : "";
   j["advanced_path"] = advPath ? advPath : "";
   string body = j.dump();
   mg_http_reply(c, STATUS_OK, "Content-Type: application/json\r\n", "%s", body.c_str());
}

void WebServer::Folder(struct mg_connection *c, struct mg_http_message* hm)
{
   char q[1024];
   mg_http_get_var(&hm->query, "q", q, sizeof(q));

   if (*q == '\0' || !mg_path_is_sane(mg_str(q))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   string path = BuildTablePath(q);

   std::error_code ec;
   if (std::filesystem::create_directory(path, ec)) {
      SetLastUpdate();
      mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
   }
   else {
      PLOGE.printf("Failed to create folder: q=%s, error=%s", q, ec.message().c_str());
      mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
   }
}

void WebServer::Extract(struct mg_connection *c, struct mg_http_message* hm)
{
   char q[1024];
   mg_http_get_var(&hm->query, "q", q, sizeof(q));

   if (*q == '\0' || !mg_path_is_sane(mg_str(q))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   string path = BuildTablePath(q);

   const std::filesystem::path filePath(path);
   std::error_code ec;
   if (std::filesystem::is_regular_file(filePath, ec)) {
      const string ext = extension_from_path(path);
      if (ext == "zip" || ext == "vpxz") {
         if (ZipUtils::Unzip(filePath, filePath.parent_path(), nullptr)) {
            PLOGI.printf("File unzipped: q=%s", path.c_str());
            SetLastUpdate();
            mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
         }
         else
            mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
      }
      else
         mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
   }
   else
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
}

void WebServer::Command(struct mg_connection *c, struct mg_http_message* hm)
{
   char cmd[1024];
   mg_http_get_var(&hm->query, "cmd", cmd, sizeof(cmd));

   if (*cmd == '\0') {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   if (!strncmp(cmd, "fps", sizeof(cmd))) {
      if (g_pplayer && g_pplayer->m_liveUI) {
         g_pplayer->m_liveUI->ToggleFPS();
         mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
      }
      else
         mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
   }
   else if (!strncmp(cmd, "shutdown", sizeof(cmd))) {
      if (g_pplayer) {
         g_pplayer->SetCloseState(Player::CS_CLOSE_CAPTURE_SCREENSHOT);
         mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
      }
      else
         mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
   }
   else if (!strncmp(cmd, "cls", sizeof(cmd))) {
      {
         std::lock_guard<std::mutex> lock(s_logMutex);
         s_recentLogs.clear();
      }
      json j = {{"status", "success"}, {"message", "Logs cleared"}};
      string response = j.dump();
      mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
   }
   else if (!strncmp(cmd, "refresh_tables", sizeof(cmd))) {
      VPinballLib::CommandData commandData = { "reloadTables", "" };
      VPinballLib::VPinballLib::SendEvent(VPINBALL_EVENT_COMMAND, &commandData);
      mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
   }
   else
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
}

void WebServer::LogStream(struct mg_connection *c, struct mg_http_message* hm)
{
   mg_printf(c, "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/event-stream\r\n"
              "Cache-Control: no-cache\r\n"
              "Connection: keep-alive\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "\r\n");

   c->is_resp = 0;

   {
      std::lock_guard<std::mutex> lock(s_logMutex);
      s_logConnections.push_back(c->id);

      for (const auto& logLine : s_recentLogs) {
         string data = "data: " + logLine + "\n\n";
         mg_send(c, data.c_str(), data.length());
      }
   }
}

void WebServer::AddLogEntry(const string& formattedLog)
{
   std::lock_guard<std::mutex> lock(s_logMutex);

   if (!s_recentLogs.empty() && s_recentLogs.back() == formattedLog)
      return;

   s_recentLogs.push_back(formattedLog);

   while (s_recentLogs.size() > MAX_RECENT_LOGS)
      s_recentLogs.pop_front();

   BroadcastLogEntry(formattedLog);
}

void WebServer::BroadcastLogEntry(const string& formattedLog)
{
   if (s_logConnections.empty())
      return;

   const string data = "data: " + formattedLog + "\n\n";

   for (const unsigned long id : s_logConnections)
      mg_wakeup(&m_mgr, id, data.c_str(), data.length());
}

void WebServer::BroadcastStatus()
{
   if (s_instance == nullptr) return;

   bool running = g_pplayer != nullptr;
   string currentTable = running ? g_pplayer->m_ptable->m_filename.string() : ""s;

   json j = {
      {"running", running},
      {"currentTable", currentTable.empty() ? nullptr : json(currentTable)},
      {"lastUpdate", s_lastUpdateTimestamp}
   };

   const string response = j.dump();

   std::lock_guard<std::mutex> lock(s_logMutex);
   if (s_statusConnections.empty()) return;
   for (const unsigned long id : s_statusConnections)
      mg_wakeup(&s_instance->m_mgr, id, response.c_str(), response.length());
}

string WebServer::GetIPAddress()
{
   struct ifaddrs *ifaddr;
   struct ifaddrs *ifa;

   if (getifaddrs(&ifaddr) == -1)
      return string();

   string result;
   for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == nullptr)
         continue;

      if (ifa->ifa_addr->sa_family != AF_INET)
         continue;

      // Walk wlan* / eth* / en* only — the upstream filter. Skip loopback and
      // anything else that isn't a real network the user's PC could reach.
      if (strncmp(ifa->ifa_name, "wlan", 4) != 0
         && strncmp(ifa->ifa_name, "eth", 3) != 0
         && strncmp(ifa->ifa_name, "en", 2) != 0)
         continue;

      // Skip interfaces that aren't actually operational. On Android, Wi-Fi
      // going down (airplane mode, Wi-Fi off) doesn't drop the wlan0 address
      // from getifaddrs() — the kernel still reports the cached IP — but
      // IFF_RUNNING clears, so this check is the one that matters. IFF_UP
      // alone isn't enough; lots of stale-but-not-running interfaces have it.
      if ((ifa->ifa_flags & IFF_UP) == 0)
         continue;
      if ((ifa->ifa_flags & IFF_RUNNING) == 0)
         continue;
      if (ifa->ifa_flags & IFF_LOOPBACK)
         continue;

      // Skip link-local 169.254.0.0/16 — set when DHCP hasn't yielded an
      // address. The server is technically bound there but no client on the
      // user's LAN can reach it.
      struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
      const uint32_t addr = ntohl(sin->sin_addr.s_addr);
      if ((addr & 0xFFFF0000u) == 0xA9FE0000u) // 169.254.0.0/16
         continue;

      char host[NI_MAXHOST];
      if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0) {
         result = host;
         break;
      }
   }

   freeifaddrs(ifaddr);
   return result;
}

bool WebServer::ValidatePathParameter(struct mg_connection *c, struct mg_http_message* hm, const char* paramName, string& outValue)
{
   char buffer[1024];
   mg_http_get_var(&hm->query, paramName, buffer, sizeof(buffer));

   if (*buffer == '\0' || !mg_path_is_sane(mg_str(buffer))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return false;
   }

   outValue = buffer;
   return true;
}

std::filesystem::path WebServer::BuildTablePath(const char* relativePath)
{
   // Web file-browser root is independent of the app Tables folder. The host
   // registers a browse root via VPinballSetActiveWebRoot; if unset, fall back
   // to the Tables folder so the desktop builds (which never touch the new API)
   // still work.
   const char* webRoot = VPinballGetActiveWebRoot();
   if (webRoot && webRoot[0] != '\0') {
      return std::filesystem::path(webRoot) / relativePath;
   }
   return g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Tables) / relativePath;
}


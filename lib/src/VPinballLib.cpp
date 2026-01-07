// license:GPLv3+

#include "core/stdafx.h"

#include "core/TableDB.h"
#include "core/VPXPluginAPIImpl.h"
#include "core/extern.h"
#include "VPinballLib.h"
#include "VPXProgress.h"
#include "WebServer.h"

#include <zip.h>
#include <filesystem>
#include <chrono>
#include <thread>
#include <set>
#include <map>
#include <queue>
#include <mutex>
#include <nlohmann/json.hpp>

#ifdef __APPLE__
#include "VPinballLib_iOS.h"
#endif


MSGPI_EXPORT void MSGPIAPI AlphaDMDPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI AlphaDMDPluginUnload();
MSGPI_EXPORT void MSGPIAPI B2SPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI B2SPluginUnload();
MSGPI_EXPORT void MSGPIAPI B2SLegacyPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI B2SLegacyPluginUnload();
MSGPI_EXPORT void MSGPIAPI DOFPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI DOFPluginUnload();
MSGPI_EXPORT void MSGPIAPI DMDUtilPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI DMDUtilPluginUnload();
MSGPI_EXPORT void MSGPIAPI FlexDMDPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI FlexDMDPluginUnload();
MSGPI_EXPORT void MSGPIAPI PinMAMEPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI PinMAMEPluginUnload();
MSGPI_EXPORT void MSGPIAPI PUPPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI PUPPluginUnload();
MSGPI_EXPORT void MSGPIAPI RemoteControlPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI RemoteControlPluginUnload();
MSGPI_EXPORT void MSGPIAPI ScoreViewPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI ScoreViewPluginUnload();
MSGPI_EXPORT void MSGPIAPI SerumPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI SerumPluginUnload();
MSGPI_EXPORT void MSGPIAPI WMPPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI WMPPluginUnload();
MSGPI_EXPORT void MSGPIAPI UpscaleDMDPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api);
MSGPI_EXPORT void MSGPIAPI UpscaleDMDPluginUnload();

// Global storage for PUP video source dimensions (updated by PUP plugin during render)
static std::atomic<int> g_pupVideoSourceWidth{0};
static std::atomic<int> g_pupVideoSourceHeight{0};

extern "C" void SetPUPVideoSourceSize(int width, int height)
{
   g_pupVideoSourceWidth = width;
   g_pupVideoSourceHeight = height;
}

static void GetPUPVideoSourceSize(int& width, int& height)
{
   width = g_pupVideoSourceWidth;
   height = g_pupVideoSourceHeight;
}

namespace VPinballLib {

VPinballLib::VPinballLib()
{
}

VPinballLib::~VPinballLib()
{
}

int VPinballLib::AppInit(int argc, char** argv)
{
   if (g_isAndroid)
      SDL_SetHint(SDL_HINT_ANDROID_ALLOW_RECREATE_ACTIVITY, "1");

   if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
      return 0;

   if (g_isIOS) {
      SDL_PropertiesID props = SDL_CreateProperties();
      SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Visual Pinball Player");
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
      m_pWindow = SDL_CreateWindowWithProperties(props);
      SDL_DestroyProperties(props);

      if (!m_pWindow)
         return 0;

      #ifdef __APPLE__
         if (!InitIOS(m_pWindow))
            return 0;
      #endif
   }

   if (g_isAndroid)
      MsgPI::MsgPluginManager::GetInstance().UpdateAPIThread();

   return 1;
}

void VPinballLib::AppIterate()
{
   if (m_gameLoop) {
      m_gameLoop();

      if (g_pplayer && (g_pplayer->GetCloseState() == Player::CS_PLAYING
         || g_pplayer->GetCloseState() == Player::CS_USER_INPUT))
         return;

      CComObject<PinTable>* pActiveTable = g_pvp->GetActiveTable();

      if (g_pplayer->GetCloseState() == Player::CS_CLOSE_CAPTURE_SCREENSHOT) {
         if (m_captureInProgress)
            return;

         std::filesystem::path tablePath(pActiveTable->m_filename);
         string imageFilename = tablePath.stem().string() + ".jpg";
         string imagePath = tablePath.parent_path().string() + PATH_SEPARATOR_CHAR + imageFilename;

         if (std::filesystem::exists(imagePath)) {
            g_pplayer->SetCloseState(Player::CS_CLOSE_APP);
            return;
         }

         m_captureInProgress = true;

         g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot(imagePath,
            [this, imagePath](bool success) {
               m_captureInProgress = false;

               if (success) {
                  PLOGI.printf("Screenshot saved: %s", imagePath.c_str());
               }
               else {
                  PLOGE.printf("Failed to save screenshot: %s", imagePath.c_str());
               }

               g_pplayer->SetCloseState(Player::CS_CLOSE_APP);
            });

         return;
      }

      PLOGI.printf("Game Loop stopping, close state: %d", g_pplayer ? (int)g_pplayer->GetCloseState() : -1);

      m_gameLoop = nullptr;

      // The table settings may have been edited during play (camera, rendering, ...), so copy them back to the editor table's settings
      pActiveTable->m_settings.Load(g_pplayer->m_ptable->m_settings);
      pActiveTable->m_settings.SetModified(g_pplayer->m_ptable->m_settings.IsModified());

      delete g_pplayer;
      g_pplayer = nullptr;

      g_pvp->CloseTable(pActiveTable);
   }
}

void VPinballLib::AppEvent(SDL_Event* event)
{
   std::lock_guard<std::mutex> lock(m_eventMutex);
   if (m_gameLoop)
      m_eventQueue.push(*event);
   else {
      while (!m_eventQueue.empty())
         m_eventQueue.pop();
   }
}

bool VPinballLib::PollAppEvent(SDL_Event& event)
{
   std::lock_guard<std::mutex> lock(m_eventMutex);
   if (m_eventQueue.empty())
      return false;

   event = m_eventQueue.front();
   m_eventQueue.pop();
   return true;
}

void VPinballLib::Init(VPinballEventCallback callback)
{
   SetEventCallback(callback);

   SDL_RunOnMainThread([](void* userdata) {
      auto* lib = static_cast<VPinballLib*>(userdata);

      g_pvp = new ::VPinball();
      g_pvp->SetLogicalNumberOfProcessors(SDL_GetNumLogicalCPUCores());
      g_pvp->m_settings.SetIniPath(g_pvp->GetPrefPath() + "VPinballX.ini");
      g_pvp->m_settings.Load(true);
      g_pvp->m_settings.SetVersion_VPinball(string(VP_VERSION_STRING_DIGITS), false);
      g_pvp->m_settings.Save();

      Logger::GetInstance()->Init();
      Logger::GetInstance()->SetupLogger(true);

      PLOGI << "VPX - " << VP_VERSION_STRING_FULL_LITERAL;
      PLOGI << "m_logicalNumberOfProcessors=" << g_pvp->GetLogicalNumberOfProcessors();
      PLOGI << "m_myPath=" << g_pvp->m_myPath;
      PLOGI << "m_myPrefPath=" << g_pvp->GetPrefPath();

      if (!DirExists(PATH_USER)) {
         std::error_code ec;
         if (std::filesystem::create_directory(PATH_USER, ec)) {
            PLOGI.printf("User path created: %s", PATH_USER.c_str());
         }
         else {
            PLOGE.printf("Unable to create user path: %s", PATH_USER.c_str());
         }
      }

      EditableRegistry::RegisterEditable<Ball>();
      EditableRegistry::RegisterEditable<Bumper>();
      EditableRegistry::RegisterEditable<Decal>();
      EditableRegistry::RegisterEditable<DispReel>();
      EditableRegistry::RegisterEditable<Flasher>();
      EditableRegistry::RegisterEditable<Flipper>();
      EditableRegistry::RegisterEditable<Gate>();
      EditableRegistry::RegisterEditable<Kicker>();
      EditableRegistry::RegisterEditable<Light>();
      EditableRegistry::RegisterEditable<LightSeq>();
      EditableRegistry::RegisterEditable<Plunger>();
      EditableRegistry::RegisterEditable<Primitive>();
      EditableRegistry::RegisterEditable<Ramp>();
      EditableRegistry::RegisterEditable<Rubber>();
      EditableRegistry::RegisterEditable<Spinner>();
      EditableRegistry::RegisterEditable<Surface>();
      EditableRegistry::RegisterEditable<Textbox>();
      EditableRegistry::RegisterEditable<Timer>();
      EditableRegistry::RegisterEditable<Trigger>();
      EditableRegistry::RegisterEditable<HitTarget>();
      EditableRegistry::RegisterEditable<PartGroup>();

      VPXPluginAPIImpl::GetInstance();

      RegisterStaticPlugins();

      for (const auto& plugin : MsgPI::MsgPluginManager::GetInstance().GetPlugins()) {
         if (lib->LoadValueBool("Plugin."s + plugin->m_id, "Enable", false))
            plugin->Load(&MsgPI::MsgPluginManager::GetInstance().GetMsgAPI());
      }

      lib->UpdateWebServer();
   }, this, true);
}

void VPinballLib::SetEventCallback(VPinballEventCallback callback)
{
   m_eventCallback = [callback](VPINBALL_EVENT event, void* data) -> void* {
      thread_local string jsonString;
      const char* jsonData = nullptr;

      if (data != nullptr) {
         nlohmann::json j;

         switch(event) {
            case VPINBALL_EVENT_LOADING_ITEMS:
            case VPINBALL_EVENT_LOADING_SOUNDS:
            case VPINBALL_EVENT_LOADING_IMAGES:
            case VPINBALL_EVENT_LOADING_FONTS:
            case VPINBALL_EVENT_LOADING_COLLECTIONS:
            case VPINBALL_EVENT_PRERENDERING: {
               ProgressData* progressData = (ProgressData*)data;
               j["progress"] = progressData->progress;
               jsonString = j.dump();
               jsonData = jsonString.c_str();
               break;
            }
            case VPINBALL_EVENT_RUMBLE: {
               RumbleData* rumbleData = (RumbleData*)data;
               j["lowFrequencyRumble"] = rumbleData->lowFrequencyRumble;
               j["highFrequencyRumble"] = rumbleData->highFrequencyRumble;
               j["durationMs"] = rumbleData->durationMs;
               jsonString = j.dump();
               jsonData = jsonString.c_str();
               break;
            }
            case VPINBALL_EVENT_SCRIPT_ERROR: {
               ScriptErrorData* scriptErrorData = (ScriptErrorData*)data;
               j["error"] = (int)scriptErrorData->error;
               j["line"] = scriptErrorData->line;
               j["position"] = scriptErrorData->position;
               j["description"] = scriptErrorData->description;
               jsonString = j.dump();
               jsonData = jsonString.c_str();
               break;
            }
            case VPINBALL_EVENT_WEB_SERVER: {
               WebServerData* webServerData = (WebServerData*)data;
               j["url"] = webServerData->url;
               jsonString = j.dump();
               jsonData = jsonString.c_str();
               break;
            }
            case VPINBALL_EVENT_COMMAND: {
               CommandData* commandData = (CommandData*)data;
               j["command"] = commandData->command;
               j["data"] = commandData->data;
               jsonString = j.dump();
               jsonData = jsonString.c_str();
               break;
            }
            default:
               break;
         }
      }

      callback(event, jsonData);
      return nullptr;
   };
}

void VPinballLib::SendEvent(VPINBALL_EVENT event, void* data)
{
   auto callback = Instance().m_eventCallback;
   if (callback)
      callback(event, data);

   if (event == VPINBALL_EVENT_PLAYER_STARTED || event == VPINBALL_EVENT_PLAYER_CLOSED)
      WebServer::BroadcastStatus();
}

void VPinballLib::RegisterStaticPlugins()
{
   static constexpr struct {
      const char* id;
      void (*load)(uint32_t, const MsgPluginAPI*);
      void (*unload)();
   } plugins[] = {
      { "ScoreView",     &ScoreViewPluginLoad,     &ScoreViewPluginUnload     },
      { "PinMAME",       &PinMAMEPluginLoad,       &PinMAMEPluginUnload       },
      { "AlphaDMD",      &AlphaDMDPluginLoad,      &AlphaDMDPluginUnload      },
      { "B2S",           &B2SPluginLoad,           &B2SPluginUnload           },
      { "B2SLegacy",     &B2SLegacyPluginLoad,     &B2SLegacyPluginUnload     },
      { "DOF",           &DOFPluginLoad,           &DOFPluginUnload           },
      { "DMDUtil",       &DMDUtilPluginLoad,       &DMDUtilPluginUnload       },
      { "FlexDMD",       &FlexDMDPluginLoad,       &FlexDMDPluginUnload       },
      { "PUP",           &PUPPluginLoad,           &PUPPluginUnload           },
      { "RemoteControl", &RemoteControlPluginLoad, &RemoteControlPluginUnload },
      { "Serum",         &SerumPluginLoad,         &SerumPluginUnload         },
      { "WMP",           &WMPPluginLoad,           &WMPPluginUnload           },
      { "UpscaleDMD",    &UpscaleDMDPluginLoad,    &UpscaleDMDPluginUnload    }
   };

   for (size_t i = 0; i < std::size(plugins); ++i) {
      auto& p = plugins[i];
      MsgPI::MsgPluginManager::GetInstance().RegisterPlugin(p.id, p.id, p.id, "", "", "", p.load, p.unload);
   }
}

void VPinballLib::Log(VPINBALL_LOG_LEVEL level, const string& message)
{
   switch (level) {
      case VPINBALL_LOG_LEVEL_DEBUG:
         PLOGD << message;
         break;
      case VPINBALL_LOG_LEVEL_INFO:
         PLOGI << message;
         break;
      case VPINBALL_LOG_LEVEL_WARN:
         PLOGW << message;
         break;
      case VPINBALL_LOG_LEVEL_ERROR:
         PLOGE << message;
         break;
   }
}

void VPinballLib::ResetLog()
{
   Logger::GetInstance()->Truncate();
}

int VPinballLib::LoadValueInt(const string& sectionName, const string& key, int defaultValue)
{
   // Use table settings when a table is loaded, otherwise use global settings
   Settings& settings = (g_pplayer && g_pplayer->m_ptable) ? g_pplayer->m_ptable->m_settings : g_pvp->m_settings;

   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
   {
      const auto* existingProp = Settings::GetRegistry().GetProperty(existingId.value());
      if (existingProp->m_type == VPX::Properties::PropertyDef::Type::Enum ||
          existingProp->m_type == VPX::Properties::PropertyDef::Type::Int ||
          existingProp->m_type == VPX::Properties::PropertyDef::Type::Bool)
         return settings.GetInt(existingId.value());

      PLOGW << "LoadValueInt: property " << sectionName << '.' << key << " exists but is not int-compatible type";
      return defaultValue;
   }

   const auto propId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::IntPropertyDef>(sectionName, key, ""s, ""s, true, INT_MIN, INT_MAX, defaultValue));
   return settings.GetInt(propId);
}

void VPinballLib::SaveValueInt(const string& sectionName, const string& key, int value)
{
   // Use table settings when a table is loaded, otherwise use global settings
   Settings& settings = (g_pplayer && g_pplayer->m_ptable) ? g_pplayer->m_ptable->m_settings : g_pvp->m_settings;
   const bool asTableOverride = (g_pplayer && g_pplayer->m_ptable);

   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
      settings.Set(existingId.value(), value, asTableOverride);
   else
      settings.Set(Settings::GetRegistry().Register(std::make_unique<VPX::Properties::IntPropertyDef>(sectionName, key, ""s, ""s, true, INT_MIN, INT_MAX, value)), value, asTableOverride);
   settings.Save();
}

float VPinballLib::LoadValueFloat(const string& sectionName, const string& key, float defaultValue)
{
   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
   {
      const auto* existingProp = Settings::GetRegistry().GetProperty(existingId.value());
      if (existingProp->m_type == VPX::Properties::PropertyDef::Type::Float)
         return g_pvp->m_settings.GetFloat(existingId.value());

      PLOGW << "LoadValueFloat: property " << sectionName << '.' << key << " exists but is not float type";
      return defaultValue;
   }

   const auto propId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::FloatPropertyDef>(sectionName, key, ""s, ""s, true, FLT_MIN, FLT_MAX, 0.f, defaultValue));
   return g_pvp->m_settings.GetFloat(propId);
}

void VPinballLib::SaveValueFloat(const string& sectionName, const string& key, float value)
{
   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
      g_pvp->m_settings.Set(existingId.value(), value, false);
   else
      g_pvp->m_settings.Set(Settings::GetRegistry().Register(std::make_unique<VPX::Properties::FloatPropertyDef>(sectionName, key, ""s, ""s, true, FLT_MIN, FLT_MAX, 0.f, value)), value, false);
   g_pvp->m_settings.Save();
}

string VPinballLib::LoadValueString(const string& sectionName, const string& key, const string& defaultValue)
{
   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
   {
      const auto* existingProp = Settings::GetRegistry().GetProperty(existingId.value());
      if (existingProp->m_type == VPX::Properties::PropertyDef::Type::String)
         return g_pvp->m_settings.GetString(existingId.value());

      PLOGW << "LoadValueString: property " << sectionName << '.' << key << " exists but is not string type";
      return defaultValue;
   }

   const auto propId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::StringPropertyDef>(sectionName, key, ""s, ""s, true, defaultValue));
   return g_pvp->m_settings.GetString(propId);
}

void VPinballLib::SaveValueString(const string& sectionName, const string& key, const string& value)
{
   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
      g_pvp->m_settings.Set(existingId.value(), value, false);
   else
      g_pvp->m_settings.Set(Settings::GetRegistry().Register(std::make_unique<VPX::Properties::StringPropertyDef>(sectionName, key, ""s, ""s, true, value)), value, false);
   g_pvp->m_settings.Save();
}

bool VPinballLib::LoadValueBool(const string& sectionName, const string& key, bool defaultValue)
{
   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
   {
      const auto* existingProp = Settings::GetRegistry().GetProperty(existingId.value());
      if (existingProp->m_type == VPX::Properties::PropertyDef::Type::Bool)
         return g_pvp->m_settings.GetBool(existingId.value());

      PLOGW << "LoadValueBool: property " << sectionName << '.' << key << " exists but is not bool type";
      return defaultValue;
   }

   const auto propId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::BoolPropertyDef>(sectionName, key, ""s, ""s, true, defaultValue));
   return g_pvp->m_settings.GetBool(propId);
}

void VPinballLib::SaveValueBool(const string& sectionName, const string& key, bool value)
{
   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
      g_pvp->m_settings.Set(existingId.value(), value, false);
   else
      g_pvp->m_settings.Set(Settings::GetRegistry().Register(std::make_unique<VPX::Properties::BoolPropertyDef>(sectionName, key, ""s, ""s, true, value)), value, false);
   g_pvp->m_settings.Save();
}

VPINBALL_STATUS VPinballLib::ResetIni()
{
   string iniFilePath = g_pvp->GetPrefPath() + "VPinballX.ini";
   if (!std::filesystem::remove(iniFilePath))
    return VPINBALL_STATUS_FAILURE;

   g_pvp->m_settings.SetIniPath(iniFilePath);
   g_pvp->m_settings.Load(true);
   g_pvp->m_settings.Save();
   return VPINBALL_STATUS_SUCCESS;
}

void VPinballLib::UpdateWebServer()
{
   m_webServer.Update();
}

VPINBALL_STATUS VPinballLib::LoadTable(const string& tablePath)
{
   // Reset cancellation flag before starting
   VPXProgress::Reset();

   VPXProgress progress;
   g_pvp->LoadFileName(tablePath, true, &progress);

   // Check if loading was cancelled
   if (progress.IsCancelled())
   {
      PLOGI.printf("Table loading was cancelled");
      return VPINBALL_STATUS_FAILURE;
   }

   bool success = g_pvp->GetActiveTable() != nullptr;
   return success ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

void VPinballLib::CancelLoading()
{
   PLOGI.printf("CancelLoading called");
   VPXProgress::SetCancelled(true);
}

VPINBALL_STATUS VPinballLib::ExtractTableScript()
{
   CComObject<PinTable>* const pActiveTable = g_pvp->GetActiveTable();
   if (!pActiveTable)
      return VPINBALL_STATUS_FAILURE;

   string tempPath = g_pvp->GetPrefPath() + "temp_script.vbs";
   pActiveTable->m_pcv->SaveToFile(tempPath);

   std::filesystem::path tablePath(pActiveTable->m_filename);
   string vbsFilename = tablePath.stem().string() + ".vbs";

   string destPath = tablePath.parent_path().string() + PATH_SEPARATOR_CHAR + vbsFilename;

   try {
      std::filesystem::copy_file(tempPath, destPath, std::filesystem::copy_options::overwrite_existing);
      std::filesystem::remove(tempPath);
   }
   
   catch (const std::exception& e) {
      PLOGE.printf("Failed to save script file: %s", e.what());
      std::filesystem::remove(tempPath);
      g_pvp->CloseTable(pActiveTable);
      return VPINBALL_STATUS_FAILURE;
   }

   g_pvp->CloseTable(pActiveTable);

   return VPINBALL_STATUS_SUCCESS;
}

VPINBALL_STATUS VPinballLib::Play()
{
   if (m_gameLoop)
      return VPINBALL_STATUS_FAILURE;

   CComObject<PinTable>* const pActiveTable = g_pvp->GetActiveTable();
   if (!pActiveTable)
      return VPINBALL_STATUS_FAILURE;

   return SDL_RunOnMainThread([](void*) { g_pvp->DoPlay(0); }, nullptr, true)
       ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

VPINBALL_STATUS VPinballLib::Stop()
{
   CComObject<PinTable>* const pActiveTable = g_pvp->GetActiveTable();
   if (!pActiveTable)
      return VPINBALL_STATUS_FAILURE;

   pActiveTable->QuitPlayer(Player::CS_CLOSE_APP);

   return VPINBALL_STATUS_SUCCESS;
}

VPINBALL_VIEW_MODE VPinballLib::GetViewMode()
{
   // Use player's table if playing, otherwise use editor's table
   PinTable* pTable = (g_pplayer && g_pplayer->m_ptable) ? g_pplayer->m_ptable : (g_pvp ? g_pvp->GetActiveTable() : nullptr);
   if (!pTable)
      return VPINBALL_VIEW_MODE_DESKTOP;

   switch (pTable->GetViewMode())
   {
   case BG_DESKTOP: return VPINBALL_VIEW_MODE_DESKTOP;
   case BG_FSS: return VPINBALL_VIEW_MODE_FSS;
   case BG_FULLSCREEN: return VPINBALL_VIEW_MODE_CABINET;
   default: return VPINBALL_VIEW_MODE_DESKTOP;
   }
}

VPINBALL_STATUS VPinballLib::SetViewMode(VPINBALL_VIEW_MODE mode)
{
   // Must have an active player with a table
   if (!g_pplayer || !g_pplayer->m_ptable)
      return VPINBALL_STATUS_FAILURE;

   ViewSetupID viewMode;
   switch (mode)
   {
   case VPINBALL_VIEW_MODE_DESKTOP: viewMode = BG_DESKTOP; break;
   case VPINBALL_VIEW_MODE_FSS: viewMode = BG_FSS; break;
   case VPINBALL_VIEW_MODE_CABINET: viewMode = BG_FULLSCREEN; break;
   default: return VPINBALL_STATUS_FAILURE;
   }

   // Must run on main thread for renderer operations
   return SDL_RunOnMainThread([](void* userdata) {
      ViewSetupID vm = *static_cast<ViewSetupID*>(userdata);
      // Use player's table - this is what the in-game UI does
      if (g_pplayer && g_pplayer->m_ptable)
      {
         g_pplayer->m_ptable->SetViewSetupOverride(vm);
         if (g_pplayer->m_renderer)
         {
            // Disable static pre-pass, recalculate layout, then re-enable
            g_pplayer->m_renderer->DisableStaticPrePass(true);
            g_pplayer->m_renderer->InitLayout();
            // Re-enable static pre-pass so DT image renders again
            g_pplayer->m_renderer->DisableStaticPrePass(false);
         }
      }
   }, &viewMode, true) ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

VPINBALL_STATUS VPinballLib::CycleViewMode()
{
   // Must have an active player with a table
   if (!g_pplayer || !g_pplayer->m_ptable)
      return VPINBALL_STATUS_FAILURE;

   // Must run on main thread for renderer operations
   return SDL_RunOnMainThread([](void*) {
      // Use player's table - this is what the in-game UI does
      if (!g_pplayer || !g_pplayer->m_ptable)
         return;

      ViewSetupID current = g_pplayer->m_ptable->GetViewMode();
      ViewSetupID next;
      switch (current)
      {
      case BG_DESKTOP: next = BG_FSS; break;
      case BG_FSS: next = BG_FULLSCREEN; break;
      case BG_FULLSCREEN: next = BG_DESKTOP; break;
      default: next = BG_DESKTOP; break;
      }

      g_pplayer->m_ptable->SetViewSetupOverride(next);
      if (g_pplayer->m_renderer)
      {
         // Disable static pre-pass, recalculate layout, then re-enable
         g_pplayer->m_renderer->DisableStaticPrePass(true);
         g_pplayer->m_renderer->InitLayout();
         // Re-enable static pre-pass so DT image renders again
         g_pplayer->m_renderer->DisableStaticPrePass(false);
      }
   }, nullptr, true) ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

VPINBALL_STATUS VPinballLib::ToggleScoreView(bool enable, int x, int y, int width, int height)
{
   // Must have an active player with a table
   if (!g_pplayer || !g_pplayer->m_ptable)
      return VPINBALL_STATUS_FAILURE;

   struct ScoreViewParams {
      bool enable;
      int x, y, width, height;
   };
   ScoreViewParams params = { enable, x, y, width, height };

   // Must run on main thread for renderer operations
   return SDL_RunOnMainThread([](void* userdata) {
      ScoreViewParams* p = static_cast<ScoreViewParams*>(userdata);
      if (!g_pplayer || !g_pplayer->m_ptable)
         return;

      if (p->enable)
      {
         // Enable ScoreView in embedded mode
         g_pplayer->m_scoreViewOutput.SetMode(g_pplayer->m_ptable->m_settings, VPX::RenderOutput::OM_EMBEDDED);
         g_pplayer->m_scoreViewOutput.SetPos(p->x, p->y);
         g_pplayer->m_scoreViewOutput.SetWidth(p->width);
         g_pplayer->m_scoreViewOutput.SetHeight(p->height);
      }
      else
      {
         // Disable ScoreView
         g_pplayer->m_scoreViewOutput.SetMode(g_pplayer->m_ptable->m_settings, VPX::RenderOutput::OM_DISABLED);
      }
   }, &params, true) ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

VPINBALL_STATUS VPinballLib::GetScoreViewSourceSize(int& width, int& height)
{
   // Get PUP video source dimensions (set by PUP plugin during render)
   GetPUPVideoSourceSize(width, height);
   return VPINBALL_STATUS_SUCCESS;
}

}

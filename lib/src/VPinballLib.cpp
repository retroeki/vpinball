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

#ifdef ENABLE_BGFX
#include "bgfx/bgfx.h"
#endif

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

// Global storage for app's internal files path (set from Android/Java side)
static std::string g_internalFilesPath;
static std::mutex g_internalFilesPathMutex;

extern "C" void VPinballSetInternalPath(const char* path)
{
   std::lock_guard<std::mutex> lock(g_internalFilesPathMutex);
   g_internalFilesPath = path ? path : "";
   PLOGI.printf("VPinballLib: Internal path set to: %s", g_internalFilesPath.c_str());
}

extern "C" const char* VPinballGetInternalPath()
{
   std::lock_guard<std::mutex> lock(g_internalFilesPathMutex);
   return g_internalFilesPath.empty() ? nullptr : g_internalFilesPath.c_str();
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
   // Check if we're suspended (surface destroyed, app in background)
   if (m_suspended.load()) {
      // Signal that we've acknowledged the suspend request
      if (!m_suspendAcknowledged.load()) {
         PLOGI << "AppIterate: Acknowledging suspend request";

         // Discard any pending bgfx draw calls so nothing new is submitted.
         // IMPORTANT: Do NOT call bgfx::frame() here. frame() presents to the Vulkan
         // swapchain, and Android may already be invalidating the surface during onPause,
         // causing a SIGSEGV.
#ifdef ENABLE_BGFX
         if (g_pplayer && g_pplayer->m_renderer && g_pplayer->m_renderer->m_renderDevice) {
            PLOGI << "AppIterate: Discarding pending bgfx draw calls";
            bgfx::discard();
            PLOGI << "AppIterate: bgfx draw calls discarded";
         }
#endif

         // Wait for bgfx's internal render thread to finish presenting the last game frame.
         // Without this delay, VPinballPause() returns immediately, onPause() completes,
         // and Android destroys the Vulkan surface while in-flight presentation is still
         // pending in the driver — causing SIGSEGV in vulkan.adreno.so.
         // This sleep delays the acknowledge, which delays VPinballPause() return on the
         // main thread, which delays surface destruction, giving the GPU time to finish.
         PLOGI << "AppIterate: Waiting for GPU to finish last frame presentation";
         std::this_thread::sleep_for(std::chrono::milliseconds(100));
         PLOGI << "AppIterate: GPU wait complete";

         m_suspendAcknowledged.store(true);
         m_suspendCV.notify_all();
      }
      // Don't render while suspended - the surface may be invalid
      return;
   }

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

      PLOGI.printf("AppIterate: Closing table after player exit: %s", pActiveTable->m_filename.c_str());
      g_pvp->CloseTable(pActiveTable);
      PLOGI.printf("AppIterate: Table closed successfully");
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

void VPinballLib::InitHeadless(VPinballEventCallback callback)
{
   SetEventCallback(callback);

   // Same initialization as Init() but runs directly without SDL_RunOnMainThread
   // Used when called from a Service context where SDL event loop isn't running

   g_pvp = new ::VPinball();
   g_pvp->SetLogicalNumberOfProcessors(SDL_GetNumLogicalCPUCores());
   g_pvp->m_settings.SetIniPath(g_pvp->GetPrefPath() + "VPinballX.ini");
   g_pvp->m_settings.Load(true);
   g_pvp->m_settings.SetVersion_VPinball(string(VP_VERSION_STRING_DIGITS), false);
   g_pvp->m_settings.Save();

   Logger::GetInstance()->Init();
   Logger::GetInstance()->SetupLogger(true);

   PLOGI << "VPX Headless - " << VP_VERSION_STRING_FULL_LITERAL;
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
      if (LoadValueBool("Plugin."s + plugin->m_id, "Enable", false))
         plugin->Load(&MsgPI::MsgPluginManager::GetInstance().GetMsgAPI());
   }

   UpdateWebServer();
   m_initialized = true;
}

void VPinballLib::UpdateEventCallback(VPinballEventCallback callback)
{
   PLOGI.printf("UpdateEventCallback called");
   SetEventCallback(callback);
}

void VPinballLib::Shutdown()
{
   PLOGI.printf("Shutdown called - cleaning up all state");

   // Stop player FIRST if running (player references table)
   if (g_pplayer) {
      PLOGI.printf("Stopping player");
      delete g_pplayer;
      g_pplayer = nullptr;
   }

   // Close any active table AFTER player is stopped
   if (g_pvp) {
      CComObject<PinTable>* pActiveTable = g_pvp->GetActiveTable();
      if (pActiveTable) {
         PLOGI.printf("Closing active table: %s", pActiveTable->m_filename.c_str());
         g_pvp->CloseTable(pActiveTable);
      }
   }

   // Unload all loaded plugins (only unload if actually loaded)
   PLOGI.printf("Unloading plugins");
   for (const auto& plugin : MsgPI::MsgPluginManager::GetInstance().GetPlugins()) {
      if (plugin->IsLoaded()) {
         PLOGI.printf("Unloading plugin: %s", plugin->m_id.c_str());
         plugin->Unload();
      }
   }

   // Delete VPinball instance
   if (g_pvp) {
      PLOGI.printf("Deleting VPinball instance");
      delete g_pvp;
      g_pvp = nullptr;
   }

   // Clear callback and state
   m_eventCallback = nullptr;
   m_gameLoop = nullptr;
   m_initialized = false;

   PLOGI.printf("Shutdown complete");
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
            case VPINBALL_EVENT_SCRIPT_ERROR:
            case VPINBALL_EVENT_FATAL_ERROR: {
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
   // Use table settings when a table is loaded, otherwise use global settings
   Settings& settings = (g_pplayer && g_pplayer->m_ptable) ? g_pplayer->m_ptable->m_settings : g_pvp->m_settings;
   const bool asTableOverride = (g_pplayer && g_pplayer->m_ptable);

   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
      settings.Set(existingId.value(), value, asTableOverride);
   else
      settings.Set(Settings::GetRegistry().Register(std::make_unique<VPX::Properties::FloatPropertyDef>(sectionName, key, ""s, ""s, true, FLT_MIN, FLT_MAX, 0.f, value)), value, asTableOverride);
   settings.Save();
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
   // Use table settings when a table is loaded, otherwise use global settings
   Settings& settings = (g_pplayer && g_pplayer->m_ptable) ? g_pplayer->m_ptable->m_settings : g_pvp->m_settings;
   const bool asTableOverride = (g_pplayer && g_pplayer->m_ptable);

   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
      settings.Set(existingId.value(), value, asTableOverride);
   else
      settings.Set(Settings::GetRegistry().Register(std::make_unique<VPX::Properties::StringPropertyDef>(sectionName, key, ""s, ""s, true, value)), value, asTableOverride);
   settings.Save();
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
   // Use table settings when a table is loaded, otherwise use global settings
   Settings& settings = (g_pplayer && g_pplayer->m_ptable) ? g_pplayer->m_ptable->m_settings : g_pvp->m_settings;
   const bool asTableOverride = (g_pplayer && g_pplayer->m_ptable);

   if (const auto existingId = Settings::GetRegistry().GetPropertyId(sectionName, key); existingId.has_value())
      settings.Set(existingId.value(), value, asTableOverride);
   else
      settings.Set(Settings::GetRegistry().Register(std::make_unique<VPX::Properties::BoolPropertyDef>(sectionName, key, ""s, ""s, true, value)), value, asTableOverride);
   settings.Save();
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

VPINBALL_STATUS VPinballLib::ResetTableIni()
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return VPINBALL_STATUS_FAILURE;

   // Get the table INI file path and delete it
   const string iniFilePath = g_pplayer->m_ptable->GetSettingsFileName();
   if (!iniFilePath.empty())
      std::filesystem::remove(iniFilePath);

   // Reset in-memory table settings (clears all overrides, falls back to global defaults)
   g_pplayer->m_ptable->m_settings.Reset();
   g_pplayer->m_ptable->m_settings.Load(false);

   // Reset scene lighting back to Table mode (SetEmissionScale forces User mode)
   if (g_pplayer->m_renderer)
   {
      g_pplayer->m_renderer->m_sceneLighting.SetMode(Renderer::SceneLighting::Mode::Table);
      g_pplayer->m_renderer->DisableStaticPrePass(true);
      g_pplayer->m_renderer->MarkShaderDirty();
   }

   return VPINBALL_STATUS_SUCCESS;
}

void VPinballLib::UpdateWebServer()
{
   m_webServer.Update();
}

VPINBALL_STATUS VPinballLib::LoadTable(const string& tablePath)
{
   PLOGI.printf("LoadTable called with path: %s", tablePath.c_str());

   // Close any existing table before loading a new one
   // This prevents the race condition where a previous table is still active
   CComObject<PinTable>* existingTable = g_pvp->GetActiveTable();
   if (existingTable)
   {
      PLOGI.printf("Closing existing table before loading new one: %s", existingTable->m_filename.c_str());
      g_pvp->CloseTable(existingTable);
   }

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

   // Verify the correct table was loaded (not just any table being active)
   CComObject<PinTable>* loadedTable = g_pvp->GetActiveTable();
   if (!loadedTable)
   {
      PLOGE.printf("Table load failed - no active table");
      return VPINBALL_STATUS_FAILURE;
   }

   // Check that the loaded table matches the requested path
   if (loadedTable->m_filename != tablePath)
   {
      PLOGE.printf("Table load mismatch! Requested: %s, Got: %s", tablePath.c_str(), loadedTable->m_filename.c_str());
      // Close the wrong table
      g_pvp->CloseTable(loadedTable);
      return VPINBALL_STATUS_FAILURE;
   }

   PLOGI.printf("Table loaded successfully: %s", loadedTable->m_filename.c_str());
   return VPINBALL_STATUS_SUCCESS;
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

VPINBALL_STATUS VPinballLib::Pause()
{
   if (!g_pplayer)
      return VPINBALL_STATUS_FAILURE;

   PLOGI << "Pause: Setting play state to false and suspending render loop";

   // First pause the game logic
   g_pplayer->SetPlayState(false);

   // Now suspend the render loop and wait for it to stop
   // This is critical for Android - we must stop rendering BEFORE the surface is destroyed
   m_suspendAcknowledged.store(false);
   m_suspended.store(true);

   // Wait for the render loop to acknowledge the suspend (with timeout)
   {
      std::unique_lock<std::mutex> lock(m_suspendMutex);
      bool acknowledged = m_suspendCV.wait_for(lock, std::chrono::milliseconds(500), [this]() {
         return m_suspendAcknowledged.load();
      });
      if (acknowledged) {
         PLOGI << "Pause: Render loop acknowledged suspend";
      } else {
         PLOGW << "Pause: Timeout waiting for render loop to suspend (may already be idle)";
      }
   }

   return VPINBALL_STATUS_SUCCESS;
}

VPINBALL_STATUS VPinballLib::Resume()
{
   if (!g_pplayer)
      return VPINBALL_STATUS_FAILURE;

   PLOGI << "Resume: Clearing suspend and resuming play state";

   // First clear the suspend flag so the render loop can run again
   m_suspended.store(false);
   m_suspendAcknowledged.store(false);

   // Then resume the game logic
   g_pplayer->SetPlayState(true);

   return VPINBALL_STATUS_SUCCESS;
}

VPINBALL_STATUS VPinballLib::SoftPause()
{
   if (!g_pplayer)
      return VPINBALL_STATUS_FAILURE;

   PLOGI << "SoftPause: Pausing game logic only (rendering continues)";

   // Only pause the game logic - keep rendering so live settings preview works
   g_pplayer->SetPlayState(false);

   return VPINBALL_STATUS_SUCCESS;
}

VPINBALL_STATUS VPinballLib::SoftResume()
{
   if (!g_pplayer)
      return VPINBALL_STATUS_FAILURE;

   PLOGI << "SoftResume: Resuming game logic";

   g_pplayer->SetPlayState(true);

   return VPINBALL_STATUS_SUCCESS;
}

VPINBALL_STATUS VPinballLib::SetBloomStrength(float strength)
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return VPINBALL_STATUS_FAILURE;

   g_pplayer->m_ptable->m_bloom_strength = strength;
   return VPINBALL_STATUS_SUCCESS;
}

float VPinballLib::GetBloomStrength()
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return 1.0f;

   return g_pplayer->m_ptable->m_bloom_strength;
}

VPINBALL_STATUS VPinballLib::SetEmissionScale(float scale)
{
   if (!g_pplayer || !g_pplayer->m_ptable || !g_pplayer->m_renderer)
      return VPINBALL_STATUS_FAILURE;

   // Set the scene lighting mode to User and update the light level
   // This mirrors what the in-game UI does for live updates
   g_pplayer->m_renderer->m_sceneLighting.SetMode(Renderer::SceneLighting::Mode::User);
   g_pplayer->m_renderer->m_sceneLighting.SetUserLightLevel(scale);

   // Also update the table's global emission scale for consistency
   g_pplayer->m_ptable->m_globalEmissionScale = scale;

   // Trigger renderer update (same as MiscSettingsPage::RequestDynamicRendererUpdate)
   g_pplayer->m_renderer->DisableStaticPrePass(true);
   g_pplayer->m_renderer->MarkShaderDirty();

   return VPINBALL_STATUS_SUCCESS;
}

float VPinballLib::GetEmissionScale()
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return 1.0f;

   // Return the scene lighting's current emission scale (the actual rendered value)
   if (g_pplayer->m_renderer)
      return g_pplayer->m_renderer->m_sceneLighting.GetGlobalEmissionScale();

   return g_pplayer->m_ptable->m_globalEmissionScale;
}

VPINBALL_STATUS VPinballLib::SetFOV(float fov)
{
   if (!g_pplayer || !g_pplayer->m_ptable || !g_pplayer->m_renderer)
      return VPINBALL_STATUS_FAILURE;

   // Clamp FOV to reasonable range (10-90 degrees)
   fov = clamp(fov, 10.0f, 90.0f);

   // Update the view setup FOV
   ViewSetup& viewSetup = g_pplayer->m_ptable->GetViewSetup();
   viewSetup.mFOV = fov;

   // Disable static pre-pass for live preview (like the in-game UI does)
   g_pplayer->m_renderer->DisableStaticPrePass(true);
   // Trigger renderer update to apply changes live
   g_pplayer->m_renderer->InitLayout();

   return VPINBALL_STATUS_SUCCESS;
}

float VPinballLib::GetFOV()
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return 45.0f;  // Default 45 degrees

   const ViewSetup& viewSetup = g_pplayer->m_ptable->GetViewSetup();
   return viewSetup.mFOV;
}

VPINBALL_STATUS VPinballLib::SetLookAt(float lookAt)
{
   if (!g_pplayer || !g_pplayer->m_ptable || !g_pplayer->m_renderer)
      return VPINBALL_STATUS_FAILURE;

   // LookAt is a percentage (0-100) - where on table camera looks
   // 0 = bottom (flippers), 100 = top
   lookAt = clamp(lookAt, 0.0f, 100.0f);

   // Update the view setup LookAt
   ViewSetup& viewSetup = g_pplayer->m_ptable->GetViewSetup();
   viewSetup.mLookAt = lookAt;

   // Disable static pre-pass for live preview (like the in-game UI does)
   g_pplayer->m_renderer->DisableStaticPrePass(true);
   // Trigger renderer update to apply changes live
   g_pplayer->m_renderer->InitLayout();

   return VPINBALL_STATUS_SUCCESS;
}

float VPinballLib::GetLookAt()
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return 25.0f;  // Default ~25%

   const ViewSetup& viewSetup = g_pplayer->m_ptable->GetViewSetup();
   return viewSetup.mLookAt;
}

VPINBALL_STATUS VPinballLib::SetLayback(float layback)
{
   if (!g_pplayer || !g_pplayer->m_ptable || !g_pplayer->m_renderer)
      return VPINBALL_STATUS_FAILURE;

   // Layback is -90 to 90 degrees (fake visual stretch for depth effect)
   layback = clamp(layback, -90.0f, 90.0f);

   // Update the view setup Layback
   ViewSetup& viewSetup = g_pplayer->m_ptable->GetViewSetup();
   viewSetup.mLayback = layback;

   // Disable static pre-pass for live preview (like the in-game UI does)
   g_pplayer->m_renderer->DisableStaticPrePass(true);
   // Trigger renderer update to apply changes live
   g_pplayer->m_renderer->InitLayout();

   return VPINBALL_STATUS_SUCCESS;
}

float VPinballLib::GetLayback()
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return 0.0f;  // Default 0 degrees

   const ViewSetup& viewSetup = g_pplayer->m_ptable->GetViewSetup();
   return viewSetup.mLayback;
}

string VPinballLib::GetTableName()
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return "";

   return g_pplayer->m_ptable->m_tableName;
}

string VPinballLib::GetTableVersion()
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return "";

   return g_pplayer->m_ptable->m_version;
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

VPINBALL_STATUS VPinballLib::CaptureScoreView()
{
   if (!g_pplayer || !g_pplayer->m_ptable)
      return VPINBALL_STATUS_FAILURE;

   // Run everything on the main thread so we can safely read/modify ScoreView state,
   // temporarily enable/resize it for a high-quality capture, and request the screenshot.
   return SDL_RunOnMainThread([](void* userdata) {
      auto* self = static_cast<VPinballLib*>(userdata);
      if (!g_pplayer || !g_pplayer->m_renderer || !g_pplayer->m_renderer->m_renderDevice)
         return;

      // If a previous capture is still pending restore, skip to avoid overwriting the
      // original ScoreView state with the temporary capture dimensions
      {
         std::lock_guard<std::mutex> lock(self->m_scoreViewCapture.mutex);
         if (self->m_scoreViewCapture.needsRestore)
            return;
      }

      auto& output = g_pplayer->m_scoreViewOutput;
      const bool wasDisabled = (output.GetMode() != VPX::RenderOutput::OM_EMBEDDED);

      // Save current embedded window state for restoration after capture
      int origX = 0, origY = 0, origW = 0, origH = 0;
      if (!wasDisabled) {
         VPX::EmbeddedWindow* embedWnd = output.GetEmbeddedWindow();
         if (embedWnd) {
            embedWnd->GetPos(origX, origY);
            origW = embedWnd->GetWidth();
            origH = embedWnd->GetHeight();
         }
      }

      // Determine capture size: use PUP source dimensions for best quality, capped at 640px wide
      int srcW = 0, srcH = 0;
      GetPUPVideoSourceSize(srcW, srcH);
      int captureW, captureH;
      if (srcW > 0 && srcH > 0) {
         const float aspect = static_cast<float>(srcW) / static_cast<float>(srcH);
         captureW = std::min(srcW, 640);
         captureH = static_cast<int>(static_cast<float>(captureW) / aspect);
         if (captureH < 1) captureH = 1;
      } else {
         captureW = 512;
         captureH = 128;
      }

      // Temporarily enable/resize ScoreView at position (0,0) for capture
      output.SetMode(g_pplayer->m_ptable->m_settings, VPX::RenderOutput::OM_EMBEDDED);
      output.SetPos(0, 0);
      output.SetWidth(captureW);
      output.SetHeight(captureH);

      // Calculate crop rect from the capture dimensions
      VPX::Window* containerWnd = g_pplayer->m_renderer->m_renderDevice->m_outputWnd[0];
      const float displayScaleX = static_cast<float>(containerWnd->GetPixelWidth()) / static_cast<float>(containerWnd->GetWidth());
      const float displayScaleY = static_cast<float>(containerWnd->GetPixelHeight()) / static_cast<float>(containerWnd->GetHeight());

      {
         std::lock_guard<std::mutex> lock(self->m_scoreViewCapture.mutex);
         self->m_scoreViewCapture.cropX = 0;
         self->m_scoreViewCapture.cropY = 0;
         self->m_scoreViewCapture.cropW = static_cast<int>(static_cast<float>(captureW) * displayScaleX);
         self->m_scoreViewCapture.cropH = static_cast<int>(static_cast<float>(captureH) * displayScaleY);
         self->m_scoreViewCapture.ready = false;
         // Store restore info so DeliverScoreViewCapture can restore original state
         self->m_scoreViewCapture.needsRestore = true;
         self->m_scoreViewCapture.wasDisabled = wasDisabled;
         self->m_scoreViewCapture.restoreX = origX;
         self->m_scoreViewCapture.restoreY = origY;
         self->m_scoreViewCapture.restoreW = origW;
         self->m_scoreViewCapture.restoreH = origH;
      }

      // Request screenshot — the marker filename routes it to DeliverScoreViewCapture.
      // If the request is rejected (another screenshot in progress), restore immediately
      // since DeliverScoreViewCapture won't be called to do it.
      struct FailsafeRestore {
         VPinballLib* lib;
         bool disabled;
         int x, y, w, h;
      };
      auto* fr = new FailsafeRestore{self, wasDisabled, origX, origY, origW, origH};
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot("__scoreview_capture__",
         [fr](bool success) {
            if (!success) {
               PLOGE << "ScoreView capture failed — restoring ScoreView state";
               SDL_RunOnMainThread([](void* userdata) {
                  auto* rp = static_cast<FailsafeRestore*>(userdata);
                  if (g_pplayer && g_pplayer->m_ptable) {
                     if (rp->disabled) {
                        g_pplayer->m_scoreViewOutput.SetMode(g_pplayer->m_ptable->m_settings, VPX::RenderOutput::OM_DISABLED);
                     } else {
                        g_pplayer->m_scoreViewOutput.SetPos(rp->x, rp->y);
                        g_pplayer->m_scoreViewOutput.SetWidth(rp->w);
                        g_pplayer->m_scoreViewOutput.SetHeight(rp->h);
                     }
                  }
                  {
                     std::lock_guard<std::mutex> lock(rp->lib->m_scoreViewCapture.mutex);
                     rp->lib->m_scoreViewCapture.needsRestore = false;
                  }
                  delete rp;
               }, fr, false);
            } else {
               delete fr;
            }
         });
   }, this, true) ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

VPINBALL_STATUS VPinballLib::GetScoreViewCapture(int& width, int& height, uint32_t* pixels, int maxPixels)
{
   if (!m_scoreViewCapture.ready)
      return VPINBALL_STATUS_FAILURE;

   std::lock_guard<std::mutex> lock(m_scoreViewCapture.mutex);
   width = m_scoreViewCapture.capturedWidth;
   height = m_scoreViewCapture.capturedHeight;
   const int totalPixels = width * height;
   if (totalPixels > maxPixels || totalPixels <= 0)
      return VPINBALL_STATUS_FAILURE;

   memcpy(pixels, m_scoreViewCapture.pixels.data(), totalPixels * sizeof(uint32_t));
   m_scoreViewCapture.ready = false;
   return VPINBALL_STATUS_SUCCESS;
}

bool VPinballLib::IsScoreViewCapture(const char* filePath) const
{
   return filePath && strcmp(filePath, "__scoreview_capture__") == 0;
}

void VPinballLib::DeliverScoreViewCapture(const uint32_t* framePixels, uint32_t frameWidth, uint32_t frameHeight, bool yflip, bool swapRB)
{
   std::lock_guard<std::mutex> lock(m_scoreViewCapture.mutex);

   int cx = m_scoreViewCapture.cropX;
   int cy = m_scoreViewCapture.cropY;
   int cw = m_scoreViewCapture.cropW;
   int ch = m_scoreViewCapture.cropH;

   // Clamp crop rect to frame bounds
   if (cx < 0) cx = 0;
   if (cy < 0) cy = 0;
   if (cx + cw > (int)frameWidth) cw = (int)frameWidth - cx;
   if (cy + ch > (int)frameHeight) ch = (int)frameHeight - cy;
   if (cw <= 0 || ch <= 0)
   {
      m_scoreViewCapture.ready = false;
      return;
   }

   m_scoreViewCapture.capturedWidth = cw;
   m_scoreViewCapture.capturedHeight = ch;
   m_scoreViewCapture.pixels.resize(cw * ch);

   for (int row = 0; row < ch; row++)
   {
      // Handle Y-flip: if yflip, row 0 in crop maps to bottom of frame
      int srcRow = yflip ? ((int)frameHeight - 1 - (cy + row)) : (cy + row);
      const uint32_t* srcLine = framePixels + srcRow * frameWidth + cx;
      uint32_t* dstLine = m_scoreViewCapture.pixels.data() + row * cw;

      if (swapRB)
      {
         // Swap R and B channels (Metal BGRA → RGBA)
         for (int col = 0; col < cw; col++)
         {
            uint32_t p = srcLine[col];
            uint8_t b = (p >> 0) & 0xFF;
            uint8_t g = (p >> 8) & 0xFF;
            uint8_t r = (p >> 16) & 0xFF;
            uint8_t a = (p >> 24) & 0xFF;
            dstLine[col] = (a << 24) | (b << 16) | (g << 8) | r;
         }
      }
      else
      {
         memcpy(dstLine, srcLine, cw * sizeof(uint32_t));
      }
   }

   m_scoreViewCapture.ready = true;

   // Restore ScoreView to its original state after capture.
   // IMPORTANT: needsRestore is cleared on the main thread (inside the restore callback),
   // NOT here on the render thread. This prevents a race where CaptureScoreView sees
   // needsRestore=false but the ScoreView hasn't actually been restored yet.
   if (m_scoreViewCapture.needsRestore)
   {
      struct RestoreParams {
         VPinballLib* lib;
         bool disabled;
         int x, y, w, h;
      };
      auto* rp = new RestoreParams{
         this,
         m_scoreViewCapture.wasDisabled,
         m_scoreViewCapture.restoreX, m_scoreViewCapture.restoreY,
         m_scoreViewCapture.restoreW, m_scoreViewCapture.restoreH
      };
      SDL_RunOnMainThread([](void* userdata) {
         auto* rp = static_cast<RestoreParams*>(userdata);
         if (g_pplayer && g_pplayer->m_ptable) {
            if (rp->disabled) {
               g_pplayer->m_scoreViewOutput.SetMode(g_pplayer->m_ptable->m_settings, VPX::RenderOutput::OM_DISABLED);
            } else {
               g_pplayer->m_scoreViewOutput.SetPos(rp->x, rp->y);
               g_pplayer->m_scoreViewOutput.SetWidth(rp->w);
               g_pplayer->m_scoreViewOutput.SetHeight(rp->h);
            }
         }
         // Clear needsRestore AFTER the restore is applied, so CaptureScoreView
         // won't start a new capture while the ScoreView is still in capture state
         {
            std::lock_guard<std::mutex> lock(rp->lib->m_scoreViewCapture.mutex);
            rp->lib->m_scoreViewCapture.needsRestore = false;
         }
         delete rp;
      }, rp, false);  // Don't wait — we're on the render thread
   }
}

VPINBALL_STATUS VPinballLib::ResetTable()
{
   if (!g_pplayer)
      return VPINBALL_STATUS_FAILURE;

   // Trigger the Reset action by simulating a key press
   // This is the same action that would be triggered by pressing F3 or the mapped reset key
   auto& inputActions = g_pplayer->m_pininput.GetInputActions();
   unsigned int resetActionId = g_pplayer->m_pininput.GetResetActionId();

   if (resetActionId >= inputActions.size())
      return VPINBALL_STATUS_FAILURE;

   // Create a direct state slot and trigger press/release
   int slot = inputActions[resetActionId]->NewDirectStateSlot();
   inputActions[resetActionId]->SetDirectState(slot, true);
   inputActions[resetActionId]->SetDirectState(slot, false);

   PLOGI.printf("ResetTable: Reset action triggered");
   return VPINBALL_STATUS_SUCCESS;
}

}

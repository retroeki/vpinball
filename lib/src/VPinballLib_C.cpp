// license:GPLv3+

#include <core/stdafx.h>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

#include "../include/vpinball/VPinballLib_C.h"
#include "VPinballLib.h"
#include "plugins/MsgPluginManager.h"

#include <dlfcn.h>  // VPinballGenerateNVRAM: dlopen libpinmame.so for headless NVRAM gen

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
   return VPinballLib::VPinballLib::Instance().AppInit(argc, argv) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
   VPinballLib::VPinballLib::Instance().AppIterate();
   return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
   VPinballLib::VPinballLib::Instance().AppEvent(event);
   return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
}

VPINBALLAPI const char* VPinballGetVersionStringFull()
{
   thread_local string version;
   version = VPinballLib::VPinballLib::Instance().GetVersionStringFull();
   return version.c_str();
}

VPINBALLAPI void VPinballInit(VPinballEventCallback callback)
{
   VPinballLib::VPinballLib::Instance().Init(callback);
}

VPINBALLAPI void VPinballInitHeadless(VPinballEventCallback callback)
{
   VPinballLib::VPinballLib::Instance().InitHeadless(callback);
}

VPINBALLAPI void VPinballUpdateEventCallback(VPinballEventCallback callback)
{
   VPinballLib::VPinballLib::Instance().UpdateEventCallback(callback);
}

VPINBALLAPI void VPinballShutdown()
{
   VPinballLib::VPinballLib::Instance().Shutdown();
}

VPINBALLAPI int VPinballIsInitialized()
{
   return VPinballLib::VPinballLib::Instance().IsInitialized() ? 1 : 0;
}

VPINBALLAPI void VPinballLog(VPINBALL_LOG_LEVEL level, const char* pMessage)
{
   if (pMessage != nullptr)
      VPinballLib::VPinballLib::Instance().Log(level, pMessage);
}

VPINBALLAPI void VPinballResetLog()
{
   VPinballLib::VPinballLib::Instance().ResetLog();
}

VPINBALLAPI int VPinballLoadValueInt(const char* pSectionName, const char* pKey, int defaultValue)
{
    if (pSectionName == nullptr || pKey == nullptr)
      return defaultValue;

   return VPinballLib::VPinballLib::Instance().LoadValueInt(pSectionName, pKey, defaultValue);
}

VPINBALLAPI void VPinballSaveValueInt(const char* pSectionName, const char* pKey, int value)
{
   if (pSectionName == nullptr || pKey == nullptr)
      return;

   VPinballLib::VPinballLib::Instance().SaveValueInt(pSectionName, pKey, value);
}

VPINBALLAPI float VPinballLoadValueFloat(const char* pSectionName, const char* pKey, float defaultValue)
{
   if (pSectionName == nullptr || pKey == nullptr)
      return defaultValue;

   return VPinballLib::VPinballLib::Instance().LoadValueFloat(pSectionName, pKey, defaultValue);
}

VPINBALLAPI void VPinballSaveValueFloat(const char* pSectionName, const char* pKey, float value)
{
   if (pSectionName == nullptr || pKey == nullptr)
      return;

   VPinballLib::VPinballLib::Instance().SaveValueFloat(pSectionName, pKey, value);
}

VPINBALLAPI const char* VPinballLoadValueString(const char* pSectionName, const char* pKey, const char* pDefaultValue)
{
   if (pSectionName == nullptr || pKey == nullptr)
      return pDefaultValue;

   thread_local string value;
   value = VPinballLib::VPinballLib::Instance().LoadValueString(pSectionName, pKey, pDefaultValue);
   return value.c_str();
}

VPINBALLAPI void VPinballSaveValueString(const char* pSectionName, const char* pKey, const char* pValue)
{
   if (pSectionName == nullptr || pKey == nullptr || pValue == nullptr)
      return;

   VPinballLib::VPinballLib::Instance().SaveValueString(pSectionName, pKey, pValue);
}

VPINBALLAPI int VPinballLoadValueBool(const char* pSectionName, const char* pKey, int defaultValue)
{
   if (pSectionName == nullptr || pKey == nullptr)
      return defaultValue;

   return VPinballLib::VPinballLib::Instance().LoadValueBool(pSectionName, pKey, defaultValue != 0) ? 1 : 0;
}

VPINBALLAPI void VPinballSaveValueBool(const char* pSectionName, const char* pKey, int value)
{
   if (pSectionName == nullptr || pKey == nullptr)
      return;

   VPinballLib::VPinballLib::Instance().SaveValueBool(pSectionName, pKey, value != 0);
}

VPINBALLAPI VPINBALL_STATUS VPinballResetIni()
{
   return VPinballLib::VPinballLib::Instance().ResetIni();
}

VPINBALLAPI VPINBALL_STATUS VPinballResetTableIni()
{
   return VPinballLib::VPinballLib::Instance().ResetTableIni();
}

VPINBALLAPI void VPinballUpdateWebServer()
{
   VPinballLib::VPinballLib::Instance().UpdateWebServer();
}

VPINBALLAPI void VPinballRefreshWebServer()
{
   VPinballLib::VPinballLib::Instance().RefreshWebServer();
}

VPINBALLAPI VPINBALL_STATUS VPinballLoadTable(const char* pPath)
{
   if (pPath == nullptr)
      return VPINBALL_STATUS_FAILURE;

   return VPinballLib::VPinballLib::Instance().LoadTable(pPath);
}

VPINBALLAPI void VPinballCancelLoading()
{
   VPinballLib::VPinballLib::Instance().CancelLoading();
}

VPINBALLAPI VPINBALL_STATUS VPinballExtractTableScript()
{
   return VPinballLib::VPinballLib::Instance().ExtractTableScript();
}

VPINBALLAPI VPINBALL_STATUS VPinballPlay()
{
   return VPinballLib::VPinballLib::Instance().Play();
}

VPINBALLAPI VPINBALL_STATUS VPinballStop()
{
   return VPinballLib::VPinballLib::Instance().Stop();
}

VPINBALLAPI VPINBALL_STATUS VPinballPause()
{
   return VPinballLib::VPinballLib::Instance().Pause();
}

VPINBALLAPI VPINBALL_STATUS VPinballResume()
{
   return VPinballLib::VPinballLib::Instance().Resume();
}

VPINBALLAPI VPINBALL_STATUS VPinballSoftPause()
{
   return VPinballLib::VPinballLib::Instance().SoftPause();
}

VPINBALLAPI VPINBALL_STATUS VPinballSoftResume()
{
   return VPinballLib::VPinballLib::Instance().SoftResume();
}

VPINBALLAPI VPINBALL_STATUS VPinballPushKeyEvent(int scancode, int pressed)
{
   return VPinballLib::VPinballLib::Instance().PushKeyEvent(scancode, pressed != 0);
}

VPINBALLAPI VPINBALL_STATUS VPinballSetThermalStatus(int status)
{
   return VPinballLib::VPinballLib::Instance().SetThermalStatus(status);
}

VPINBALLAPI VPINBALL_STATUS VPinballSetBatteryTempC(float tempC)
{
   return VPinballLib::VPinballLib::Instance().SetBatteryTempC(tempC);
}

VPINBALLAPI VPINBALL_STATUS VPinballSetBloomStrength(float strength)
{
   return VPinballLib::VPinballLib::Instance().SetBloomStrength(strength);
}

VPINBALLAPI float VPinballGetBloomStrength()
{
   return VPinballLib::VPinballLib::Instance().GetBloomStrength();
}

VPINBALLAPI VPINBALL_STATUS VPinballSetUserCGBrightness(float v)  { return VPinballLib::VPinballLib::Instance().SetUserCGBrightness(v); }
VPINBALLAPI VPINBALL_STATUS VPinballSetUserCGContrast(float v)    { return VPinballLib::VPinballLib::Instance().SetUserCGContrast(v); }
VPINBALLAPI VPINBALL_STATUS VPinballSetUserCGSaturation(float v)  { return VPinballLib::VPinballLib::Instance().SetUserCGSaturation(v); }
VPINBALLAPI VPINBALL_STATUS VPinballSetUserCGTemperature(float v) { return VPinballLib::VPinballLib::Instance().SetUserCGTemperature(v); }
VPINBALLAPI float VPinballGetUserCGBrightness()  { return VPinballLib::VPinballLib::Instance().GetUserCGBrightness(); }
VPINBALLAPI float VPinballGetUserCGContrast()    { return VPinballLib::VPinballLib::Instance().GetUserCGContrast(); }
VPINBALLAPI float VPinballGetUserCGSaturation()  { return VPinballLib::VPinballLib::Instance().GetUserCGSaturation(); }
VPINBALLAPI float VPinballGetUserCGTemperature() { return VPinballLib::VPinballLib::Instance().GetUserCGTemperature(); }

VPINBALLAPI VPINBALL_STATUS VPinballSetEmissionScale(float scale)
{
   return VPinballLib::VPinballLib::Instance().SetEmissionScale(scale);
}

VPINBALLAPI float VPinballGetEmissionScale()
{
   return VPinballLib::VPinballLib::Instance().GetEmissionScale();
}

VPINBALLAPI VPINBALL_STATUS VPinballSetFOV(float fov)
{
   return VPinballLib::VPinballLib::Instance().SetFOV(fov);
}

VPINBALLAPI float VPinballGetFOV()
{
   return VPinballLib::VPinballLib::Instance().GetFOV();
}

VPINBALLAPI VPINBALL_STATUS VPinballSetLookAt(float lookAt)
{
   return VPinballLib::VPinballLib::Instance().SetLookAt(lookAt);
}

VPINBALLAPI float VPinballGetLookAt()
{
   return VPinballLib::VPinballLib::Instance().GetLookAt();
}

VPINBALLAPI VPINBALL_STATUS VPinballSetLayback(float layback)
{
   return VPinballLib::VPinballLib::Instance().SetLayback(layback);
}

VPINBALLAPI float VPinballGetLayback()
{
   return VPinballLib::VPinballLib::Instance().GetLayback();
}

VPINBALLAPI VPINBALL_STATUS VPinballSetViewX(float x)
{
   return VPinballLib::VPinballLib::Instance().SetViewX(x);
}

VPINBALLAPI float VPinballGetViewX()
{
   return VPinballLib::VPinballLib::Instance().GetViewX();
}

VPINBALLAPI VPINBALL_STATUS VPinballSetViewY(float y)
{
   return VPinballLib::VPinballLib::Instance().SetViewY(y);
}

VPINBALLAPI float VPinballGetViewY()
{
   return VPinballLib::VPinballLib::Instance().GetViewY();
}

VPINBALLAPI VPINBALL_STATUS VPinballSetViewZ(float z)
{
   return VPinballLib::VPinballLib::Instance().SetViewZ(z);
}

VPINBALLAPI float VPinballGetViewZ()
{
   return VPinballLib::VPinballLib::Instance().GetViewZ();
}

VPINBALLAPI VPINBALL_VIEW_MODE VPinballGetViewMode()
{
   return VPinballLib::VPinballLib::Instance().GetViewMode();
}

VPINBALLAPI VPINBALL_STATUS VPinballSetViewMode(VPINBALL_VIEW_MODE mode)
{
   return VPinballLib::VPinballLib::Instance().SetViewMode(mode);
}

VPINBALLAPI VPINBALL_STATUS VPinballCycleViewMode()
{
   return VPinballLib::VPinballLib::Instance().CycleViewMode();
}

VPINBALLAPI VPINBALL_STATUS VPinballToggleScoreView(bool enable, int x, int y, int width, int height)
{
   return VPinballLib::VPinballLib::Instance().ToggleScoreView(enable, x, y, width, height);
}

VPINBALLAPI VPINBALL_STATUS VPinballGetScoreViewSourceSize(int* width, int* height)
{
   int w = 0, h = 0;
   VPINBALL_STATUS status = VPinballLib::VPinballLib::Instance().GetScoreViewSourceSize(w, h);
   if (width) *width = w;
   if (height) *height = h;
   return status;
}

VPINBALLAPI VPINBALL_STATUS VPinballCaptureScoreView()
{
   return VPinballLib::VPinballLib::Instance().CaptureScoreView();
}

VPINBALLAPI VPINBALL_STATUS VPinballGetScoreViewCapture(int* width, int* height, uint32_t* pixels, int maxPixels)
{
   int w = 0, h = 0;
   VPINBALL_STATUS status = VPinballLib::VPinballLib::Instance().GetScoreViewCapture(w, h, pixels, maxPixels);
   if (width) *width = w;
   if (height) *height = h;
   return status;
}

VPINBALLAPI VPINBALL_STATUS VPinballSetMusicVolume(int volume)
{
   if (!g_pplayer)
      return VPINBALL_STATUS_FAILURE;

   int clampedVolume = clamp(volume, 0, 100);
   return SDL_RunOnMainThread([](void* userdata) {
      int vol = *static_cast<int*>(userdata);
      if (g_pplayer) {
         g_pplayer->m_MusicVolume = vol;
         g_pplayer->UpdateVolume();
      }
   }, &clampedVolume, true) ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

VPINBALLAPI VPINBALL_STATUS VPinballSetSoundVolume(int volume)
{
   if (!g_pplayer)
      return VPINBALL_STATUS_FAILURE;

   int clampedVolume = clamp(volume, 0, 100);
   return SDL_RunOnMainThread([](void* userdata) {
      int vol = *static_cast<int*>(userdata);
      if (g_pplayer) {
         g_pplayer->m_SoundVolume = vol;
         g_pplayer->UpdateVolume();
      }
   }, &clampedVolume, true) ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

VPINBALLAPI int VPinballGetMusicVolume()
{
   if (!g_pplayer)
      return 100;
   return g_pplayer->m_MusicVolume;
}

VPINBALLAPI int VPinballGetSoundVolume()
{
   if (!g_pplayer)
      return 100;
   return g_pplayer->m_SoundVolume;
}

VPINBALLAPI VPINBALL_STATUS VPinballSetPinmameVolume(int volume)
{
   if (!g_pplayer)
      return VPINBALL_STATUS_FAILURE;

   int clampedVolume = clamp(volume, -32, 32);
   return SDL_RunOnMainThread([](void* userdata) {
      int vol = *static_cast<int*>(userdata);
      if (g_pplayer) {
         g_pplayer->m_PinmameVolume = vol;
         g_pplayer->UpdateVolume();
      }
   }, &clampedVolume, true) ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

VPINBALLAPI int VPinballGetPinmameVolume()
{
   if (!g_pplayer)
      return 0;
   return g_pplayer->m_PinmameVolume;
}

VPINBALLAPI const char* VPinballGetTableName()
{
   thread_local string tableName;
   tableName = VPinballLib::VPinballLib::Instance().GetTableName();
   return tableName.c_str();
}

VPINBALLAPI const char* VPinballGetTableVersion()
{
   thread_local string tableVersion;
   tableVersion = VPinballLib::VPinballLib::Instance().GetTableVersion();
   return tableVersion.c_str();
}

// Payload shape must match the PinMAME plugin's PinMAMENvramQuery struct.
namespace {
struct VPinballNvramQuery {
   uint8_t* buffer;
   int maxBytes;
   int bytesWritten;
   int isNvramTable;
};
}

VPINBALLAPI int VPinballGetNVRAM(uint8_t* buffer, int maxBytes, int* isNvramTable)
{
   if (isNvramTable) *isNvramTable = 0;
   if (!buffer || maxBytes <= 0) return 0;
   auto& msgApi = MsgPI::MsgPluginManager::GetInstance().GetMsgAPI();
   const unsigned int msgId = msgApi.GetMsgID("VPINBALL", "GET_NVRAM");
   VPinballNvramQuery q { buffer, maxBytes, 0, 0 };
   msgApi.BroadcastMsg(0, msgId, &q);
   msgApi.ReleaseMsgID(msgId);
   if (isNvramTable) *isNvramTable = q.isNvramTable;
   return q.bytesWritten;
}

// ---------------------------------------------------------------------------
// Headless first-run NVRAM generation
//
// PinMAME (libpinmame.so) owns NVRAM: it loads <rom>.nv at the start of a run
// and writes it back when the emulation thread stops. We can therefore create a
// missing <rom>.nv WITHOUT the VPX player, renderer, SDL surface or B2S — by
// running the ROM headlessly here. libpinmame ships as its own .so reached only
// through the PinMAME plugin during play; we dlopen it so libvpinball's link
// graph is unchanged. The structs/enums below MIRROR libpinmame.h exactly and
// must stay in sync (ABI): see src/libpinmame/libpinmame.h.
// ---------------------------------------------------------------------------
namespace {

constexpr int PM_MAX_PATH = 512;          // PINMAME_MAX_PATH
constexpr int PM_CORE_MAXNVRAM = 131118;  // CORE_MAXNVRAM (wpc/core.h)

// Mirrors PinmameConfig. NOTE: vpmPath is an INLINE char[PINMAME_MAX_PATH], not a
// pointer, and all callbacks are pointer-sized — keep this layout exact.
struct PmConfig {
   int   audioFormat;        // PINMAME_AUDIO_FORMAT_INT16 = 0
   int   sampleRate;
   char  vpmPath[PM_MAX_PATH];
   void* cb_OnStateUpdated;
   void* cb_OnDisplayAvailable;
   void* cb_OnDisplayUpdated;
   void* cb_OnAudioAvailable;
   void* cb_OnAudioUpdated;
   void* cb_OnMechAvailable;
   void* cb_OnMechUpdated;
   void* cb_OnSolenoidUpdated;
   void* cb_OnConsoleDataUpdated;
   void* cb_IsKeyPressed;
   void* cb_OnLogMessage;
   void* cb_OnSoundCommand;
};

// Mirrors PinmameNVRAMState: { int nvramNo; uint8_t oldStat; uint8_t currStat; }
struct PmNvramState {
   int     nvramNo;
   uint8_t oldStat;
   uint8_t currStat;
};

typedef void (*pm_setconfig_t)(const PmConfig*);
typedef int  (*pm_run_t)(const char*);   // returns PINMAME_STATUS; OK == 0
typedef void (*pm_stop_t)();
typedef int  (*pm_isrunning_t)();
typedef int  (*pm_changednvram_t)(PmNvramState*);

// Forward libpinmame's internal logs to logcat during headless generation
// (otherwise a ROM-load failure / stall is invisible). Matches
// PinmameOnLogMessageCallback: (PINMAME_LOG_LEVEL, const char*, va_list, void*).
static void PmGenLog(int logLevel, const char* format, va_list args, void* /*userData*/)
{
   char buf[1024];
   vsnprintf(buf, sizeof(buf), format, args);
   if (logLevel >= 2) PLOGE.printf("[pinmame-gen] %s", buf);
   else               PLOGI.printf("[pinmame-gen] %s", buf);
}

} // namespace

VPINBALLAPI int VPinballGenerateNVRAM(const char* romName, const char* pinmamePath, int maxSeconds)
{
   if (!romName || !pinmamePath)
      return 0;
   if (maxSeconds <= 0)
      maxSeconds = 30;

   void* lib = dlopen("libpinmame.so", RTLD_NOW | RTLD_GLOBAL);
   if (!lib) {
      PLOGE.printf("VPinballGenerateNVRAM: dlopen(libpinmame.so) failed: %s", dlerror());
      return 0;
   }

   auto SetConfig    = (pm_setconfig_t)   dlsym(lib, "PinmameSetConfig");
   auto Run          = (pm_run_t)         dlsym(lib, "PinmameRun");
   auto Stop         = (pm_stop_t)        dlsym(lib, "PinmameStop");
   auto IsRunning    = (pm_isrunning_t)   dlsym(lib, "PinmameIsRunning");
   auto ChangedNVRAM = (pm_changednvram_t)dlsym(lib, "PinmameGetChangedNVRAM");
   if (!SetConfig || !Run || !Stop) {
      PLOGE.printf("VPinballGenerateNVRAM: missing libpinmame symbols");
      dlclose(lib);
      return 0;
   }

   if (IsRunning && IsRunning()) {
      PLOGE.printf("VPinballGenerateNVRAM: PinMAME already running; aborting (rom=%s)", romName);
      dlclose(lib);
      return 0;
   }

   PmConfig cfg;
   memset(&cfg, 0, sizeof(cfg));          // callbacks NULL — headless, state pulled not pushed
   cfg.audioFormat = 0;                   // INT16
   cfg.sampleRate  = 48000;
   cfg.cb_OnLogMessage = (void*)&PmGenLog;  // surface ROM-load / boot logs to logcat
   strncpy(cfg.vpmPath, pinmamePath, PM_MAX_PATH - 1);
   // libpinmame composes <vpmPath>/roms, <vpmPath>/nvram — match the plugin's
   // trailing-separator path so ROM/NVRAM dirs resolve identically.
   {
      size_t len = strlen(cfg.vpmPath);
      if (len > 0 && len < (size_t)(PM_MAX_PATH - 1) && cfg.vpmPath[len - 1] != '/') {
         cfg.vpmPath[len] = '/';
         cfg.vpmPath[len + 1] = '\0';
      }
   }
   SetConfig(&cfg);

   PLOGI.printf("VPinballGenerateNVRAM: headless boot rom=%s path=%s maxSeconds=%d", romName, pinmamePath, maxSeconds);
   const int status = Run(romName);
   if (status != 0) {                     // != PINMAME_STATUS_OK
      PLOGE.printf("VPinballGenerateNVRAM: PinmameRun failed status=%d rom=%s", status, romName);
      Stop();                             // no-op if no game thread
      dlclose(lib);
      // GAME_NOT_FOUND (2): the rom name isn't a real PinMAME driver (e.g. a vpx
      // cGameName that doesn't map to a romset) — permanent, so signal the caller
      // to mark it no-NVRAM and never retry. Other failures (config/already-running)
      // are transient: return 0 so the caller retries next launch.
      return (status == 2) ? 2 : 0;
   }

   // Completion heuristic (adaptive, not a fixed timer). PinmameGetChangedNVRAM
   // returns the number of NVRAM bytes that changed since the last poll, or -1 when
   // the game has no nvram_handler / NVRAM isn't available yet.
   //
   // The valid <rom>.nv is written during the ROM's boot RAM-clear + factory-default
   // init, which shows up as ONE big burst (observed: ~1560 bytes for Jurassic Park,
   // ~521 for Space Station). After that the game runs (attract / a long mech
   // diagnostic) and keeps writing working-RAM that PinMAME dumps as NVRAM — JP sits
   // at ~40-65 bytes/poll for as long as it runs. Those steady writes are NOT init,
   // so we must NOT wait for them to stop (JP never goes quiet). Instead we anchor on
   // the burst: stop a grace period after the LAST big-burst write (> burstThreshold,
   // set well above any runtime spike). A quiet-settle path still covers small/quiet
   // ROMs that have no big burst, a no-NVRAM probe bails ROMs with no handler, and
   // maxSeconds is a final runaway backstop. Per-second counts are logged for tuning.
   PmNvramState* buf = ChangedNVRAM
      ? (PmNvramState*)malloc(sizeof(PmNvramState) * (size_t)PM_CORE_MAXNVRAM)
      : nullptr;
   const int pollMs = 250;
   const int minRunMs = 8000;          // never stop before this
   const int burstThreshold = 256;     // > this many changed bytes/poll = init burst (runtime stays well below)
   const int postBurstGraceMs = 8000;  // stop this long after the last init-burst write
   const int settleWindowMs = 5000;    // (fallback) writes <= settledThreshold this long => settled
   const int settledThreshold = 32;    // (fallback) quiet level for ROMs with no big burst
   const int noNvramProbeMs = 18000;   // NVRAM never seen by here => ROM has no handler
   int elapsedMs = 0;
   int settledMs = 0;
   int logAccumMs = 0;
   int peak = 0;
   int lastChanged = -1;
   int lastBurstMs = -1;               // elapsed at the last poll exceeding burstThreshold
   bool sawNvram = false;
   bool noNvram = false;
   while (elapsedMs < maxSeconds * 1000) {
      SDL_Delay(pollMs);
      elapsedMs += pollMs;
      // PinmameRun flips the running flag to 2 ("Starting") synchronously, BEFORE the
      // game thread has loaded the ROM and built the machine's memory map. PinMAME's
      // own guard is only `if (!_isRunning)`, which passes while state==2, so polling
      // NVRAM in that window calls the ROM's nvram_handler against not-yet-initialized
      // emulated memory and segfaults — reliably on large/slow romsets (e.g. Stern SAM
      // like acd_170h, whose ~99MB load keeps state==2 well past the first 250ms poll).
      // Only poll once FULLY started (IsRunning(): 0=stopped, 1=started, 2=starting).
      // If the symbol is missing, fall back to the old unconditional poll.
      int changed = -1;
      const bool started = !IsRunning || IsRunning() == 1;
      if (started && buf && ChangedNVRAM)
         changed = ChangedNVRAM(buf);
      lastChanged = changed;
      if (changed >= 0) {
         sawNvram = true;
         if (changed > peak) peak = changed;
         if (changed > burstThreshold) lastBurstMs = elapsedMs;
         if (changed <= settledThreshold) settledMs += pollMs; else settledMs = 0;
      }
      logAccumMs += pollMs;
      if (logAccumMs >= 1000) {
         logAccumMs = 0;
         PLOGI.printf("VPinballGenerateNVRAM: t=%ds changed=%d peak=%d lastBurstMs=%d settledMs=%d",
                      elapsedMs / 1000, lastChanged, peak, lastBurstMs, settledMs);
      }
      // No NVRAM ever appeared -> ROM has no nvram_handler; don't wait out the cap.
      if (!sawNvram && elapsedMs >= noNvramProbeMs) {
         noNvram = true;
         break;
      }
      if (elapsedMs >= minRunMs) {
         // Primary: saw the big init burst and a grace has passed since the last one.
         if (lastBurstMs >= 0 && (elapsedMs - lastBurstMs) >= postBurstGraceMs)
            break;
         // Fallback (no big burst, e.g. small NVRAM): writes went quiet for the window.
         if (sawNvram && lastBurstMs < 0 && settledMs >= settleWindowMs)
            break;
      }
   }
   if (buf)
      free(buf);

   PLOGI.printf("VPinballGenerateNVRAM: stopping after %dms (sawNvram=%d noNvram=%d peak=%d lastBurstMs=%d) rom=%s",
                elapsedMs, (int)sawNvram, (int)noNvram, peak, lastBurstMs, romName);
   Stop();                                // joins emulation thread -> flushes <rom>.nv
   dlclose(lib);
   return 1;
}

VPINBALLAPI VPINBALL_STATUS VPinballResetTable()
{
   return VPinballLib::VPinballLib::Instance().ResetTable();
}

// Payload shape must match the PinMAME plugin's PinMAMESwitchSet struct.
namespace {
struct VPinballSwitchSet {
   int switchNum;
   int state;
   int handled;
};
}

VPINBALLAPI VPINBALL_STATUS VPinballSetSwitch(int switchNum, int state)
{
   auto& msgApi = MsgPI::MsgPluginManager::GetInstance().GetMsgAPI();
   const unsigned int msgId = msgApi.GetMsgID("VPINBALL", "SET_SWITCH");
   VPinballSwitchSet s { switchNum, state, 0 };
   msgApi.BroadcastMsg(0, msgId, &s);
   msgApi.ReleaseMsgID(msgId);
   return s.handled ? VPINBALL_STATUS_SUCCESS : VPINBALL_STATUS_FAILURE;
}

// Payload shape must match the PinMAME plugin's PinMAMESwitchGet struct.
namespace {
struct VPinballSwitchGet {
   int switchNum;
   int value;
   int handled;
};
}

VPINBALLAPI int VPinballGetSwitch(int switchNum)
{
   auto& msgApi = MsgPI::MsgPluginManager::GetInstance().GetMsgAPI();
   const unsigned int msgId = msgApi.GetMsgID("VPINBALL", "GET_SWITCH");
   VPinballSwitchGet g { switchNum, 0, 0 };
   msgApi.BroadcastMsg(0, msgId, &g);
   msgApi.ReleaseMsgID(msgId);
   return g.handled ? g.value : -1;
}


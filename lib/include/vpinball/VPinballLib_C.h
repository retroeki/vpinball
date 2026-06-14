// license:GPLv3+

#pragma once

#ifdef _MSC_VER
#define VPINBALLAPI extern "C" __declspec(dllexport)
#define VPINBALLCALLBACK __stdcall
#else
#define VPINBALLAPI extern "C" __attribute__((visibility("default")))
#define VPINBALLCALLBACK
#endif

// Enums

typedef enum {
   VPINBALL_LOG_LEVEL_DEBUG,
   VPINBALL_LOG_LEVEL_INFO,
   VPINBALL_LOG_LEVEL_WARN,
   VPINBALL_LOG_LEVEL_ERROR
} VPINBALL_LOG_LEVEL;

typedef enum {
   VPINBALL_STATUS_SUCCESS,
   VPINBALL_STATUS_FAILURE
} VPINBALL_STATUS;

typedef enum {
   VPINBALL_SCRIPT_ERROR_TYPE_COMPILE,
   VPINBALL_SCRIPT_ERROR_TYPE_RUNTIME
} VPINBALL_SCRIPT_ERROR_TYPE;

typedef enum {
   VPINBALL_VIEW_MODE_DESKTOP = 0,
   VPINBALL_VIEW_MODE_FSS = 1,
   VPINBALL_VIEW_MODE_CABINET = 2
} VPINBALL_VIEW_MODE;

typedef enum {
   VPINBALL_EVENT_LOADING_ITEMS,
   VPINBALL_EVENT_LOADING_SOUNDS,
   VPINBALL_EVENT_LOADING_IMAGES,
   VPINBALL_EVENT_LOADING_FONTS,
   VPINBALL_EVENT_LOADING_COLLECTIONS,
   VPINBALL_EVENT_PRERENDERING,
   VPINBALL_EVENT_PLAYER_STARTED,
   VPINBALL_EVENT_RUMBLE,
   VPINBALL_EVENT_SCRIPT_ERROR,
   VPINBALL_EVENT_PLAYER_CLOSED,
   VPINBALL_EVENT_WEB_SERVER,
   VPINBALL_EVENT_COMMAND,
   VPINBALL_EVENT_MENU_PRESSED,
   VPINBALL_EVENT_FATAL_ERROR, // Unrecoverable error (GPU/shader/crash) - player will close after this
   // Per-chunk web-server upload progress. Fires once per /upload POST so
   // hosts can render an in-flight indicator on the matching library row.
   VPINBALL_EVENT_WEB_UPLOAD
} VPINBALL_EVENT;

// Callbacks

typedef void (*VPinballEventCallback)(VPINBALL_EVENT, const char*);

// Functions

VPINBALLAPI const char* VPinballGetVersionStringFull();
VPINBALLAPI void VPinballInit(VPinballEventCallback callback);
VPINBALLAPI void VPinballInitHeadless(VPinballEventCallback callback);  // Init without SDL main thread (for services)
VPINBALLAPI void VPinballUpdateEventCallback(VPinballEventCallback callback);  // Update callback without reinit
VPINBALLAPI void VPinballShutdown();  // Clean up all state for fresh reinit
VPINBALLAPI int VPinballIsInitialized();  // Check if already initialized
VPINBALLAPI void VPinballLog(VPINBALL_LOG_LEVEL level, const char* message);
VPINBALLAPI void VPinballResetLog();

// Settings

VPINBALLAPI int VPinballLoadValueInt(const char* pSectionName, const char* pKey, int defaultValue);
VPINBALLAPI void VPinballSaveValueInt(const char* pSectionName, const char* pKey, int value);
VPINBALLAPI float VPinballLoadValueFloat(const char* pSectionName, const char* pKey, float defaultValue);
VPINBALLAPI void VPinballSaveValueFloat(const char* pSectionName, const char* pKey, float value);
VPINBALLAPI const char* VPinballLoadValueString(const char* pSectionName, const char* pKey, const char* pDefaultValue);
VPINBALLAPI void VPinballSaveValueString(const char* pSectionName, const char* pKey, const char* pValue);
VPINBALLAPI int VPinballLoadValueBool(const char* pSectionName, const char* pKey, int defaultValue);
VPINBALLAPI void VPinballSaveValueBool(const char* pSectionName, const char* pKey, int value);
VPINBALLAPI VPINBALL_STATUS VPinballResetIni();
VPINBALLAPI VPINBALL_STATUS VPinballResetTableIni();

// Web Server

VPINBALLAPI void VPinballUpdateWebServer();
VPINBALLAPI void VPinballRefreshWebServer();

// Player

VPINBALLAPI VPINBALL_STATUS VPinballLoadTable(const char* pPath);
VPINBALLAPI void VPinballCancelLoading();
VPINBALLAPI VPINBALL_STATUS VPinballExtractTableScript();
VPINBALLAPI VPINBALL_STATUS VPinballPlay();
VPINBALLAPI VPINBALL_STATUS VPinballStop();
VPINBALLAPI VPINBALL_STATUS VPinballPause();
VPINBALLAPI VPINBALL_STATUS VPinballResume();
VPINBALLAPI VPINBALL_STATUS VPinballSoftPause();   // Pause game logic only, keep rendering
VPINBALLAPI VPINBALL_STATUS VPinballSoftResume();
// Direct-path key event injection — bypasses SDL's event queue so every call
// lands deterministically. scancode is an SDL_Scancode value (e.g. 36 for '7',
// 77 for End). pressed: non-zero = key down, zero = key up.
VPINBALLAPI VPINBALL_STATUS VPinballPushKeyEvent(int scancode, int pressed);
VPINBALLAPI VPINBALL_STATUS VPinballSetThermalStatus(int status);
VPINBALLAPI VPINBALL_STATUS VPinballSetBatteryTempC(float tempC);
VPINBALLAPI VPINBALL_STATUS VPinballSetBloomStrength(float strength);
VPINBALLAPI float VPinballGetBloomStrength();
// Playfield reflection quality (0=Off, 1=Balls Only, 2=Static, 3=Static & Balls,
// 4=Static & Unsynced Dynamic, 5=Dynamic). Applies live to the running player.
VPINBALLAPI VPINBALL_STATUS VPinballSetReflectionMode(int mode);
VPINBALLAPI int VPinballGetReflectionMode();
VPINBALLAPI VPINBALL_STATUS VPinballSetUserCGBrightness(float v);
VPINBALLAPI VPINBALL_STATUS VPinballSetUserCGContrast(float v);
VPINBALLAPI VPINBALL_STATUS VPinballSetUserCGSaturation(float v);
VPINBALLAPI VPINBALL_STATUS VPinballSetUserCGTemperature(float v);
VPINBALLAPI float VPinballGetUserCGBrightness();
VPINBALLAPI float VPinballGetUserCGContrast();
VPINBALLAPI float VPinballGetUserCGSaturation();
VPINBALLAPI float VPinballGetUserCGTemperature();
VPINBALLAPI VPINBALL_STATUS VPinballSetEmissionScale(float scale);
VPINBALLAPI float VPinballGetEmissionScale();
VPINBALLAPI VPINBALL_STATUS VPinballSetFOV(float fov);  // Field of View (10-90 degrees, default 45)
VPINBALLAPI float VPinballGetFOV();
VPINBALLAPI VPINBALL_STATUS VPinballSetLookAt(float lookAt);  // Where camera looks on table (0-100%, 0=flippers, 100=top)
VPINBALLAPI float VPinballGetLookAt();
VPINBALLAPI VPINBALL_STATUS VPinballSetLayback(float layback);  // Layback distortion (-90 to 90 degrees, default 0)
VPINBALLAPI float VPinballGetLayback();

// Camera Position (in centimeters, relative to table bottom center)
VPINBALLAPI VPINBALL_STATUS VPinballSetViewX(float x);   // Camera X position (cm, default 0)
VPINBALLAPI float VPinballGetViewX();
VPINBALLAPI VPINBALL_STATUS VPinballSetViewY(float y);   // Camera Y position (cm, default 20)
VPINBALLAPI float VPinballGetViewY();
VPINBALLAPI VPINBALL_STATUS VPinballSetViewZ(float z);   // Camera Z / height (cm, default 70)
VPINBALLAPI float VPinballGetViewZ();

// Table Info

VPINBALLAPI const char* VPinballGetTableName();
VPINBALLAPI const char* VPinballGetTableVersion();

// NVRAM (PinMAME-emulated ROM tables only). Reads the running emulator's
// in-memory NVRAM. Returns the number of bytes written to `buffer`.
//
//   buffer       caller-provided buffer
//   maxBytes     size of `buffer`
//   isNvramTable [out] 1 if a PinMAME Controller is currently running (ROM
//                table); 0 for EM / original VPX-only tables.
//
// Returns 0 when isNvramTable=0 or when no bytes are available yet.
VPINBALLAPI int VPinballGetNVRAM(uint8_t* buffer, int maxBytes, int* isNvramTable);

// First-run NVRAM generation (PinMAME ROM tables). Boots the ROM HEADLESSLY via
// libpinmame (no player / renderer / SDL surface / B2S) to create <rom>.nv when
// it is missing, so the first real launch is immediately playable instead of
// coming up in a factory-reset / no-credits state. romName is the PinMAME game
// name; pinmamePath is the directory containing roms/ and nvram/. Blocks until
// the ROM's NVRAM has initialized (or maxSeconds elapses), then stops the
// emulator (which flushes <rom>.nv). Call OFF the UI thread. Returns 1 if the
// emulator ran, 0 on failure (libpinmame or ROM not found, or already running),
// 2 if the ROM name is not a real PinMAME driver (GAME_NOT_FOUND), or 3 if the
// romset is incomplete/corrupt (required ROM files missing — see
// VPinballGetLastRomLoadError).
VPINBALLAPI int VPinballGenerateNVRAM(const char* romName, const char* pinmamePath, int maxSeconds);

// Comma-joined list of ROM files PinMAME reported missing during the most recent
// VPinballGenerateNVRAM boot (empty string if none). Valid until the next
// VPinballGenerateNVRAM call. Diagnostics for a return value of 3.
VPINBALLAPI const char* VPinballGetLastRomLoadError();

// View Mode

VPINBALLAPI VPINBALL_VIEW_MODE VPinballGetViewMode();
VPINBALLAPI VPINBALL_STATUS VPinballSetViewMode(VPINBALL_VIEW_MODE mode);
VPINBALLAPI VPINBALL_STATUS VPinballCycleViewMode();
VPINBALLAPI VPINBALL_STATUS VPinballToggleScoreView(bool enable, int x, int y, int width, int height);
VPINBALLAPI VPINBALL_STATUS VPinballGetScoreViewSourceSize(int* width, int* height);

// ScoreView capture (async: call Capture, then poll Get until it returns SUCCESS)
VPINBALLAPI VPINBALL_STATUS VPinballCaptureScoreView();
VPINBALLAPI VPINBALL_STATUS VPinballGetScoreViewCapture(int* width, int* height, uint32_t* pixels, int maxPixels);

// Audio Volume (0-100)

VPINBALLAPI VPINBALL_STATUS VPinballSetMusicVolume(int volume);
VPINBALLAPI VPINBALL_STATUS VPinballSetSoundVolume(int volume);
VPINBALLAPI int VPinballGetMusicVolume();
VPINBALLAPI int VPinballGetSoundVolume();
VPINBALLAPI VPINBALL_STATUS VPinballSetPinmameVolume(int volume);
VPINBALLAPI int VPinballGetPinmameVolume();

// Table control
VPINBALLAPI VPINBALL_STATUS VPinballResetTable();

// Direct PinMAME switch write. Bypasses the keyboard / SDL path so the host
// can drive switches as level signals (e.g. coin-door latch) regardless of
// the table script's toggleKeyCoinDoor / momentary mode. state: 0 = inactive,
// non-zero = active. Returns FAILURE when no PinMAME Controller is currently
// running (EM / original VPX-only tables).
VPINBALLAPI VPINBALL_STATUS VPinballSetSwitch(int switchNum, int state);

// Direct PinMAME switch read. Returns 0 or 1 for the live matrix bit (after
// per-table polarity inversion applied by core_getSw). Returns -1 if no
// Controller is currently running. Used by the host to seed UI state — e.g.
// reading sw 22 at Service-screen entry to know whether the coin door is
// open or closed in the ROM's view.
VPINBALLAPI int VPinballGetSwitch(int switchNum);


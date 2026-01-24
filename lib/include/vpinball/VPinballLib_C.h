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
   VPINBALL_EVENT_FATAL_ERROR  // Unrecoverable error (GPU/shader/crash) - player will close after this
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

// Web Server

VPINBALLAPI void VPinballUpdateWebServer();

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
VPINBALLAPI VPINBALL_STATUS VPinballSetBloomStrength(float strength);
VPINBALLAPI float VPinballGetBloomStrength();
VPINBALLAPI VPINBALL_STATUS VPinballSetEmissionScale(float scale);
VPINBALLAPI float VPinballGetEmissionScale();

// View Mode

VPINBALLAPI VPINBALL_VIEW_MODE VPinballGetViewMode();
VPINBALLAPI VPINBALL_STATUS VPinballSetViewMode(VPINBALL_VIEW_MODE mode);
VPINBALLAPI VPINBALL_STATUS VPinballCycleViewMode();
VPINBALLAPI VPINBALL_STATUS VPinballToggleScoreView(bool enable, int x, int y, int width, int height);
VPINBALLAPI VPINBALL_STATUS VPinballGetScoreViewSourceSize(int* width, int* height);

// Audio Volume (0-100)

VPINBALLAPI VPINBALL_STATUS VPinballSetMusicVolume(int volume);
VPINBALLAPI VPINBALL_STATUS VPinballSetSoundVolume(int volume);
VPINBALLAPI int VPinballGetMusicVolume();
VPINBALLAPI int VPinballGetSoundVolume();


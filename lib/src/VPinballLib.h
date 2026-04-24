// license:GPLv3+

#pragma once

#include <string>
#include <functional>
#include <queue>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include "../include/vpinball/VPinballLib_C.h"
#include "WebServer.h"
#include "core/vpversion.h"

namespace VPinballLib {

using std::string;
using std::vector;

struct ProgressData {
   int progress;
};

struct RumbleData {
   uint16_t lowFrequencyRumble;
   uint16_t highFrequencyRumble;
   uint32_t durationMs;
};

struct ScriptErrorData {
   VPINBALL_SCRIPT_ERROR_TYPE error;
   int line;
   int position;
   string description;
};

struct WebServerData {
   string url;
};

struct CommandData {
   string command;
   string data;
};

class VPinballLib
{
public:
   static VPinballLib& Instance()
   {
      static VPinballLib inst;
      return inst;
   }

   int AppInit(int argc, char** argv);
   void AppIterate();
   void AppEvent(SDL_Event* event);
   bool PollAppEvent(SDL_Event& event);
   SDL_Window* GetWindow() { return m_pWindow; }
#ifdef __APPLE__
   void* GetMetalLayer() { return m_pMetalLayer; }
   void SetMetalLayer(void* layer) { m_pMetalLayer = layer; }
#endif
   string GetVersionStringFull() { return VP_VERSION_STRING_FULL_LITERAL; };
   void Init(VPinballEventCallback callback);
   void InitHeadless(VPinballEventCallback callback);  // Init without SDL main thread
   void UpdateEventCallback(VPinballEventCallback callback);  // Update callback without reinit
   void Shutdown();  // Clean up all state for fresh reinit
   bool IsInitialized() const { return m_initialized; }
   static void SendEvent(VPINBALL_EVENT event, void* data);
   void Log(VPINBALL_LOG_LEVEL level, const string& message);
   void ResetLog();
   int LoadValueInt(const string& sectionName, const string& key, int defaultValue);
   void SaveValueInt(const string& sectionName, const string& key, int value);
   float LoadValueFloat(const string& sectionName, const string& key, float defaultValue);
   void SaveValueFloat(const string& sectionName, const string& key, float value);
   string LoadValueString(const string& sectionName, const string& key, const string& defaultValue);
   void SaveValueString(const string& sectionName, const string& key, const string& value);
   bool LoadValueBool(const string& sectionName, const string& key, bool defaultValue);
   void SaveValueBool(const string& sectionName, const string& key, bool value);
   VPINBALL_STATUS ResetIni();
   VPINBALL_STATUS ResetTableIni();
   void UpdateWebServer();
   VPINBALL_STATUS LoadTable(const string& tablePath);
   void CancelLoading();
   VPINBALL_STATUS ExtractTableScript();
   VPINBALL_STATUS Play();
   VPINBALL_STATUS Stop();
   VPINBALL_STATUS Pause();
   VPINBALL_STATUS Resume();
   VPINBALL_STATUS SoftPause();   // Pause game logic only, keep rendering for live settings preview
   VPINBALL_STATUS SoftResume();
   // Direct-path key event injection. Used by the Android in-game service-menu
   // overlay: the synthetic-key round-trip through SDL's event queue + our own
   // m_eventQueue + the game-loop pump is lossy under load (observed as "some
   // service taps register, some don't" on Data East DMD ROMs), and this
   // bypasses all of it by calling InputManager::PushButtonEvent directly —
   // exactly what SDLInputHandler does for a real keypress.
   // scancode is SDL_Scancode (SDL_SCANCODE_7 = 36, _8 = 37, _END = 77, etc.)
   VPINBALL_STATUS PushKeyEvent(int scancode, bool pressed);
   // Called from the Android PowerManager.OnThermalStatusChangedListener so the stats overlay can surface the current thermal state.
   VPINBALL_STATUS SetThermalStatus(int status);
   // Called from Android's ACTION_BATTERY_CHANGED receiver (EXTRA_TEMPERATURE / 10).
   VPINBALL_STATUS SetBatteryTempC(float tempC);
   VPINBALL_STATUS SetBloomStrength(float strength);
   float GetBloomStrength();
   VPINBALL_STATUS SetEmissionScale(float scale);
   float GetEmissionScale();
   VPINBALL_STATUS SetFOV(float fov);
   float GetFOV();
   VPINBALL_STATUS SetLookAt(float lookAt);
   float GetLookAt();
   VPINBALL_STATUS SetLayback(float layback);
   float GetLayback();
   VPINBALL_STATUS SetViewX(float x);   // Camera X position in cm
   float GetViewX();
   VPINBALL_STATUS SetViewY(float y);   // Camera Y position in cm
   float GetViewY();
   VPINBALL_STATUS SetViewZ(float z);   // Camera Z position in cm
   float GetViewZ();
   string GetTableName();
   string GetTableVersion();
   VPINBALL_VIEW_MODE GetViewMode();
   VPINBALL_STATUS SetViewMode(VPINBALL_VIEW_MODE mode);
   VPINBALL_STATUS CycleViewMode();
   VPINBALL_STATUS ToggleScoreView(bool enable, int x, int y, int width, int height);
   VPINBALL_STATUS GetScoreViewSourceSize(int& width, int& height);
   VPINBALL_STATUS CaptureScoreView();
   VPINBALL_STATUS GetScoreViewCapture(int& width, int& height, uint32_t* pixels, int maxPixels);

   // Called by screenShot() callback to check if this is a ScoreView capture request
   bool IsScoreViewCapture(const char* filePath) const;
   // Called by screenShot() callback to deliver cropped ScoreView pixels
   void DeliverScoreViewCapture(const uint32_t* framePixels, uint32_t frameWidth, uint32_t frameHeight, bool yflip, bool swapRB);

   VPINBALL_STATUS ResetTable();
   void SetGameLoop(std::function<void()> gameLoop) { m_gameLoop = gameLoop; }

private:
   VPinballLib();
   ~VPinballLib();
   VPinballLib(const VPinballLib&) = delete;
   VPinballLib& operator=(const VPinballLib&) = delete;
   void SetEventCallback(VPinballEventCallback callback);
   static void RegisterStaticPlugins();

   SDL_Window* m_pWindow = nullptr;
#ifdef __APPLE__
   void* m_pMetalLayer = nullptr;
#endif
   WebServer m_webServer;
   std::function<void*(VPINBALL_EVENT, void*)> m_eventCallback = nullptr;
   std::function<void()> m_gameLoop = nullptr;
   std::queue<SDL_Event> m_eventQueue;
   std::mutex m_eventMutex;
   bool m_captureInProgress = false;
   bool m_initialized = false;

   // ScoreView capture state
   struct {
      std::atomic<bool> requested{false};
      std::atomic<bool> ready{false};
      int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
      std::vector<uint32_t> pixels;
      int capturedWidth = 0, capturedHeight = 0;
      std::mutex mutex;
      // State restoration after capture (ScoreView is temporarily resized for quality)
      bool needsRestore = false;
      bool wasDisabled = false;
      int restoreX = 0, restoreY = 0, restoreW = 0, restoreH = 0;
   } m_scoreViewCapture;

   // Suspend/resume synchronization for safe surface lifecycle handling
   std::atomic<bool> m_suspended{false};           // Request to suspend rendering
   std::atomic<bool> m_suspendAcknowledged{false}; // Render loop has acknowledged suspend
   std::mutex m_suspendMutex;
   std::condition_variable m_suspendCV;
};

}

// license:GPLv3+

#include <cstdlib>
#include <chrono>
#include <cstring>
#include <charconv>
#include <algorithm>
#include <vector>

#include "plugins/VPXPlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/LoggingPlugin.h"

// Host reel-image channel (lib/src/VPinballLib.cpp, same statically linked
// module; ScoreView.cpp consumes it the same way). ReelDmd
// composites EM score-reel artwork into a tightly packed w*h*3 sRGB image;
// returns false when no reel image is active. EM tables publish no DMD
// display source, so this is the only way their score reaches external DMDs.
extern "C" bool GetReelImage(int* width, int* height, uint64_t* version, std::vector<uint8_t>* out);

#pragma warning(push)
#pragma warning(disable : 4251) // xxx needs dll-interface
#include "DMDUtil/DMDUtil.h"
#pragma warning(pop)

#define DMDUTIL_TINT_R 255
#define DMDUTIL_TINT_G 140
#define DMDUTIL_TINT_B 0

using namespace std;

namespace DMDUtilPlugin {
   
static const MsgPluginAPI* msgApi = nullptr;
static uint32_t endpointId;

static unsigned int onGameStartId;
static unsigned int onGameEndId;
static unsigned int onDmdSrcChangedId;
static unsigned int getDmdSrcMsgId;

static std::mutex sourceMutex;
static std::thread updateThread;
static DisplaySrcId selectedDmdId = {};
static bool isRunning = false;

static DMDUtil::DMD* pDmd = nullptr;

static uint8_t tintR;
static uint8_t tintG;
static uint8_t tintB;

MSGPI_BOOL_VAL_SETTING(zeDMDProp, "ZeDMD", "ZeDMD", "", true, true);
MSGPI_STRING_VAL_SETTING(zeDMDDeviceFolderProp, "ZeDMDDevice", "ZeDMDDevice", "", true, "", 1024);
MSGPI_BOOL_VAL_SETTING(zeDMDDebugFolderProp, "ZeDMDDebug", "ZeDMDDebug", "", true, false);
MSGPI_INT_VAL_SETTING(zeDMDBrightnessFolderProp, "ZeDMDBrightness", "ZeDMDBrightness", "", true, -1, 1000, -1);
MSGPI_BOOL_VAL_SETTING(zeDMDWifiProp, "ZeDMDWiFi", "ZeDMDWiFi", "", true, false);
MSGPI_STRING_VAL_SETTING(zeDMDWiFiAddrFolderProp, "ZeDMDWiFiAddr", "ZeDMDWiFiAddr", "", true, "zedmd-wifi.local", 1024);
MSGPI_BOOL_VAL_SETTING(pixelcadeProp, "Pixelcade", "Pixelcade", "", true, true);
MSGPI_STRING_VAL_SETTING(pixelcadeDeviceProp, "PixelcadeDevice", "PixelcadeDevice", "", true, "", 1024);
MSGPI_BOOL_VAL_SETTING(dmdServerFolderProp, "DMDServer", "DMDServer", "", true, false);
MSGPI_STRING_VAL_SETTING(dmdServerAddrFolderProp, "DMDServerAddr", "DMDServerAddr", "", true, "localhost", 1024);
MSGPI_INT_VAL_SETTING(dmdServerPortFolderProp, "DMDServerPort", "DMDServerPort", "", true, 0, 1000, 6789);

MSGPI_BOOL_VAL_SETTING(findDisplaysProp, "FindDisplays", "FindDisplays", "", true, true);
MSGPI_BOOL_VAL_SETTING(dumpDMDTxtProp, "DumpDMDTxt", "DumpDMDTxt", "", true, false);
MSGPI_BOOL_VAL_SETTING(dumpDMDRawProp, "DumpDMDRaw", "DumpDMDRaw", "", true, false);
MSGPI_INT_VAL_SETTING(lumTintRProp, "LumTintR", "LumTintR", "", true, 0, 255, DMDUTIL_TINT_R);
MSGPI_INT_VAL_SETTING(lumTintGProp, "LumTintG", "LumTintG", "", true, 0, 255, DMDUTIL_TINT_G);
MSGPI_INT_VAL_SETTING(lumTintBProp, "LumTintB", "LumTintB", "", true, 0, 255, DMDUTIL_TINT_B);


LPI_USE();
#define LOGD LPI_LOGD
#define LOGI LPI_LOGI
#define LOGW LPI_LOGW
#define LOGE LPI_LOGE

LPI_IMPLEMENT

void DMDUTILCALLBACK OnDMDUtilLog(DMDUtil_LogLevel logLevel, const char* format, va_list args)
{
   va_list args_copy;
   va_copy(args_copy, args);
   int size = vsnprintf(nullptr, 0, format, args_copy);
   va_end(args_copy);
   if (size > 0) {
      char* const buffer = static_cast<char*>(malloc(size + 1));
      vsnprintf(buffer, size + 1, format, args);
      switch(logLevel) {
         case DMDUtil_LogLevel_INFO:
            LOGI("%s", buffer);
            break;
         case DMDUtil_LogLevel_DEBUG:
            LOGD("%s", buffer);
            break;
         case DMDUtil_LogLevel_ERROR:
            LOGE("%s", buffer);
            break;
         default:
            break;
      }
      free(buffer);
   }
}

// Frames pushed through libdmdutil must fit DMD::Update.data (256*64*3) and
// the dmdserver/ZeDMD size caps.
#define DMDUTIL_MAX_FRAME_W 256
#define DMDUTIL_MAX_FRAME_H 64

// Box-average downscale of a tightly packed sRGB888 image to fit within
// maxW x maxH, preserving aspect ratio. Copies through unchanged when the
// source already fits. Reel composites are arbitrarily sized (built from the
// table's real reel-strip artwork) and routinely exceed the DMD frame cap.
static void FitRGB24(const std::vector<uint8_t>& src, const int srcW, const int srcH,
                     std::vector<uint8_t>& dst, int& dstW, int& dstH)
{
   if (srcW <= DMDUTIL_MAX_FRAME_W && srcH <= DMDUTIL_MAX_FRAME_H)
   {
      dst = src;
      dstW = srcW;
      dstH = srcH;
      return;
   }
   const float scale = std::min((float)DMDUTIL_MAX_FRAME_W / srcW, (float)DMDUTIL_MAX_FRAME_H / srcH);
   dstW = std::max(1, (int)(srcW * scale));
   dstH = std::max(1, (int)(srcH * scale));
   dst.assign((size_t)dstW * dstH * 3, 0);
   for (int y = 0; y < dstH; y++)
   {
      const int sy0 = y * srcH / dstH;
      const int sy1 = std::max(sy0 + 1, (y + 1) * srcH / dstH);
      for (int x = 0; x < dstW; x++)
      {
         const int sx0 = x * srcW / dstW;
         const int sx1 = std::max(sx0 + 1, (x + 1) * srcW / dstW);
         unsigned int sum[3] = { 0, 0, 0 };
         for (int sy = sy0; sy < sy1; sy++)
            for (int sx = sx0; sx < sx1; sx++)
            {
               const uint8_t* const p = &src[((size_t)sy * srcW + sx) * 3];
               sum[0] += p[0];
               sum[1] += p[1];
               sum[2] += p[2];
            }
         const unsigned int n = (unsigned int)((sy1 - sy0) * (sx1 - sx0));
         uint8_t* const d = &dst[((size_t)y * dstW + x) * 3];
         d[0] = (uint8_t)(sum[0] / n);
         d[1] = (uint8_t)(sum[1] / n);
         d[2] = (uint8_t)(sum[2] / n);
      }
   }
}

static void RescanSources(const bool quiet);

static void UpdateThread()
{
   // Create the device on this background thread: FindDisplays runs
   // ConnectDMDServer synchronously, and an unreachable dmdserver host blocks
   // on the TCP connect for the full kernel timeout (~2 minutes). Doing this
   // in the game-start broadcast handler froze table loading for that long.
   // Note: a close during such a hang is covered by the app-side process kill.
   if (!pDmd)
   {
      DMDUtil::DMD* const dmd = new DMDUtil::DMD();

      if (findDisplaysProp_Val)
          dmd->FindDisplays();

      if (dumpDMDTxtProp_Val)
          dmd->DumpDMDTxt();

      if (dumpDMDRawProp_Val)
          dmd->DumpDMDRaw();

      tintR = static_cast<uint8_t>(lumTintRProp_Val);
      tintG = static_cast<uint8_t>(lumTintGProp_Val);
      tintB = static_cast<uint8_t>(lumTintBProp_Val);

      pDmd = dmd;
   }

   int lastFrameID = 0;
   uint64_t lastReelVersion = 0;
   int idleTicks = 0;
   std::vector<uint8_t> reelImage, reelScaled;
   while (isRunning && pDmd)
   {
      // Fixed update at 60 FPS
      std::this_thread::sleep_for(std::chrono::microseconds(16666));

      bool needRescan = false;
      {
      std::lock_guard<std::mutex> lock(sourceMutex);

      if (selectedDmdId.id.id == 0)
      {
         // Poll for late/missed source publications (see RescanSources).
         if (++idleTicks >= 120) { // ~2s
            idleTicks = 0;
            needRescan = true;
         }
         // No DMD display source on the bus (EM tables): stream the composited
         // score-reel image instead. The version counter only changes when a
         // displayed reel value changes, so this is idle most ticks.
         int reelW = 0, reelH = 0;
         uint64_t reelVersion = 0;
         if (GetReelImage(&reelW, &reelH, &reelVersion, &reelImage)
            && reelVersion != lastReelVersion && reelW > 0 && reelH > 0
            && reelImage.size() >= (size_t)reelW * reelH * 3)
         {
            lastReelVersion = reelVersion;
            int outW = 0, outH = 0;
            FitRGB24(reelImage, reelW, reelH, reelScaled, outW, outH);
            pDmd->UpdateRGB24Data(reelScaled.data(), (uint16_t)outW, (uint16_t)outH);
         }
      }
      }
      if (needRescan)
      {
         RescanSources(true); // quiet: logs only when a source is found
         continue;
      }

      std::lock_guard<std::mutex> lock(sourceMutex);

      if (selectedDmdId.id.id == 0)
         continue;

      const DisplayFrame frame = selectedDmdId.GetRenderFrame(selectedDmdId.id);
      if (lastFrameID == frame.frameId)
         continue;
      lastFrameID = frame.frameId;

      switch(selectedDmdId.frameFormat) {
         case CTLPI_DISPLAY_FORMAT_LUM32F:
         {
            const float* const __restrict luminanceData = static_cast<const float*>(frame.frame);
            uint8_t* const __restrict rgb24Data = (uint8_t*)malloc(selectedDmdId.width * selectedDmdId.height * 3);

            for (unsigned int i = 0; i < selectedDmdId.width * selectedDmdId.height; ++i) {
                const float lum = luminanceData[i];
                rgb24Data[i * 3    ] = (uint8_t)(lum * tintR);
                rgb24Data[i * 3 + 1] = (uint8_t)(lum * tintG);
                rgb24Data[i * 3 + 2] = (uint8_t)(lum * tintB);
            }

            pDmd->UpdateRGB24Data(rgb24Data, selectedDmdId.width, selectedDmdId.height);
            free(rgb24Data);
         }
         break;

         case CTLPI_DISPLAY_FORMAT_SRGB888:
            pDmd->UpdateRGB24Data(static_cast<const uint8_t*>(frame.frame), selectedDmdId.width, selectedDmdId.height);
            break;

         case CTLPI_DISPLAY_FORMAT_SRGB565:
            pDmd->UpdateRGB16Data((const uint16_t*)frame.frame, selectedDmdId.width, selectedDmdId.height);
            break;
      }
   }
   isRunning = false;
}

// Query the bus and (re)select the display source to stream. Called from the
// ON_SRC_CHG broadcast AND polled from the update thread while no source is
// selected: a publisher's change broadcast can arrive nested inside another
// broadcast (e.g. AlphaDMD publishing from within libpinmame's seg-change
// broadcast), in which case the GET query here reaches no subscribers and the
// source would otherwise stay unselected until some unrelated re-broadcast.
static void RescanSources(const bool quiet)
{
   DisplaySrcId newDmdId = {};

   GetDisplaySrcMsg getSrcMsg = { 1024, 0, new DisplaySrcId[1024] };
   msgApi->BroadcastMsg( endpointId, getDmdSrcMsgId, &getSrcMsg);

   bool foundDMD = false;

   // Select the largest color display
   for (unsigned int i = 0; i < getSrcMsg.count; i++) {
      if (getSrcMsg.entries[i].frameFormat != CTLPI_DISPLAY_FORMAT_LUM32F) {
          if (getSrcMsg.entries[i].width > newDmdId.width) {
              newDmdId = getSrcMsg.entries[i];
              foundDMD = true;
          }
      }
   }

   // Defaults to the largest monochrome display
   if (!foundDMD) {
      for (unsigned int i = 0; i < getSrcMsg.count; i++) {
         if (getSrcMsg.entries[i].frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F) {
             if (getSrcMsg.entries[i].width > newDmdId.width) {
               newDmdId = getSrcMsg.entries[i];
               foundDMD = true;
            }
         }
      }
   }

   delete[] getSrcMsg.entries;

   std::lock_guard<std::mutex> lock(sourceMutex);
   selectedDmdId = newDmdId;

   if (foundDMD)
      LOGI("DMD Source Changed: format=%d, width=%d, height=%d", newDmdId.frameFormat, newDmdId.width, newDmdId.height);
   else if (!quiet)
      LOGI("DMD Source Changed: no display source on the bus (EM reel fallback applies if a reel image is active)");
}

static void onDmdSrcChanged(const unsigned int msgId, void* userData, void* msgData)
{
   RescanSources(false);
}

// The update thread runs for the whole game session (not just while a bus DMD
// source exists): EM reel tables never publish a display source, and their
// score is streamed via the GetReelImage fallback in UpdateThread.
static void onGameStart(const unsigned int msgId, void* userData, void* msgData)
{
   // Device creation (and its potentially slow network connect) happens at the
   // top of UpdateThread, never on this (game) thread.
   isRunning = true;
   if (!updateThread.joinable())
      updateThread = std::thread(UpdateThread);
}

static void onGameEnd(const unsigned int msgId, void* userData, void* msgData)
{
   isRunning = false;
   if (updateThread.joinable())
      updateThread.join();
   delete pDmd;
   pDmd = nullptr;
   // Drop the source selection: its GetRenderFrame callback dangles once the
   // publishing plugin tears down, and the next game's thread starts before
   // any onDmdSrcChanged fires.
   std::lock_guard<std::mutex> lock(sourceMutex);
   selectedDmdId = {};
}

}

using namespace DMDUtilPlugin;

MSGPI_EXPORT void MSGPIAPI DMDUtilPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;

   LPISetup(endpointId, msgApi); // Request and setup shared login API

   msgApi->SubscribeMsg(endpointId, onGameStartId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_START), onGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onGameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), onGameEnd, nullptr);
   msgApi->SubscribeMsg(endpointId, onDmdSrcChangedId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_ON_SRC_CHG_MSG), onDmdSrcChanged, nullptr);

   msgApi->RegisterSetting(endpointId, &zeDMDProp);
   msgApi->RegisterSetting(endpointId, &zeDMDDeviceFolderProp);
   msgApi->RegisterSetting(endpointId, &zeDMDDebugFolderProp);
   msgApi->RegisterSetting(endpointId, &zeDMDBrightnessFolderProp);
   msgApi->RegisterSetting(endpointId, &zeDMDWifiProp);
   msgApi->RegisterSetting(endpointId, &zeDMDWiFiAddrFolderProp);
   msgApi->RegisterSetting(endpointId, &pixelcadeProp);
   msgApi->RegisterSetting(endpointId, &pixelcadeDeviceProp);
   msgApi->RegisterSetting(endpointId, &dmdServerFolderProp);
   msgApi->RegisterSetting(endpointId, &dmdServerAddrFolderProp);
   msgApi->RegisterSetting(endpointId, &dmdServerPortFolderProp);

   msgApi->RegisterSetting(endpointId, &findDisplaysProp);
   msgApi->RegisterSetting(endpointId, &dumpDMDTxtProp);
   msgApi->RegisterSetting(endpointId, &dumpDMDRawProp);
   msgApi->RegisterSetting(endpointId, &lumTintRProp);
   msgApi->RegisterSetting(endpointId, &lumTintGProp);
   msgApi->RegisterSetting(endpointId, &lumTintBProp);

   getDmdSrcMsgId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_GET_SRC_MSG);

   DMDUtil::Config* pConfig = DMDUtil::Config::GetInstance();
   pConfig->SetLogCallback(OnDMDUtilLog);
   pConfig->SetZeDMD(zeDMDProp_Val);
   pConfig->SetZeDMDDevice(zeDMDDeviceFolderProp_Get());
   pConfig->SetZeDMDDebug(zeDMDDebugFolderProp_Get());
   pConfig->SetZeDMDBrightness(zeDMDBrightnessFolderProp_Val);
   pConfig->SetZeDMDWiFiEnabled(zeDMDWifiProp_Val);
   pConfig->SetZeDMDWiFiAddr(zeDMDWiFiAddrFolderProp_Get());
   pConfig->SetPixelcade(pixelcadeProp_Val);
   pConfig->SetPixelcadeDevice(pixelcadeDeviceProp_Get());
   pConfig->SetDMDServer(dmdServerFolderProp_Val);
   pConfig->SetDMDServerAddr(dmdServerAddrFolderProp_Get());
   pConfig->SetDMDServerPort(dmdServerPortFolderProp_Val);
}

MSGPI_EXPORT void MSGPIAPI DMDUtilPluginUnload()
{
   onGameEnd(onGameEndId, nullptr, nullptr);

   msgApi->UnsubscribeMsg(onGameStartId, onGameStart);
   msgApi->UnsubscribeMsg(onGameEndId, onGameEnd);
   msgApi->UnsubscribeMsg(onDmdSrcChangedId, onDmdSrcChanged);

   msgApi->ReleaseMsgID(onGameStartId);
   msgApi->ReleaseMsgID(onGameEndId);
   msgApi->ReleaseMsgID(onDmdSrcChangedId);
   msgApi->ReleaseMsgID(getDmdSrcMsgId);

   msgApi = nullptr;
}

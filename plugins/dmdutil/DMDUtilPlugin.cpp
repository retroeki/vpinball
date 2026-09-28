// license:GPLv3+

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstring>
#include <charconv>
#include <format>
#include <algorithm>
#include <vector>

#include "plugins/ColorSpace.h"
#include "plugins/VPXPlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/LoggingPlugin.h"

// Host reel-image channel (lib/src/VPinballLib.cpp, same statically linked
// module; ScoreView.cpp consumes it the same way). ReelDmd composites EM
// score-reel artwork into a tightly packed w*h*4 sRGBA image (opaque digits,
// semi-transparent surround for the on-screen view); returns false when no reel
// image is active. EM tables publish no DMD display source, so this is the only
// way their score reaches external DMDs. A physical DMD is opaque, so we flatten
// the alpha over black before streaming it.
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

using namespace PinballPlugin::Controller;

static const MsgPluginAPI* msgApi = nullptr;
static uint32_t endpointId;
static unsigned int onGameStartId;

static std::unique_ptr<CtrlItemConsumer<DisplaySrcId>> dmdSource;
static std::unique_ptr<class DMDUtilDispatcher> dmdDispatcher;

MSGPI_BOOL_VAL_SETTING(zeDMDProp, "ZeDMD", "ZeDMD", "", true, false);
MSGPI_STRING_VAL_SETTING(zeDMDDeviceFolderProp, "ZeDMDDevice", "ZeDMDDevice", "", true, "", 1024);
MSGPI_BOOL_VAL_SETTING(zeDMDDebugFolderProp, "ZeDMDDebug", "ZeDMDDebug", "", true, false);
MSGPI_INT_VAL_SETTING(zeDMDBrightnessFolderProp, "ZeDMDBrightness", "ZeDMDBrightness", "", true, -1, 1000, -1);
MSGPI_BOOL_VAL_SETTING(zeDMDWiFiEnabledProp, "ZeDMDWiFiEnabled", "ZeDMDWiFiEnabled", "", true, false);
MSGPI_STRING_VAL_SETTING(zeDMDWiFiAddrFolderProp, "ZeDMDWiFiAddr", "ZeDMDWiFiAddr", "", true, "zedmd-wifi.local", 1024);
MSGPI_BOOL_VAL_SETTING(zeDMDSPIEnabledProp, "ZeDMDSPIEnabled", "ZeDMDSPIEnabled", "", true, false);
MSGPI_INT_VAL_SETTING(zeDMDSPISpeedProp, "ZeDMDSPISpeed", "ZeDMDSPISpeed", "", true, 0, 100000000, 72000000);
MSGPI_INT_VAL_SETTING(zeDMDSPIFramePauseProp, "ZeDMDSPIFramePause", "ZeDMDSPIFramePause", "", true, 0, 1000, 2);
MSGPI_INT_VAL_SETTING(zeDMDSPIWidthProp, "ZeDMDSPIWidth", "ZeDMDSPIWidth", "", true, 0, 1000, 128);
MSGPI_INT_VAL_SETTING(zeDMDSPIHeightProp, "ZeDMDSPIHeight", "ZeDMDSPIHeight", "", true, 0, 1000, 32);
MSGPI_BOOL_VAL_SETTING(pixelcadeProp, "Pixelcade", "Pixelcade", "", true, false);
MSGPI_STRING_VAL_SETTING(pixelcadeDeviceProp, "PixelcadeDevice", "PixelcadeDevice", "", true, "", 1024);
MSGPI_BOOL_VAL_SETTING(pin2dmdProp, "PIN2DMD", "PIN2DMD", "", true, false);
MSGPI_BOOL_VAL_SETTING(dmdServerFolderProp, "DMDServer", "DMDServer", "", true, false);
MSGPI_STRING_VAL_SETTING(dmdServerAddrFolderProp, "DMDServerAddr", "DMDServerAddr", "", true, "localhost", 1024);
MSGPI_INT_VAL_SETTING(dmdServerPortFolderProp, "DMDServerPort", "DMDServerPort", "", true, 0, 65535, 6789);

MSGPI_BOOL_VAL_SETTING(findDisplaysProp, "FindDisplays", "FindDisplays", "", true, true);
MSGPI_BOOL_VAL_SETTING(dumpDMDTxtProp, "DumpDMDTxt", "DumpDMDTxt", "", true, false);
MSGPI_BOOL_VAL_SETTING(dumpDMDRawProp, "DumpDMDRaw", "DumpDMDRaw", "", true, false);
MSGPI_INT_VAL_SETTING(lumTintRProp, "LumTintR", "LumTintR", "", true, 0, 255, DMDUTIL_TINT_R);
MSGPI_INT_VAL_SETTING(lumTintGProp, "LumTintG", "LumTintG", "", true, 0, 255, DMDUTIL_TINT_G);
MSGPI_INT_VAL_SETTING(lumTintBProp, "LumTintB", "LumTintB", "", true, 0, 255, DMDUTIL_TINT_B);


LPI_USE_CPP();
#define LOGD DMDUtilPlugin::LPI_LOGD_CPP
#define LOGI DMDUtilPlugin::LPI_LOGI_CPP
#define LOGW DMDUtilPlugin::LPI_LOGW_CPP
#define LOGE DMDUtilPlugin::LPI_LOGE_CPP

LPI_IMPLEMENT_CPP // Implement shared log support

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

class DMDUtilDispatcher
{
public:
   DMDUtilDispatcher()
   {
      DMDUtil::Config* pConfig = DMDUtil::Config::GetInstance();
      pConfig->SetLogCallback(OnDMDUtilLog);
      pConfig->SetZeDMD(zeDMDProp_Val);
      pConfig->SetZeDMDDevice(zeDMDDeviceFolderProp_Get());
      pConfig->SetZeDMDDebug(zeDMDDebugFolderProp_Get());
      pConfig->SetZeDMDBrightness(zeDMDBrightnessFolderProp_Val);
      pConfig->SetZeDMDWiFiEnabled(zeDMDWiFiEnabledProp_Val);
      pConfig->SetZeDMDWiFiAddr(zeDMDWiFiAddrFolderProp_Get());
      pConfig->SetZeDMDSpiEnabled(zeDMDSPIEnabledProp_Val);
      pConfig->SetZeDMDSpiSpeed(zeDMDSPISpeedProp_Val);
      pConfig->SetZeDMDSpiFramePause(zeDMDSPIFramePauseProp_Val);
      pConfig->SetZeDMDWidth(zeDMDSPIWidthProp_Val);
      pConfig->SetZeDMDHeight(zeDMDSPIHeightProp_Val);
      pConfig->SetPixelcade(pixelcadeProp_Val);
      pConfig->SetPixelcadeDevice(pixelcadeDeviceProp_Get());
      pConfig->SetPIN2DMD(pin2dmdProp_Val);
      pConfig->SetDMDServer(dmdServerFolderProp_Val);
      pConfig->SetDMDServerAddr(dmdServerAddrFolderProp_Get());
      pConfig->SetDMDServerPort(dmdServerPortFolderProp_Val);

      m_updateThread = std::thread(&DMDUtilDispatcher::UpdateThread, this);
   }

   ~DMDUtilDispatcher()
   {
      m_isRunning = false;
      if (m_updateThread.joinable())
         m_updateThread.join();
   }

private:
   void UpdateThread()
   {
      // Create the device on this background thread: FindDisplays runs
      // ConnectDMDServer synchronously, and an unreachable dmdserver host blocks
      // on the TCP connect for the full kernel timeout (~2 minutes). Doing this
      // on the plugin API thread froze table loading for that long.
      // Note: a close during such a hang is covered by the app-side process kill.
      m_pDmd = std::make_unique<DMDUtil::DMD>();

      if (findDisplaysProp_Val)
         m_pDmd->FindDisplays();

      if (dumpDMDTxtProp_Val)
         m_pDmd->DumpDMDTxt();

      if (dumpDMDRawProp_Val)
         m_pDmd->DumpDMDRaw();

      m_lastFrameID = 0;
      while (m_isRunning)
      {
         // Fixed update at 60 FPS
         // TODO the dispatch should be done at the refesh rate of the target display
         std::this_thread::sleep_for(std::chrono::microseconds(16666));

         const bool hasSource = dmdSource->With(
            [this](const std::vector<DisplaySrcId>& items)
            {
               if (items.empty())
                  return false;
               ProcessFrame(items.front());
               return true;
            });

         // No DMD display source on the bus (EM tables): stream the composited score-reel image instead
         if (!hasSource)
            ProcessReelImage();
      }
   }

   void ProcessReelImage()
   {
      // The version counter only changes when a displayed reel value changes, so this is idle most ticks
      int reelW = 0, reelH = 0;
      uint64_t reelVersion = 0;
      if (!GetReelImage(&reelW, &reelH, &reelVersion, nullptr) || reelVersion == m_lastReelVersion || reelW <= 0 || reelH <= 0)
         return;
      if (!GetReelImage(&reelW, &reelH, &reelVersion, &m_reelImage) || reelW <= 0 || reelH <= 0 || m_reelImage.size() < (size_t)reelW * reelH * 4)
         return;
      m_lastReelVersion = reelVersion;
      // RGBA -> RGB24, alpha composited over black (a physical DMD is opaque):
      // opaque digits stay bright, the translucent surround collapses to dark.
      const size_t px = (size_t)reelW * reelH;
      m_reelFlat.resize(px * 3);
      for (size_t i = 0; i < px; ++i)
      {
         const unsigned a = m_reelImage[i * 4 + 3];
         m_reelFlat[i * 3 + 0] = (uint8_t)((m_reelImage[i * 4 + 0] * a) / 255);
         m_reelFlat[i * 3 + 1] = (uint8_t)((m_reelImage[i * 4 + 1] * a) / 255);
         m_reelFlat[i * 3 + 2] = (uint8_t)((m_reelImage[i * 4 + 2] * a) / 255);
      }
      int outW = 0, outH = 0;
      FitRGB24(m_reelFlat, reelW, reelH, m_reelScaled, outW, outH);
      m_pDmd->UpdateRGB24Data(m_reelScaled.data(), (uint16_t)outW, (uint16_t)outH);
   }

   void ProcessFrame(const DisplaySrcId& dmdSource)
   {
      const DisplayFrame frame = dmdSource.GetRenderFrame(dmdSource.callContext);
      if (m_lastFrameID == frame.frameId)
         return;
      m_lastFrameID = frame.frameId;

      switch (dmdSource.frameFormat)
      {
      case CTLPI_DISPLAY_FORMAT_LUM32F:
      {
         const float* const __restrict luminanceData = static_cast<const float*>(frame.frame);
         uint8_t* const __restrict rgb24Data = new uint8_t[dmdSource.width * dmdSource.height * 3];

         const uint32_t tintR = (uint32_t)lumTintRProp_Val;
         const uint32_t tintG = (uint32_t)lumTintGProp_Val;
         const uint32_t tintB = (uint32_t)lumTintBProp_Val;

         // LUM32F is a linear luminance while UpdateRGB24Data seems to take gamma encoded components,
         // at least the SRGB888 case below shows this by handing it a frame unconverted. So encode once
         // per dot, then apply the (also gamma encoded) tint
         // FIXME to be verified if the SRGB888 is really the reference, or if now both cases are wrong
         const unsigned int wh = dmdSource.width * dmdSource.height;
         for (unsigned int i = 0; i < wh; ++i)
         {
            const uint32_t lum = VPXColorSpace::LinearToSRGB(luminanceData[i]);
            rgb24Data[i * 3    ] = (uint8_t)((lum * tintR + 127u) / 255u);
            rgb24Data[i * 3 + 1] = (uint8_t)((lum * tintG + 127u) / 255u);
            rgb24Data[i * 3 + 2] = (uint8_t)((lum * tintB + 127u) / 255u);
         }

         m_pDmd->UpdateRGB24Data(rgb24Data, dmdSource.width, dmdSource.height);
         delete[] rgb24Data;
      }
      break;

      case CTLPI_DISPLAY_FORMAT_SRGB888: m_pDmd->UpdateRGB24Data(static_cast<const uint8_t*>(frame.frame), dmdSource.width, dmdSource.height); break;

      case CTLPI_DISPLAY_FORMAT_SRGB565: m_pDmd->UpdateRGB16Data((const uint16_t*)frame.frame, dmdSource.width, dmdSource.height); break;
      }
   }

   static void DMDUTILCALLBACK OnDMDUtilLog(DMDUtil_LogLevel logLevel, const char* format, va_list args)
   {
#ifndef _DEBUG
      // libdmdutil debug relays are dropped by the host logger in release; skip the double
      // vsnprintf format instead of building a string that will be discarded.
      if (logLevel == DMDUtil_LogLevel_DEBUG)
         return;
#endif
      va_list args_copy;
      va_copy(args_copy, args);
      int size = vsnprintf(nullptr, 0, format, args_copy);
      va_end(args_copy);
      if (size > 0)
      {
         string buffer(size + 1, '\0');
         vsnprintf(buffer.data(), size + 1, format, args);
         buffer.pop_back(); // remove null terminator
         switch (logLevel)
         {
         case DMDUtil_LogLevel_INFO: LOGI(buffer); break;
         case DMDUtil_LogLevel_DEBUG: LOGD(buffer); break;
         case DMDUtil_LogLevel_ERROR: LOGE(buffer); break;
         default: break;
         }
      }
   }

   std::unique_ptr<DMDUtil::DMD> m_pDmd;
   std::thread m_updateThread;
   bool m_isRunning = true;
   int m_lastFrameID = 0;
   uint64_t m_lastReelVersion = 0;
   std::vector<uint8_t> m_reelImage, m_reelScaled, m_reelFlat;
};


static void SelectSource(std::vector<DisplaySrcId>& items)
{
   bool foundDMD = false;
   DisplaySrcId newDmdId = { };

   // Skip video monitor sources of pinball/video hybrids like Baby Pac-Man or Granny
   // and the Gators, which are flagged as CRT displays and are not meant for DMD
   // devices. Also skip sources larger than libdmdutil's update buffers, which are
   // fixed at 256x64 pixels and overflowed by larger frames (vpinball/libdmdutil#65).
   constexpr unsigned int maxPixels = 256 * 64;
   auto isSupported = [](const DisplaySrcId& src)
   {
      if ((src.hardware & CTLPI_DISPLAY_HARDWARE_FAMILY_MASK) == CTLPI_DISPLAY_HARDWARE_CRT_DISPLAY)
      {
         LOGI(std::format("Display source of {}x{} pixels is a video display and cannot be shown on DMD devices, skipping it", src.width, src.height));
         return false;
      }
      if ((unsigned int)src.width * src.height > maxPixels)
      {
         LOGW(std::format("Display source of {}x{} pixels exceeds the size supported by libdmdutil and cannot be shown on DMD devices, skipping it", src.width, src.height));
         return false;
      }
      return true;
   };

   // Select the largest color display
   for (const DisplaySrcId& src : items)
   {
      if (src.frameFormat != CTLPI_DISPLAY_FORMAT_LUM32F && isSupported(src))
      {
         if (src.width > newDmdId.width)
         {
            newDmdId = src;
            foundDMD = true;
         }
      }
   }

   // Defaults to the largest monochrome display
   if (!foundDMD)
   {
      for (const DisplaySrcId& src : items)
      {
         if (src.frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F && isSupported(src))
         {
            if (src.width > newDmdId.width)
            {
               newDmdId = src;
               foundDMD = true;
            }
         }
      }
   }

   items.clear();
   if (foundDMD)
      items.push_back(newDmdId);
}

// The dispatcher runs for the whole game session (not just while a bus DMD
// source exists): EM reel tables never publish a display source, and their
// score is streamed via the GetReelImage fallback of the update thread.
static void onGameStart(const unsigned int msgId, void* userData, void* msgData)
{
   // Device creation (and its potentially slow network connect) happens at the
   // top of the update thread, never on this (game) thread.
   if (dmdDispatcher == nullptr)
      dmdDispatcher = std::make_unique<DMDUtilDispatcher>();
}

}

using namespace DMDUtilPlugin;

MSGPI_EXPORT void MSGPIAPI DMDUtilPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;

   LPISetup(endpointId, msgApi); // Request and setup shared login API

   msgApi->RegisterSetting(endpointId, &zeDMDProp);
   msgApi->RegisterSetting(endpointId, &zeDMDDeviceFolderProp);
   msgApi->RegisterSetting(endpointId, &zeDMDDebugFolderProp);
   msgApi->RegisterSetting(endpointId, &zeDMDBrightnessFolderProp);
   msgApi->RegisterSetting(endpointId, &zeDMDWiFiEnabledProp);
   msgApi->RegisterSetting(endpointId, &zeDMDWiFiAddrFolderProp);
   msgApi->RegisterSetting(endpointId, &zeDMDSPIEnabledProp);
   msgApi->RegisterSetting(endpointId, &zeDMDSPISpeedProp);
   msgApi->RegisterSetting(endpointId, &zeDMDSPIFramePauseProp);
   msgApi->RegisterSetting(endpointId, &zeDMDSPIWidthProp);
   msgApi->RegisterSetting(endpointId, &zeDMDSPIHeightProp);
   msgApi->RegisterSetting(endpointId, &pixelcadeProp);
   msgApi->RegisterSetting(endpointId, &pixelcadeDeviceProp);
   msgApi->RegisterSetting(endpointId, &pin2dmdProp);
   msgApi->RegisterSetting(endpointId, &dmdServerFolderProp);
   msgApi->RegisterSetting(endpointId, &dmdServerAddrFolderProp);
   msgApi->RegisterSetting(endpointId, &dmdServerPortFolderProp);

   msgApi->RegisterSetting(endpointId, &findDisplaysProp);
   msgApi->RegisterSetting(endpointId, &dumpDMDTxtProp);
   msgApi->RegisterSetting(endpointId, &dumpDMDRawProp);
   msgApi->RegisterSetting(endpointId, &lumTintRProp);
   msgApi->RegisterSetting(endpointId, &lumTintGProp);
   msgApi->RegisterSetting(endpointId, &lumTintBProp);

   dmdSource = std::make_unique<CtrlItemConsumer<DisplaySrcId>>(
      msgApi, endpointId, CTLPI_DISPLAY_GET_SRC_MSG, CTLPI_DISPLAY_ON_SRC_CHG_MSG,
      [](std::vector<DisplaySrcId>& items) { SelectSource(items); },
      []() { },
      []() { dmdSource->With([](const std::vector<DisplaySrcId>& items) {
            if (items.empty())
            {
               LOGI("No DMD source selected");
               return;
            }
            const DisplaySrcId& dmdSrc = items.front();
            LOGI(std::format("DMD source selected [endpointId={}.{}, {}x{} fmt={}]", dmdSrc.id.endpointId, dmdSrc.id.resId, dmdSrc.width, dmdSrc.height, dmdSrc.frameFormat));
            if (dmdDispatcher == nullptr)
               dmdDispatcher = std::make_unique<DMDUtilDispatcher>();
         }); });
   dmdSource->Subscribe();

   msgApi->SubscribeMsg(endpointId, onGameStartId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_START), onGameStart, nullptr);
}

MSGPI_EXPORT void MSGPIAPI DMDUtilPluginUnload()
{
   msgApi->UnsubscribeMsg(onGameStartId, onGameStart, nullptr);
   msgApi->ReleaseMsgID(onGameStartId);
   dmdDispatcher = nullptr;
   dmdSource->Unsubscribe();
   dmdSource = nullptr;
   msgApi = nullptr;
}

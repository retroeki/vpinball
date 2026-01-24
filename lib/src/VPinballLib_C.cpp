// license:GPLv3+

#include <core/stdafx.h>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

#include "../include/vpinball/VPinballLib_C.h"
#include "VPinballLib.h"

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

VPINBALLAPI void VPinballUpdateWebServer()
{
   VPinballLib::VPinballLib::Instance().UpdateWebServer();
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

VPINBALLAPI VPINBALL_STATUS VPinballSetBloomStrength(float strength)
{
   return VPinballLib::VPinballLib::Instance().SetBloomStrength(strength);
}

VPINBALLAPI float VPinballGetBloomStrength()
{
   return VPinballLib::VPinballLib::Instance().GetBloomStrength();
}

VPINBALLAPI VPINBALL_STATUS VPinballSetEmissionScale(float scale)
{
   return VPinballLib::VPinballLib::Instance().SetEmissionScale(scale);
}

VPINBALLAPI float VPinballGetEmissionScale()
{
   return VPinballLib::VPinballLib::Instance().GetEmissionScale();
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


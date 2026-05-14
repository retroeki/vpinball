// license:GPLv3+

#include "core/stdafx.h"

#include "../../../include/vpinball/VPinballLib_C.h"
#include "../../../src/VPinballLib.h"

#include <SDL3/SDL_system.h>
#include <jni.h>
#include <vector>

#ifdef ENABLE_XR
#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

using namespace VPinballLib;

static jobject gJNICallbackObject = nullptr;
static jmethodID gJNIOnEventMethod = nullptr;
static JavaVM* gJavaVM = nullptr;

// Get JNIEnv for current thread, attaching if necessary
static JNIEnv* GetJNIEnv()
{
   if (!gJavaVM)
      return nullptr;

   JNIEnv* env = nullptr;
   int status = gJavaVM->GetEnv((void**)&env, JNI_VERSION_1_6);

   if (status == JNI_EDETACHED)
   {
      // Thread not attached, attach it
      if (gJavaVM->AttachCurrentThread(&env, nullptr) != 0)
         return nullptr;
   }
   else if (status != JNI_OK)
   {
      return nullptr;
   }

   return env;
}

void VPinballJNI_OnEventCallback(VPINBALL_EVENT event, const char* jsonData)
{
   if (!gJNICallbackObject || !gJNIOnEventMethod)
      return;

   JNIEnv* env = GetJNIEnv();
   if (!env)
      return;

   jstring jsonDataString = jsonData ? env->NewStringUTF(jsonData) : nullptr;
   env->CallVoidMethod(gJNICallbackObject, gJNIOnEventMethod, (jint)event, jsonDataString);

   if (jsonDataString)
      env->DeleteLocalRef(jsonDataString);

   if (env->ExceptionCheck())
      env->ExceptionClear();
}

extern "C" {

JNIEXPORT jstring JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetVersionStringFull(JNIEnv* env, jobject obj)
{
   return env->NewStringUTF(VPinballGetVersionStringFull());
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballInit(JNIEnv* env, jobject obj, jobject callback)
{
   if (!callback)
      return;

   // Store JavaVM for use from other threads
   env->GetJavaVM(&gJavaVM);

   gJNICallbackObject = env->NewGlobalRef(callback);
   gJNIOnEventMethod = env->GetMethodID(env->GetObjectClass(gJNICallbackObject), "onEvent", "(ILjava/lang/String;)V");

   VPinballInit(VPinballJNI_OnEventCallback);
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballInitHeadless(JNIEnv* env, jobject obj, jobject callback)
{
   if (!callback)
      return;

   // Store JavaVM for use from other threads
   env->GetJavaVM(&gJavaVM);

   // Release old callback if exists
   if (gJNICallbackObject) {
      env->DeleteGlobalRef(gJNICallbackObject);
   }

   gJNICallbackObject = env->NewGlobalRef(callback);
   gJNIOnEventMethod = env->GetMethodID(env->GetObjectClass(gJNICallbackObject), "onEvent", "(ILjava/lang/String;)V");

   VPinballInitHeadless(VPinballJNI_OnEventCallback);
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballUpdateEventCallback(JNIEnv* env, jobject obj, jobject callback)
{
   if (!callback)
      return;

   // Store JavaVM for use from other threads
   env->GetJavaVM(&gJavaVM);

   // Release old callback if exists
   if (gJNICallbackObject) {
      env->DeleteGlobalRef(gJNICallbackObject);
   }

   gJNICallbackObject = env->NewGlobalRef(callback);
   gJNIOnEventMethod = env->GetMethodID(env->GetObjectClass(gJNICallbackObject), "onEvent", "(ILjava/lang/String;)V");

   VPinballUpdateEventCallback(VPinballJNI_OnEventCallback);
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballShutdown(JNIEnv* env, jobject obj)
{
   VPinballShutdown();

   // Release callback
   if (gJNICallbackObject) {
      env->DeleteGlobalRef(gJNICallbackObject);
      gJNICallbackObject = nullptr;
   }
   gJNIOnEventMethod = nullptr;
}

JNIEXPORT jboolean JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballIsInitialized(JNIEnv* env, jobject obj)
{
   return VPinballIsInitialized() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballLog(JNIEnv* env, jobject obj, jint level, jstring message)
{
   const char* pMessage = env->GetStringUTFChars(message, nullptr);
   VPinballLog(static_cast<VPINBALL_LOG_LEVEL>(level), pMessage);
   env->ReleaseStringUTFChars(message, pMessage);
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballResetLog(JNIEnv* env, jobject obj)
{
   VPinballResetLog();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballLoadValueInt(JNIEnv* env, jobject obj, jstring sectionName, jstring key, jint defaultValue)
{
   const char* pSectionName = env->GetStringUTFChars(sectionName, nullptr);
   const char* pKey = env->GetStringUTFChars(key, nullptr);
   int result = VPinballLoadValueInt(pSectionName, pKey, defaultValue);
   env->ReleaseStringUTFChars(key, pKey);
   env->ReleaseStringUTFChars(sectionName, pSectionName);
   return result;
}

JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballLoadValueFloat(JNIEnv* env, jobject obj, jstring sectionName, jstring key, jfloat defaultValue)
{
   const char* pSectionName = env->GetStringUTFChars(sectionName, nullptr);
   const char* pKey = env->GetStringUTFChars(key, nullptr);
   float result = VPinballLoadValueFloat(pSectionName, pKey, defaultValue);
   env->ReleaseStringUTFChars(key, pKey);
   env->ReleaseStringUTFChars(sectionName, pSectionName);
   return result;
}

JNIEXPORT jstring JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballLoadValueString(JNIEnv* env, jobject obj, jstring sectionName, jstring key, jstring defaultValue)
{
   const char* pSectionName = env->GetStringUTFChars(sectionName, nullptr);
   const char* pKey = env->GetStringUTFChars(key, nullptr);
   const char* pDefaultValue = env->GetStringUTFChars(defaultValue, nullptr);
   const char* pResult = VPinballLoadValueString(pSectionName, pKey, pDefaultValue);
   env->ReleaseStringUTFChars(defaultValue, pDefaultValue);
   env->ReleaseStringUTFChars(key, pKey);
   env->ReleaseStringUTFChars(sectionName, pSectionName);
   return env->NewStringUTF(pResult);
}

JNIEXPORT jboolean JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballLoadValueBool(JNIEnv* env, jobject obj, jstring sectionName, jstring key, jboolean defaultValue)
{
   const char* pSectionName = env->GetStringUTFChars(sectionName, nullptr);
   const char* pKey = env->GetStringUTFChars(key, nullptr);
   int result = VPinballLoadValueBool(pSectionName, pKey, defaultValue ? 1 : 0);
   env->ReleaseStringUTFChars(key, pKey);
   env->ReleaseStringUTFChars(sectionName, pSectionName);
   return result != 0;
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSaveValueInt(JNIEnv* env, jobject obj, jstring sectionName, jstring key, jint value)
{
   const char* pSectionName = env->GetStringUTFChars(sectionName, nullptr);
   const char* pKey = env->GetStringUTFChars(key, nullptr);
   VPinballSaveValueInt(pSectionName, pKey, value);
   env->ReleaseStringUTFChars(key, pKey);
   env->ReleaseStringUTFChars(sectionName, pSectionName);
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSaveValueFloat(JNIEnv* env, jobject obj, jstring sectionName, jstring key, jfloat value)
{
   const char* pSectionName = env->GetStringUTFChars(sectionName, nullptr);
   const char* pKey = env->GetStringUTFChars(key, nullptr);
   VPinballSaveValueFloat(pSectionName, pKey, value);
   env->ReleaseStringUTFChars(key, pKey);
   env->ReleaseStringUTFChars(sectionName, pSectionName);
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSaveValueString(JNIEnv* env, jobject obj, jstring sectionName, jstring key, jstring value)
{
   const char* pSectionName = env->GetStringUTFChars(sectionName, nullptr);
   const char* pKey = env->GetStringUTFChars(key, nullptr);
   const char* pValue = env->GetStringUTFChars(value, nullptr);
   VPinballSaveValueString(pSectionName, pKey, pValue);
   env->ReleaseStringUTFChars(value, pValue);
   env->ReleaseStringUTFChars(key, pKey);
   env->ReleaseStringUTFChars(sectionName, pSectionName);
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSaveValueBool(JNIEnv* env, jobject obj, jstring sectionName, jstring key, jboolean value)
{
   const char* pSectionName = env->GetStringUTFChars(sectionName, nullptr);
   const char* pKey = env->GetStringUTFChars(key, nullptr);
   VPinballSaveValueBool(pSectionName, pKey, value ? 1 : 0);
   env->ReleaseStringUTFChars(key, pKey);
   env->ReleaseStringUTFChars(sectionName, pSectionName);
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballResetIni(JNIEnv* env, jobject obj)
{
   return VPinballResetIni();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballResetTableIni(JNIEnv* env, jobject obj)
{
   return VPinballResetTableIni();
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballUpdateWebServer(JNIEnv* env, jobject obj)
{
   VPinballUpdateWebServer();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballLoadTable(JNIEnv* env, jobject obj, jstring path)
{
   const char* pPath = env->GetStringUTFChars(path, nullptr);
   VPINBALL_STATUS status = VPinballLoadTable(pPath);
   env->ReleaseStringUTFChars(path, pPath);
   return status;
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballCancelLoading(JNIEnv* env, jobject obj)
{
   VPinballCancelLoading();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballExtractTableScript(JNIEnv* env, jobject obj)
{
   return VPinballExtractTableScript();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballPlay(JNIEnv* env, jobject obj)
{
   return VPinballPlay();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballStop(JNIEnv* env, jobject obj)
{
   return VPinballStop();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballPause(JNIEnv* env, jobject obj)
{
   return VPinballPause();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballResume(JNIEnv* env, jobject obj)
{
   return VPinballResume();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSoftPause(JNIEnv* env, jobject obj)
{
   return VPinballSoftPause();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSoftResume(JNIEnv* env, jobject obj)
{
   return VPinballSoftResume();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballPushKeyEvent(JNIEnv* env, jobject obj, jint scancode, jboolean pressed)
{
   return VPinballPushKeyEvent(scancode, pressed ? 1 : 0);
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetThermalStatus(JNIEnv* env, jobject obj, jint status)
{
   return VPinballSetThermalStatus(status);
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetBatteryTempC(JNIEnv* env, jobject obj, jfloat tempC)
{
   return VPinballSetBatteryTempC(tempC);
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetBloomStrength(JNIEnv* env, jobject obj, jfloat strength)
{
   return VPinballSetBloomStrength(strength);
}

JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetBloomStrength(JNIEnv* env, jobject obj)
{
   return VPinballGetBloomStrength();
}

JNIEXPORT jint   JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetUserCGBrightness(JNIEnv*, jobject, jfloat v)  { return VPinballSetUserCGBrightness(v); }
JNIEXPORT jint   JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetUserCGContrast(JNIEnv*, jobject, jfloat v)    { return VPinballSetUserCGContrast(v); }
JNIEXPORT jint   JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetUserCGSaturation(JNIEnv*, jobject, jfloat v)  { return VPinballSetUserCGSaturation(v); }
JNIEXPORT jint   JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetUserCGTemperature(JNIEnv*, jobject, jfloat v) { return VPinballSetUserCGTemperature(v); }
JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetUserCGBrightness(JNIEnv*, jobject)  { return VPinballGetUserCGBrightness(); }
JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetUserCGContrast(JNIEnv*, jobject)    { return VPinballGetUserCGContrast(); }
JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetUserCGSaturation(JNIEnv*, jobject)  { return VPinballGetUserCGSaturation(); }
JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetUserCGTemperature(JNIEnv*, jobject) { return VPinballGetUserCGTemperature(); }

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetEmissionScale(JNIEnv* env, jobject obj, jfloat scale)
{
   return VPinballSetEmissionScale(scale);
}

JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetEmissionScale(JNIEnv* env, jobject obj)
{
   return VPinballGetEmissionScale();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetFOV(JNIEnv* env, jobject obj, jfloat fov)
{
   return VPinballSetFOV(fov);
}

JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetFOV(JNIEnv* env, jobject obj)
{
   return VPinballGetFOV();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetLookAt(JNIEnv* env, jobject obj, jfloat lookAt)
{
   return VPinballSetLookAt(lookAt);
}

JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetLookAt(JNIEnv* env, jobject obj)
{
   return VPinballGetLookAt();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetLayback(JNIEnv* env, jobject obj, jfloat layback)
{
   return VPinballSetLayback(layback);
}

JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetLayback(JNIEnv* env, jobject obj)
{
   return VPinballGetLayback();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetViewX(JNIEnv* env, jobject obj, jfloat x)
{
   return VPinballSetViewX(x);
}

JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetViewX(JNIEnv* env, jobject obj)
{
   return VPinballGetViewX();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetViewY(JNIEnv* env, jobject obj, jfloat y)
{
   return VPinballSetViewY(y);
}

JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetViewY(JNIEnv* env, jobject obj)
{
   return VPinballGetViewY();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetViewZ(JNIEnv* env, jobject obj, jfloat z)
{
   return VPinballSetViewZ(z);
}

JNIEXPORT jfloat JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetViewZ(JNIEnv* env, jobject obj)
{
   return VPinballGetViewZ();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetViewMode(JNIEnv* env, jobject obj)
{
   return VPinballGetViewMode();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetViewMode(JNIEnv* env, jobject obj, jint mode)
{
   return VPinballSetViewMode(static_cast<VPINBALL_VIEW_MODE>(mode));
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballCycleViewMode(JNIEnv* env, jobject obj)
{
   return VPinballCycleViewMode();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballToggleScoreView(JNIEnv* env, jobject obj, jboolean enable, jint x, jint y, jint width, jint height)
{
   return VPinballToggleScoreView(enable, x, y, width, height);
}

JNIEXPORT jintArray JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetScoreViewSourceSize(JNIEnv* env, jobject obj)
{
   int width = 0, height = 0;
   VPinballGetScoreViewSourceSize(&width, &height);
   jintArray result = env->NewIntArray(2);
   jint values[2] = { width, height };
   env->SetIntArrayRegion(result, 0, 2, values);
   return result;
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballCaptureScoreView(JNIEnv* env, jobject obj)
{
   return VPinballCaptureScoreView();
}

JNIEXPORT jintArray JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetScoreViewCapture(JNIEnv* env, jobject obj)
{
   // First, probe dimensions with a small buffer to check if ready
   int width = 0, height = 0;
   // Use a two-pass approach: first call with null-like to get dimensions, but our API needs pixels buffer
   // Instead, allocate a reasonable max buffer (4K = 3840x2160 = ~8M pixels, but ScoreView is typically small)
   // ScoreView is usually DMD-sized, so 1920x1080 max is very generous
   const int maxPixels = 1920 * 1080;
   std::vector<uint32_t> pixels(maxPixels);

   VPINBALL_STATUS status = VPinballGetScoreViewCapture(&width, &height, pixels.data(), maxPixels);
   if (status != VPINBALL_STATUS_SUCCESS || width <= 0 || height <= 0)
      return nullptr;

   // Pack as [width, height, pixel0, pixel1, ...]
   const int totalPixels = width * height;
   jintArray result = env->NewIntArray(2 + totalPixels);
   if (!result)
      return nullptr;

   jint header[2] = { width, height };
   env->SetIntArrayRegion(result, 0, 2, header);
   env->SetIntArrayRegion(result, 2, totalPixels, reinterpret_cast<const jint*>(pixels.data()));
   return result;
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetMusicVolume(JNIEnv* env, jobject obj, jint volume)
{
   return VPinballSetMusicVolume(volume);
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetSoundVolume(JNIEnv* env, jobject obj, jint volume)
{
   return VPinballSetSoundVolume(volume);
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetMusicVolume(JNIEnv* env, jobject obj)
{
   return VPinballGetMusicVolume();
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetSoundVolume(JNIEnv* env, jobject obj)
{
   return VPinballGetSoundVolume();
}

// Declare the external function from VPinballLib.cpp
extern "C" void VPinballSetInternalPath(const char* path);
extern "C" void VPinballSetWebLibraryPath(const char* path);
extern "C" void VPinballSetWebAdvancedPath(const char* path);

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetInternalPath(JNIEnv* env, jobject obj, jstring path)
{
   const char* pPath = env->GetStringUTFChars(path, nullptr);
   VPinballSetInternalPath(pPath);
   env->ReleaseStringUTFChars(path, pPath);
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetWebLibraryPath(JNIEnv* env, jobject obj, jstring path)
{
   const char* pPath = env->GetStringUTFChars(path, nullptr);
   VPinballSetWebLibraryPath(pPath);
   env->ReleaseStringUTFChars(path, pPath);
}

JNIEXPORT void JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballSetWebAdvancedPath(JNIEnv* env, jobject obj, jstring path)
{
   const char* pPath = env->GetStringUTFChars(path, nullptr);
   VPinballSetWebAdvancedPath(pPath);
   env->ReleaseStringUTFChars(path, pPath);
}

JNIEXPORT jstring JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetTableName(JNIEnv* env, jobject obj)
{
   return env->NewStringUTF(VPinballGetTableName());
}

JNIEXPORT jstring JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetTableVersion(JNIEnv* env, jobject obj)
{
   return env->NewStringUTF(VPinballGetTableVersion());
}

// Returns the running ROM's NVRAM bytes, or null for EM / non-ROM tables.
// Sized to PinMAME's max NVRAM (~64 KB). The actual ROM rarely uses that much.
JNIEXPORT jbyteArray JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballGetNVRAM(JNIEnv* env, jobject obj)
{
   static constexpr int kMaxNvramBytes = 65536;
   std::vector<uint8_t> buffer(kMaxNvramBytes);
   int isNvramTable = 0;
   const int written = VPinballGetNVRAM(buffer.data(), kMaxNvramBytes, &isNvramTable);
   if (!isNvramTable || written <= 0) return nullptr;
   jbyteArray result = env->NewByteArray(written);
   env->SetByteArrayRegion(result, 0, written, reinterpret_cast<const jbyte*>(buffer.data()));
   return result;
}

JNIEXPORT jint JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballResetTable(JNIEnv* env, jobject obj)
{
   return VPinballResetTable();
}

#ifdef ENABLE_XR
JNIEXPORT jboolean JNICALL Java_org_vpinball_app_jni_VPinballJNI_VPinballInitOpenXR(JNIEnv* env, jobject obj, jobject activity)
{
   JavaVM* vm;
   env->GetJavaVM(&vm);

   jobject globalActivity = env->NewGlobalRef(activity);

   XrLoaderInitInfoAndroidKHR loaderInitInfo = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
   loaderInitInfo.applicationVM = vm;
   loaderInitInfo.applicationContext = globalActivity;

   PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR;
   xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&xrInitializeLoaderKHR);

   if (xrInitializeLoaderKHR != nullptr)
   {
      XrResult result = xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInitInfo);
      return result == XR_SUCCESS;
   }
   return false;
}
#endif

}

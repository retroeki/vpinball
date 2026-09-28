// license:GPLv3+

#include "common.h"
#include "plugins/MsgPlugin.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/ScriptablePlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h" // Only used for optional feature (locating PinMAME files along a VPX table)

#include "Rom.h"
#include "Roms.h"
#include "Settings.h"
#include "GameSettings.h"
#include "Game.h"
#include "Games.h"
#include "ControllerSettings.h"
#include "Controller.h"

#include <filesystem>
#include <cassert>
#include <charconv>
#include <cstring>
#include <mutex>

namespace PinMAME
{

// Gate for verbose per-event PinMAME host-bridge diagnostics: the per-switch read/write
// traces below fire on every host switch set/get during play. Off by default; set to 1
// (or -DPINMAME_DEBUG_LOG=1) and rebuild to re-enable. Genuine errors (LOGE) stay on.
#ifndef PINMAME_DEBUG_LOG
#define PINMAME_DEBUG_LOG 0
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Scriptable object definitions

#define PSC_VAR_SET_PinMAME_Rom(variant, value) PSC_VAR_SET_object(Rom, variant, value)
PSC_CLASS_START(PinMAME_Rom, Rom)
   PSC_PROP_R(string, Name)
   PSC_PROP_R(int32, State)
   PSC_PROP_R(string, StateDescription)
   PSC_PROP_R(int32, Length)
   PSC_PROP_R(int32, ExpLength)
   PSC_PROP_R(int32, Checksum)
   PSC_PROP_R(int32, ExpChecksum)
   PSC_PROP_R(int32, Flags)
   //PSC_FUNCTION0(void, Audit) // not yet supported (2 functions with the same name, matched by their arguments)
   PSC_FUNCTION1(void, Audit, bool)
PSC_CLASS_END()

PSC_CLASS_START(PinMAME_Roms, Roms)
PSC_CLASS_END()

#define PSC_VAR_SET_PinMAME_Settings(variant, value) PSC_VAR_SET_object(Settings, variant, value)
PSC_CLASS_START(PinMAME_Settings, Settings)
   PSC_PROP_RW_ARRAY1(int, Value, string)
PSC_CLASS_END()

#define PSC_VAR_SET_PinMAME_GameSettings(variant, value) PSC_VAR_SET_object(GameSettings, variant, value)
PSC_CLASS_START(PinMAME_GameSettings, GameSettings)
   PSC_PROP_RW_ARRAY1(int, Value, string)
PSC_CLASS_END()

#define PSC_VAR_SET_PinMAME_Game(variant, value) PSC_VAR_SET_object(Game, variant, value)
PSC_CLASS_START(PinMAME_Game, Game)
   PSC_PROP_R(string, Name)
   PSC_PROP_R(string, Description)
   PSC_PROP_R(string, Year)
   PSC_PROP_R(string, Manufacturer)
   PSC_PROP_R(string, CloneOf)
   PSC_PROP_R(PinMAME_GameSettings, Settings)
PSC_CLASS_END()

PSC_CLASS_START(PinMAME_Games, Games)
PSC_CLASS_END()

PSC_CLASS_START(PinMAME_ControllerSettings, ControllerSettings)
PSC_CLASS_END()

PSC_ARRAY1(PinMAME_ByteArray, uint8, 0)
#define PSC_VAR_SET_PinMAME_ByteArray(variant, value) PSC_VAR_SET_array1(PinMAME_ByteArray, variant, value)
#define PSC_VAR_PinMAME_ByteArray(variant) PSC_VAR_array1(uint8_t, variant)

PSC_ARRAY1(PinMAME_IntArray, int32, 0)
#define PSC_VAR_SET_PinMAME_IntArray(variant, value) PSC_VAR_SET_array1(PinMAME_IntArray, variant, value)

// Map a an array of struct to a 2 dimensions array of int32_t
PSC_ARRAY2(PinMAME_StructArray, int32, 0, 0)
#define PSC_VAR_SET_PinMAME_StructArray2(structType, fieldName1, fieldName2, variant, value) { \
      const unsigned int nDimensions = 2; \
      const std::vector<structType>& vec = (value); \
      const size_t size0 = vec.size(); \
      ScriptArray* array = static_cast<ScriptArray*>(malloc(sizeof(ScriptArray) + nDimensions * sizeof(int) + size0 * (2 * sizeof(int32_t)))); \
      array->Release = [](ScriptArray* me) { free(me); }; \
      array->lengths[0] = static_cast<unsigned int>(vec.size()); \
      array->lengths[1] = 2; \
      int32_t* pData = reinterpret_cast<int32_t*>(&array->lengths[2]); \
      for (size_t i = 0; i < size0; i++, pData += 2) { \
         pData[0] = vec[i].fieldName1; \
         pData[1] = vec[i].fieldName2; \
      } \
      (variant).vArray = array; \
   }
#define PSC_VAR_SET_PinMAME_StructArray3(structType, fieldName1, fieldName2, fieldName3, variant, value) { \
      const unsigned int nDimensions = 3; \
      const std::vector<structType>& vec = (value); \
      const size_t size0 = vec.size(); \
      ScriptArray* array = static_cast<ScriptArray*>(malloc(sizeof(ScriptArray) + nDimensions * sizeof(int) + size0 * (3 * sizeof(int32_t)))); \
      array->Release = [](ScriptArray* me) { free(me); }; \
      array->lengths[0] = static_cast<unsigned int>(vec.size()); \
      array->lengths[1] = 3; \
      int32_t* pData = reinterpret_cast<int32_t*>(&array->lengths[2]); \
      for (size_t i = 0; i < size0; i++, pData += 3) { \
         pData[0] = vec[i].fieldName1; \
         pData[1] = vec[i].fieldName2; \
         pData[2] = vec[i].fieldName3; \
      } \
      (variant).vArray = array; \
   }

#define PSC_PROP_R_StructArray2(type, fieldName1, fieldName2, name) \
   members.push_back( { { #name }, { "PinMAME_StructArray" }, 0, { }, \
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant* pRet) { \
         PSC_VAR_SET_PinMAME_StructArray2(type, fieldName1, fieldName2, *pRet, static_cast<_BindedClass*>(me)->Get##name()); } });

#define PSC_PROP_R_StructArray3(type, fieldName1, fieldName2, fieldName3, name) \
   members.push_back( { { #name }, { "PinMAME_StructArray" }, 0, { }, \
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant* pRet) { \
         PSC_VAR_SET_PinMAME_StructArray3(type, fieldName1, fieldName2, fieldName3, *pRet, static_cast<_BindedClass*>(me)->Get##name()); } });
#define PSC_PROP_R_StructArray3_2(type, fieldName1, fieldName2, fieldName3, name, arg1, arg2) \
   members.push_back( { { #name }, { "PinMAME_StructArray" }, 2, { { #arg1 }, { #arg2 } }, \
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant* pRet) { \
         PSC_VAR_SET_PinMAME_StructArray3(type, fieldName1, fieldName2, fieldName3, *pRet, static_cast<_BindedClass*>(me)->Get##name(PSC_VAR_##arg1(pArgs[0]), PSC_VAR_##arg2(pArgs[1]))); } });
#define PSC_PROP_R_StructArray3_3(type, fieldName1, fieldName2, fieldName3, name, arg1, arg2, arg3) \
   members.push_back( { { #name }, { "PinMAME_StructArray"}, 3, { { #arg1 }, { #arg2 }, { #arg3 } }, \
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant* pRet) { \
         PSC_VAR_SET_PinMAME_StructArray3(type, fieldName1, fieldName2, fieldName3, *pRet, static_cast<_BindedClass*>(me)->Get##name( PSC_VAR_##arg1(pArgs[0]), PSC_VAR_##arg2(pArgs[1]), PSC_VAR_##arg3(pArgs[2]) )); } } );
#define PSC_PROP_R_StructArray3_4(type, fieldName1, fieldName2, fieldName3, name, arg1, arg2, arg3, arg4) \
   members.push_back( { { #name }, { "PinMAME_StructArray" }, 4, { { #arg1 }, { #arg2 }, { #arg3 }, { #arg4 } }, \
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant* pRet) { \
         PSC_VAR_SET_PinMAME_StructArray3(type, fieldName1, fieldName2, fieldName3, *pRet, static_cast<_BindedClass*>(me)->Get##name( PSC_VAR_##arg1(pArgs[0]), PSC_VAR_##arg2(pArgs[1]), PSC_VAR_##arg3(pArgs[2]), PSC_VAR_##arg4(pArgs[3]) )); } } );


PSC_CLASS_START(PinMAME_Controller, Controller)
   // Overall setup
   PSC_PROP_R(string, Version)
   PSC_PROP_RW(string, GameName)
   PSC_PROP_R(string, ROMName)
   PSC_PROP_RW(string, SplashInfoLine)
   PSC_PROP_RW(bool, HandleKeyboard)
   PSC_PROP_RW(int32, HandleMechanics)
   PSC_PROP_R(PinMAME_Settings, Settings)
   PSC_PROP_RW_ARRAY1(int32, SolMask, int)
   // Run/Pause/Stop
   PSC_FUNCTION0(void, Run)
   PSC_FUNCTION1(void, Run, int32)
   PSC_FUNCTION2(void, Run, int32, int)
   PSC_PROP_W(double, TimeFence)
   PSC_PROP_R(bool, Running)
   PSC_PROP_RW(bool, Pause)
   PSC_FUNCTION0(void, Stop)
   PSC_PROP_RW(bool, Hidden)
   // Emulated machine state access
   PSC_PROP_RW_ARRAY1(bool, Switch, int)
   PSC_PROP_W_ARRAY1(int32, Mech, int)
   PSC_PROP_R_ARRAY1(int32, GetMech, int)
   PSC_PROP_R_ARRAY1(bool, Lamp, int)
   PSC_PROP_R_ARRAY1(bool, LampCallback, int)
   PSC_PROP_R_ARRAY1(bool, Solenoid, int)
   PSC_FUNCTION2(void, B2SSetScore, int, int)
   PSC_FUNCTION2(void, B2SSetData, int, int)
   PSC_PROP_R_ARRAY1(int32, GIString, int)
   PSC_PROP_RW_ARRAY1(int32, Dip, int)
   PSC_PROP_R(PinMAME_ByteArray, NVRAM)
   PSC_PROP_R_StructArray3(PinmameNVRAMState, nvramNo, oldStat, currStat, ChangedNVRAM);
   PSC_PROP_R_StructArray2(PinmameLampState, lampNo, state, ChangedLamps);
   PSC_PROP_R_StructArray2(PinmameGIState, giNo, state, ChangedGIStrings);
   PSC_PROP_R_StructArray2(PinmameSolenoidState, solNo, state, ChangedSolenoids);
   PSC_PROP_R_StructArray2(PinmameSoundCommand, sndNo, sndNo, NewSoundCommands); // 2nd field is unused
   PSC_PROP_R_StructArray3_2(PinmameLEDState, ledNo, chgSeg, state, ChangedLEDs, int, int);
   PSC_PROP_R_StructArray3_3(PinmameLEDState, ledNo, chgSeg, state, ChangedLEDs, int, int, int);
   PSC_PROP_R_StructArray3_4(PinmameLEDState, ledNo, chgSeg, state, ChangedLEDs, int, int, int, int);
   PSC_PROP_R(int, RawDmdWidth)
   PSC_PROP_R(int, RawDmdHeight)
   PSC_PROP_R(PinMAME_ByteArray, RawDmdPixels)
   PSC_PROP_R(PinMAME_IntArray, RawDmdColoredPixels)
   // Overall information
   PSC_PROP_R_ARRAY1(PinMAME_Game, Games, string)
   // Deprecated properties
   PSC_PROP_RW(bool, DoubleSize)
   PSC_PROP_RW(bool, LockDisplay)
   PSC_PROP_RW(bool, ShowFrame)
   PSC_PROP_RW(bool, ShowDMDOnly)
   PSC_PROP_RW(bool, ShowTitle)
   PSC_PROP_RW(int, FastFrames)
   PSC_PROP_RW(bool, IgnoreRomCrc)
   PSC_PROP_RW(bool, CabinetMode)
   PSC_PROP_RW(int, SoundMode)
   PSC_FUNCTION0(void, ShowOptsDialog)
   PSC_FUNCTION1(void, ShowOptsDialog, int32)
   // Custom property to allow host to identify the object as the plugin version
   PSC_PROP_R(bool, IsPlugin)
PSC_CLASS_END()


///////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin interface

static const MsgPluginAPI* msgApi = nullptr;
static ScriptablePluginAPI* scriptApi = nullptr;
static unsigned int getScriptApiMsgId = 0;
static unsigned int getVpxApiMsgId = 0;

static uint32_t endpointId;

static Controller* controller = nullptr;

// Host-side NVRAM query (subscribed in Load, unsubscribed in Unload). Lets
// the host (VPinballLib) request the running ROM's current NVRAM bytes
// without going through the script API or the .nv file on disk.
//
// Payload struct must match the host's declaration; intentionally simple
// POD so we don't have to share a header between host and plugin.
struct PinMAMENvramQuery {
   uint8_t* buffer;       // caller-provided output buffer
   int maxBytes;          // capacity of buffer
   int bytesWritten;      // filled by callback (0 if not a ROM table)
   int isNvramTable;      // filled by callback (1 if a Controller is running)
};
static unsigned int getNvramMsgId = 0;
static void OnGetNvramRequest(const unsigned int /*msgId*/, void* /*context*/, void* msgData)
{
   PinMAMENvramQuery* q = static_cast<PinMAMENvramQuery*>(msgData);
   if (q == nullptr) return;
   q->bytesWritten = 0;
   q->isNvramTable = (controller != nullptr) ? 1 : 0;
   if (controller == nullptr || q->buffer == nullptr || q->maxBytes <= 0) return;
   const auto bytes = controller->GetNVRAM();
   const int n = static_cast<int>(bytes.size());
   const int copyBytes = (n < q->maxBytes) ? n : q->maxBytes;
   memcpy(q->buffer, bytes.data(), copyBytes);
   q->bytesWritten = copyBytes;
}

// Host-side direct switch write (mirrors the GET_NVRAM pattern). Lets the
// host (VPinballLib) drive a PinMAME switch as a level signal — used by the
// in-game coin-door latch so tables with toggleKeyCoinDoor=False (real-cabinet
// mode) see the door as held open instead of momentarily pulsed.
//
// Payload struct must match the host's declaration in VPinballLib_C.cpp.
struct PinMAMESwitchSet {
   int switchNum;
   int state;
   int handled;
};
static unsigned int setSwitchMsgId = 0;
static void OnSetSwitchRequest(const unsigned int /*msgId*/, void* /*context*/, void* msgData)
{
   PinMAMESwitchSet* s = static_cast<PinMAMESwitchSet*>(msgData);
   if (s == nullptr) {
      LOGE("[SetSwitch] msgData=nullptr"s);
      return;
   }
   s->handled = 0;
   if (controller == nullptr) {
      LOGE(std::format("[SetSwitch] no controller running, sw={} state={} ignored", s->switchNum, s->state));
      return;
   }
#if PINMAME_DEBUG_LOG
   const bool prev = controller->GetSwitch(s->switchNum);
#endif
   controller->SetSwitch(s->switchNum, s->state != 0);
#if PINMAME_DEBUG_LOG
   const bool now = controller->GetSwitch(s->switchNum);
   LOGI(std::format("[SetSwitch] sw={} state={} (prev={} → now={})", s->switchNum, s->state, prev ? 1 : 0, now ? 1 : 0));
#endif
   s->handled = 1;
}

// Host-side direct switch read (matches OnSetSwitchRequest but read-only).
// Payload struct must match the host's declaration in VPinballLib_C.cpp.
struct PinMAMESwitchGet {
   int switchNum;
   int value;
   int handled;
};
static unsigned int getSwitchMsgId = 0;
static void OnGetSwitchRequest(const unsigned int /*msgId*/, void* /*context*/, void* msgData)
{
   PinMAMESwitchGet* g = static_cast<PinMAMESwitchGet*>(msgData);
   if (g == nullptr) return;
   g->handled = 0;
   if (controller == nullptr) return;
   g->value = controller->GetSwitch(g->switchNum) ? 1 : 0;
   g->handled = 1;
#if PINMAME_DEBUG_LOG
   LOGI(std::format("[GetSwitch] sw={} value={}", g->switchNum, g->value));
#endif
}

PSC_ERROR_IMPLEMENT(scriptApi); // Implement script error

LPI_IMPLEMENT_CPP // Implement shared log support

MSGPI_STRING_VAL_SETTING(pinMAMEPathProp, "PinMAMEPath", "PinMAME Path", "Folder that contains PinMAME subfolders (roms, nvram, ...)", true, "", 1024);
MSGPI_BOOL_VAL_SETTING(cheatProp, "Cheat", "Cheat Mode", "", true, false);
// Output sample rate from PinMAME, in Hz. The default 0 means "auto":
// on Android the host audio engine (AAudio) runs at 48000 so we pick that
// to skip one resample stage in our pipeline. Manual values worth trying
// when a ROM sounds rough:
//   22050 — pre-DCS WPC (Creature From The Black Lagoon, Funhouse, Twilight Zone)
//   32000 — DCS / DCS-95 Williams (Indiana Jones, Star Wars, Medieval Madness)
//   48000 — modern Stern SAM/SPIKE; matches Android device rate
//   96000 — highest quality, double the CPU cost in PinMAME's resampler
// PinMAME 3.6+ resamples any output rate cleanly, so this is mostly about
// matching downstream stages to avoid stacked non-integer ratios.
MSGPI_INT_VAL_SETTING(audioSampleRateProp, "AudioSampleRate", "Audio Sample Rate (Hz)",
   "Output sample rate from PinMAME, in Hz. 0 = auto (matches the audio device, typically 48000 on Android). "
   "Common manual values: 22050 for pre-DCS WPC tables (Creature From The Black Lagoon, Funhouse, Twilight Zone), "
   "32000 for DCS Williams (Indiana Jones, Medieval Madness), 48000 for modern Stern, 96000 for max quality.",
   true, 0, 96000, 0);

void PINMAMECALLBACK OnLogMessage(PINMAME_LOG_LEVEL logLevel, const char* format, va_list args, void* const pUserData)
{
   va_list args_copy;
   va_copy(args_copy, args);
   int size = vsnprintf(nullptr, 0, format, args_copy);
   va_end(args_copy);
   if (size > 0) {
      string buffer(size + 1, '\0');
      vsnprintf(buffer.data(), size + 1, format, args);
      buffer.pop_back(); // remove null terminator
      if (buffer.starts_with("Average FPS:"s))
      {
         // Skip as the FPS does not correspond to anything here
      }
      else if (logLevel == PINMAME_LOG_LEVEL_INFO)
      {
         LOGI(buffer);
      }
      else if (logLevel == PINMAME_LOG_LEVEL_ERROR)
      {
         LOGE(buffer);
      }
   }
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// Overall game messages

static void OnControllerGameStart(Controller*)
{
   assert(controller->GetRunning());

}

static void OnControllerGameEnd(Controller*)
{
}

static void OnControllerDestroyed(Controller*)
{
   controller = nullptr;
}

}


///////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin lifecycle

using namespace PinMAME;

MSGPI_EXPORT void MSGPIAPI PinMAMEPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   controller = nullptr;
   endpointId = sessionId;
   msgApi = api;

   // Optional VPX API
   getVpxApiMsgId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API);

   // Request and setup shared login API
   LPISetup(endpointId, msgApi);

   msgApi->RegisterSetting(endpointId, &pinMAMEPathProp);
   msgApi->RegisterSetting(endpointId, &cheatProp);
   msgApi->RegisterSetting(endpointId, &audioSampleRateProp);

   // Contribute our API to the script engine
   getScriptApiMsgId = msgApi->GetMsgID(SCRIPTPI_NAMESPACE, SCRIPTPI_MSG_GET_API);
   msgApi->BroadcastMsg(endpointId, getScriptApiMsgId, &scriptApi);
   auto regLambda = [](ScriptClassDef* scd) { scriptApi->RegisterScriptClass(scd); };
   auto arrayLambda = [](ScriptArrayDef* sad) { scriptApi->RegisterScriptArrayType(sad); };
   RegisterPinMAME_Rom(regLambda);
   RegisterPinMAME_Roms(regLambda);
   RegisterPinMAME_Game(regLambda);
   RegisterPinMAME_Games(regLambda);
   RegisterPinMAME_Settings(regLambda);
   RegisterPinMAME_GameSettings(regLambda);
   RegisterPinMAME_Controller(regLambda);
   RegisterPinMAME_ControllerSettings(regLambda);
   RegisterPinMAME_ByteArray(arrayLambda);
   RegisterPinMAME_IntArray(arrayLambda);
   RegisterPinMAME_StructArray(arrayLambda);
   PinMAME_Controller_SCD->CreateObject = []()
   {
      assert(controller == nullptr); // We do not support having multiple instance running concurrently

      // Pick the output sample rate. 0 in the INI means "auto" — on Android
      // we default to 48000 to match AAudio's device rate so miniaudio does no
      // resampling. Without this auto path the host pipeline would do
      // PinMAME (rom rate → 44100) → miniaudio (44100 → 48000), stacking two
      // non-integer ratios; bad tables already at the edge of PinMAME's
      // internal resampler can audibly suffer.
      int requestedSampleRate = audioSampleRateProp_Val;
      if (requestedSampleRate <= 0)
      {
#ifdef __ANDROID__
         requestedSampleRate = 48000;
#else
         requestedSampleRate = 44100;
#endif
      }
      LOGI(std::format("PinMAME audio sample rate: {} Hz (user setting: {}, 0=auto)", requestedSampleRate, audioSampleRateProp_Val));

      PinmameConfig config = {
         PINMAME_AUDIO_FORMAT_INT16,
         requestedSampleRate,
         "",
         NULL, // State update => prefer update on request
         NULL, // Display available => prefer state block
         NULL, // Display updated => prefer update on request
         NULL, //
         NULL, //
         NULL, // Mech available
         NULL, // Mech updated
         NULL, // Solenoid updated => prefer update on request
         NULL, // Console updated => TODO implement (for Stern SAM)
         NULL, // Is key pressed => TODO implement ?
         &OnLogMessage,
         NULL, // Sound command callback - libpinmame broadcasts via message API directly
      };

      // Define pinmame directory (for ROM, NVRAM, ... eventually using VPX API if available)
      std::filesystem::path pinmamePath;
      std::filesystem::path memmapPath;
      VPXPluginAPI* vpxApi = nullptr;
      msgApi->BroadcastMsg(endpointId, getVpxApiMsgId, &vpxApi);
      
      // Prioritize a pinmame folder along the table
      if (vpxApi != nullptr)
      {
         VPXTableInfo tableInfo;
         vpxApi->GetTableInfo(&tableInfo);
         std::filesystem::path tablePath = tableInfo.path;
         pinmamePath = find_case_insensitive_directory_path(tablePath.parent_path() / "pinmame"sv / "roms"sv);
         if (!pinmamePath.empty())
            pinmamePath = pinmamePath.parent_path();
         memmapPath = find_case_insensitive_directory_path(tablePath.parent_path() / "pinmame"sv / "memmaps"sv);
      }

      // Defaults to the global setting
      if (pinmamePath.empty())
         pinmamePath = pinMAMEPathProp_Get();
      if (memmapPath.empty())
         memmapPath = std::filesystem::path(pinMAMEPathProp_Get()) / "memmaps"sv;

      // Custom platforms defaults
      #if (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__)
      if (pinmamePath.empty() && vpxApi != nullptr)
      {
         VPXInfo vpxInfo;
         vpxApi->GetVpxInfo(&vpxInfo);
         pinmamePath = find_case_insensitive_directory_path(std::filesystem::path(vpxInfo.prefPath) / "pinmame"sv);
      }
      #elif defined(__APPLE__) || defined(__linux__)
      if (pinmamePath.empty())
         pinmamePath = std::filesystem::path(getenv("HOME")) / ".pinmame"sv;
      #endif

      // FIXME implement a last resort or just ask the user to define its path setup in the settings ?
      if (pinmamePath.empty())
         LOGE("PinMAME path is not defined."s);
      else
         strncpy_s(const_cast<char*>(config.vpmPath), PINMAME_MAX_PATH, (pinmamePath / ""sv).string().c_str());

      Controller* pController = new Controller(msgApi, endpointId, config, memmapPath);
      pController->SetOnDestroyHandler(OnControllerDestroyed);
      pController->SetOnGameStartHandler(OnControllerGameStart);
      pController->SetOnGameEndHandler(OnControllerGameEnd);
      pController->SetCheat(cheatProp_Val);
      controller = pController;

      return static_cast<void*>(pController);
   };
   scriptApi->SubmitTypeLibrary(endpointId);
   scriptApi->SetCOMObjectOverride("VPinMAME.Controller", PinMAME_Controller_SCD);

   PinmameSetMsgAPI(const_cast<MsgPluginAPI*>(msgApi), endpointId);

   // Host-side NVRAM query subscription.
   getNvramMsgId = msgApi->GetMsgID("VPINBALL", "GET_NVRAM");
   msgApi->SubscribeMsg(endpointId, getNvramMsgId, &OnGetNvramRequest, nullptr);

   // Host-side direct switch write subscription (used by the in-game coin-door
   // latch in com.retroeki.pinball — see project memory
   // "coin-door-key-mode-mismatch").
   setSwitchMsgId = msgApi->GetMsgID("VPINBALL", "SET_SWITCH");
   msgApi->SubscribeMsg(endpointId, setSwitchMsgId, &OnSetSwitchRequest, nullptr);

   // Host-side direct switch read — pairs with SET_SWITCH; the host uses it to
   // seed UI state from the ROM-side switch matrix (e.g. coin-door image).
   getSwitchMsgId = msgApi->GetMsgID("VPINBALL", "GET_SWITCH");
   msgApi->SubscribeMsg(endpointId, getSwitchMsgId, &OnGetSwitchRequest, nullptr);
}

MSGPI_EXPORT void MSGPIAPI PinMAMEPluginUnload()
{
   if (controller)
   {
      int nRemainingRef = 0;
      while (controller)
      {
         controller->Release();
         nRemainingRef++;
      }
      LOGE(std::format("PinMAME Controller was not destroyed before unloading the plugin ({} remaining references)", nRemainingRef));
   }

   if (getNvramMsgId != 0) {
      msgApi->UnsubscribeMsg(getNvramMsgId, &OnGetNvramRequest, nullptr);
      msgApi->ReleaseMsgID(getNvramMsgId);
      getNvramMsgId = 0;
   }

   if (setSwitchMsgId != 0) {
      msgApi->UnsubscribeMsg(setSwitchMsgId, &OnSetSwitchRequest, nullptr);
      msgApi->ReleaseMsgID(setSwitchMsgId);
      setSwitchMsgId = 0;
   }

   if (getSwitchMsgId != 0) {
      msgApi->UnsubscribeMsg(getSwitchMsgId, &OnGetSwitchRequest, nullptr);
      msgApi->ReleaseMsgID(getSwitchMsgId);
      getSwitchMsgId = 0;
   }

   scriptApi->SetCOMObjectOverride("VPinMAME.Controller", nullptr);
   auto regLambda = [](ScriptClassDef* scd) { scriptApi->UnregisterScriptClass(scd); };
   auto arrayLambda = [](ScriptArrayDef* sad) { scriptApi->UnregisterScriptArrayType(sad); };
   UnregisterPinMAME_Rom(regLambda);
   UnregisterPinMAME_Roms(regLambda);
   UnregisterPinMAME_Game(regLambda);
   UnregisterPinMAME_Games(regLambda);
   UnregisterPinMAME_Settings(regLambda);
   UnregisterPinMAME_GameSettings(regLambda);
   UnregisterPinMAME_Controller(regLambda);
   UnregisterPinMAME_ControllerSettings(regLambda);
   UnregisterPinMAME_ByteArray(arrayLambda);
   UnregisterPinMAME_IntArray(arrayLambda);
   UnregisterPinMAME_StructArray(arrayLambda);

   msgApi->ReleaseMsgID(getVpxApiMsgId);
   msgApi->ReleaseMsgID(getScriptApiMsgId);
   PinmameSetMsgAPI(nullptr, 0);
   msgApi = nullptr;
}

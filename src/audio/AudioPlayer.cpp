// license:GPLv3+

#include "core/stdafx.h"
#include "AudioPlayer.h"
#include "AudioStreamPlayer.h"
#include "SoundPlayer.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>
#ifdef __ANDROID__
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_CUSTOM
#ifdef __ANDROID__
// On Android we use miniaudio's built-in AAudio backend so our mixing runs
// inside AAudio's privileged data-callback thread (SCHED_FIFO, granted by the
// audio framework when LOW_LATENCY perfMode is requested). That bypasses SDL's
// audio worker thread which runs at nice -20 / SCHED_OTHER and gets preempted
// by Android View / GPU work happening in the same :native_process.
#define MA_ENABLE_AAUDIO
#endif
#include "miniaudio/extras/stb_vorbis.c"
#include "miniaudio/miniaudio.h"
#include "miniaudio/miniaudio.c"

// Simple SDL3 backend for miniaudio, derived from miniaudio's backend example

struct ma_device_ex
{
   ma_device device; // Make this the first member so we can cast between ma_device and ma_device_ex.
   SDL_AudioDeviceID deviceID;
   SDL_AudioStream* stream;
   vector<uint8_t> buffer;

   // Audio-thread diagnostics. Single-writer (the SDL audio worker thread)
   // so no synchronization needed. Used to isolate app-side mixing slowness
   // from platform-side HAL/DSP throttling when audio pops are reported.
   uint64_t diagLastCallbackNs = 0;
   uint64_t diagPeriodNs = 0;          // expected gap between callbacks
   uint64_t diagWindowStartNs = 0;     // start of current ~5s summary window
   uint64_t diagLastImmediateLogNs = 0; // rate-limit on per-event log emission
   uint32_t diagCallbackCount = 0;
   uint32_t diagSlowCallbacks = 0;     // mixing time > 25% of period
   uint32_t diagLateGaps = 0;          // inter-arrival > 1.5x period
   uint64_t diagMaxMixNs = 0;
   uint64_t diagMaxGapNs = 0;
   const char* diagLabel = "";

   // One-shot flag: first audio callback on this device tries to promote the
   // SDL audio worker thread to a real-time scheduling class so the kernel
   // stops preempting it for normal threads. Logs the actual outcome.
   bool threadPriorityAttempted = false;
};

static ma_result ma_context_enumerate_devices__sdl(ma_context* pContext, ma_enum_devices_callback_proc callback, void* pUserData)
{
   int count;
   auto pAudioList = SDL_GetAudioPlaybackDevices(&count);
   if (pAudioList == nullptr)
      return MA_ERROR;
   for (int i = 0; i < count; ++i)
   {
      ma_device_info deviceInfo;
      MA_ZERO_OBJECT(&deviceInfo);
      deviceInfo.id.custom.i = pAudioList[i];
      ma_strncpy_s(deviceInfo.name, sizeof(deviceInfo.name), SDL_GetAudioDeviceName(pAudioList[i]), (size_t)-1);
      ma_bool32 cbResult = callback(pContext, ma_device_type_playback, &deviceInfo, pUserData);
      if (cbResult == MA_FALSE)
         break;
   }
   SDL_free(pAudioList);
   return MA_SUCCESS;
}

static ma_result ma_context_get_device_info__sdl(ma_context* pContext, ma_device_type deviceType, const ma_device_id* pDeviceID, ma_device_info* pDeviceInfo)
{
   if (deviceType != ma_device_type_playback)
      return MA_DEVICE_TYPE_NOT_SUPPORTED;

   if (pDeviceID == nullptr)
   {
      pDeviceInfo->id.custom.i = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
      ma_strncpy_s(pDeviceInfo->name, sizeof(pDeviceInfo->name), MA_DEFAULT_PLAYBACK_DEVICE_NAME, (size_t)-1);
   }
   else
   {
      pDeviceInfo->id.custom.i = pDeviceID->custom.i;
      ma_strncpy_s(pDeviceInfo->name, sizeof(pDeviceInfo->name), SDL_GetAudioDeviceName(pDeviceID->custom.i), (size_t)-1);
   }
   if (pDeviceInfo->id.custom.i == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)
      pDeviceInfo->isDefault = MA_TRUE;

   SDL_AudioSpec specs;
   if (pDeviceInfo->isDefault)
   {
      SDL_AudioDeviceID tempDeviceID = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
      if (tempDeviceID == 0)
      {
         PLOGE << "Failed to open default SDL device.";
         return MA_FAILED_TO_OPEN_BACKEND_DEVICE;
      }
      SDL_GetAudioDeviceFormat(tempDeviceID, &specs, nullptr);
      SDL_CloseAudioDevice(tempDeviceID);
   }
   else
   {
      SDL_GetAudioDeviceFormat(pDeviceInfo->id.custom.i, &specs, nullptr);
   }

   pDeviceInfo->nativeDataFormatCount = 1;
   pDeviceInfo->nativeDataFormats[0].format = ma_format_f32;
   pDeviceInfo->nativeDataFormats[0].channels = specs.channels;
   pDeviceInfo->nativeDataFormats[0].sampleRate = specs.freq;
   pDeviceInfo->nativeDataFormats[0].flags = 0;

   return MA_SUCCESS;
}

#ifdef __ANDROID__
// Called on the SDL audio worker thread the first time we see it. Tries to
// upgrade the thread's scheduling class so the kernel doesn't preempt it for
// normal threads. SCHED_FIFO usually requires CAP_SYS_NICE on Android; if it
// fails we fall back to maxing the nice value. Also pins the thread to the
// big-core cluster so we don't share a core with Compose / GC work on the
// little cluster. Logs the actual outcome.
static void TryPromoteAudioThread(const char* label)
{
   const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));

   // --- 1. Scheduling class / priority ---
   const int oldPolicy = sched_getscheduler(0);
   const int oldNice = getpriority(PRIO_PROCESS, 0);

   struct sched_param param = {};
   param.sched_priority = 1; // Lowest SCHED_FIFO priority; we don't need to outrank AAudio's own thread.
   const int rcFifo = sched_setscheduler(0, SCHED_FIFO, &param);
   const int fifoErrno = (rcFifo == 0) ? 0 : errno;
   if (rcFifo == 0)
   {
      const int newPolicy = sched_getscheduler(0);
      PLOGI << "[AudioDiag " << label << "] tid=" << tid
            << " promoted to SCHED_FIFO (was policy=" << oldPolicy
            << ", nice=" << oldNice << ", now policy=" << newPolicy
            << ", prio=" << param.sched_priority << ')';
   }
   else
   {
      const int rcNice = setpriority(PRIO_PROCESS, 0, -20);
      const int niceErrno = (rcNice == 0) ? 0 : errno;
      const int newNice = getpriority(PRIO_PROCESS, 0);
      PLOGI << "[AudioDiag " << label << "] tid=" << tid
            << " SCHED_FIFO denied (errno=" << fifoErrno
            << "), setpriority(-20) rc=" << rcNice
            << " errno=" << niceErrno
            << " policy=" << oldPolicy << " oldNice=" << oldNice << " newNice=" << newNice;
   }

   // --- 2. CPU affinity: pin to big cores ---
   // ARM big.LITTLE convention on recent Snapdragon: 0-3 little (A510 efficient),
   // 4-6 big (A710/A715 performance), 7 prime (X3/X4). Pin to 4-7 so the audio
   // thread stays out of the throttle-prone little cluster.
   cpu_set_t currentMask;
   CPU_ZERO(&currentMask);
   const int rcGet = sched_getaffinity(0, sizeof(currentMask), &currentMask);
   const int currentCount = (rcGet == 0) ? CPU_COUNT(&currentMask) : -1;

   cpu_set_t newMask;
   CPU_ZERO(&newMask);
   int pinned = 0;
   for (int cpu = 4; cpu < CPU_SETSIZE && cpu < 8; ++cpu)
   {
      // Only set bits the kernel currently allows us to use, so a device with
      // fewer than 8 cores or one that's offlined a big core doesn't trip us.
      if (rcGet == 0 && CPU_ISSET(cpu, &currentMask))
      {
         CPU_SET(cpu, &newMask);
         ++pinned;
      }
   }

   if (pinned == 0)
   {
      PLOGI << "[AudioDiag " << label << "] affinity skip: no big cores available "
            << "(getaffinity rc=" << rcGet << ", currentCount=" << currentCount << ')';
      return;
   }

   const int rcSet = sched_setaffinity(0, sizeof(newMask), &newMask);
   const int setErrno = (rcSet == 0) ? 0 : errno;

   // Read back to confirm what actually stuck.
   cpu_set_t actualMask;
   CPU_ZERO(&actualMask);
   sched_getaffinity(0, sizeof(actualMask), &actualMask);

   PLOGI << "[AudioDiag " << label << "] affinity: setaffinity rc=" << rcSet
         << " errno=" << setErrno
         << " requested=4-7 (" << pinned << " bits)"
         << " actualMask=" << std::hex
         << (CPU_ISSET(0, &actualMask) ? "0" : "")
         << (CPU_ISSET(1, &actualMask) ? "1" : "")
         << (CPU_ISSET(2, &actualMask) ? "2" : "")
         << (CPU_ISSET(3, &actualMask) ? "3" : "")
         << (CPU_ISSET(4, &actualMask) ? "4" : "")
         << (CPU_ISSET(5, &actualMask) ? "5" : "")
         << (CPU_ISSET(6, &actualMask) ? "6" : "")
         << (CPU_ISSET(7, &actualMask) ? "7" : "")
         << std::dec;
}
#endif

void ma_audio_callback_playback__sdl(void* pUserData, SDL_AudioStream* stream, int additional_amount, const int total_amount)
{
   auto pDevice = static_cast<ma_device_ex*>(pUserData);

#ifdef __ANDROID__
   if (!pDevice->threadPriorityAttempted)
   {
      pDevice->threadPriorityAttempted = true;
      TryPromoteAudioThread(pDevice->diagLabel);
   }
#endif

   const uint64_t entryNs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
         std::chrono::steady_clock::now().time_since_epoch()).count());

   // Resizing on the audio thread would malloc; we pre-size in ma_device_init__sdl,
   // but keep a defensive resize in case the device asks for a larger chunk.
   if ((int)pDevice->buffer.size() < total_amount)
      pDevice->buffer.resize(total_amount);

   const int sizePerMAFrame = ma_get_bytes_per_frame(pDevice->device.playback.internalFormat, pDevice->device.playback.internalChannels);
   const int nFrames = total_amount / sizePerMAFrame;
   ma_device__read_frames_from_client(&pDevice->device, nFrames, pDevice->buffer.data());
   SDL_PutAudioStreamData(stream, pDevice->buffer.data(), nFrames * sizePerMAFrame);

   const uint64_t exitNs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
         std::chrono::steady_clock::now().time_since_epoch()).count());
   const uint64_t mixNs = exitNs - entryNs;
   const uint64_t gapNs = pDevice->diagLastCallbackNs ? entryNs - pDevice->diagLastCallbackNs : 0;
   pDevice->diagLastCallbackNs = entryNs;

   if (pDevice->diagWindowStartNs == 0)
      pDevice->diagWindowStartNs = entryNs;

   pDevice->diagCallbackCount++;
   if (mixNs > pDevice->diagMaxMixNs) pDevice->diagMaxMixNs = mixNs;
   if (gapNs > pDevice->diagMaxGapNs) pDevice->diagMaxGapNs = gapNs;

   // Tighter thresholds than the original: slow = mixing exceeded 25% of period budget
   // (still well under the deadline, but warns us we're trending up); late = gap exceeded
   // 1.5x period, the smallest gap that can produce audible underrun.
   const uint64_t slowThresholdNs = pDevice->diagPeriodNs ? (pDevice->diagPeriodNs / 4) : 1000000;
   const uint64_t lateThresholdNs = pDevice->diagPeriodNs ? (pDevice->diagPeriodNs * 3 / 2) : 24000000;
   const bool isSlow = mixNs > slowThresholdNs;
   const bool isLate = gapNs > lateThresholdNs;
   if (isSlow) pDevice->diagSlowCallbacks++;
   if (isLate) pDevice->diagLateGaps++;

   // Immediate emission on a slow or late callback, rate-limited so a sustained
   // problem doesn't flood the log. Catches one-off events (e.g. menu transitions)
   // that the 5-second summary would otherwise swallow.
   if (isSlow || isLate)
   {
      const uint64_t sinceLastImmediateNs = entryNs - pDevice->diagLastImmediateLogNs;
      if (sinceLastImmediateNs > 250000000ULL)
      {
         pDevice->diagLastImmediateLogNs = entryNs;
         PLOGI << "[AudioDiag " << pDevice->diagLabel << "] "
               << (isSlow ? "SLOW " : "")
               << (isLate ? "LATE " : "")
               << "mix=" << (mixNs / 1000) << "us"
               << " gap=" << (gapNs / 1000) << "us"
               << " period=" << (pDevice->diagPeriodNs / 1000) << "us"
               << " thresholds=slow>" << (slowThresholdNs / 1000) << "us/late>" << (lateThresholdNs / 1000) << "us";
      }
   }

   // 5-second summary if anything triggered in the window.
   const uint64_t windowNs = entryNs - pDevice->diagWindowStartNs;
   if (windowNs >= 5000000000ULL)
   {
      if (pDevice->diagSlowCallbacks > 0 || pDevice->diagLateGaps > 0)
      {
         PLOGI << "[AudioDiag " << pDevice->diagLabel << "] window cb=" << pDevice->diagCallbackCount
               << " slow=" << pDevice->diagSlowCallbacks
               << " late=" << pDevice->diagLateGaps
               << " maxMix=" << (pDevice->diagMaxMixNs / 1000) << "us"
               << " maxGap=" << (pDevice->diagMaxGapNs / 1000) << "us"
               << " period=" << (pDevice->diagPeriodNs / 1000) << "us";
      }
      pDevice->diagWindowStartNs = entryNs;
      pDevice->diagCallbackCount = 0;
      pDevice->diagSlowCallbacks = 0;
      pDevice->diagLateGaps = 0;
      pDevice->diagMaxMixNs = 0;
      pDevice->diagMaxGapNs = 0;
   }
}

#ifdef __ANDROID__
// AAudio data callback. miniaudio calls this from AAudio's realtime SCHED_FIFO
// thread (granted because we requested LOW_LATENCY perfMode). The actual
// mixing is delegated to ma_engine_data_callback_internal; this wrapper only
// adds the same timing diagnostic we use on the SDL custom backend so we can
// verify the priority gain empirically.
static void ma_data_callback_aaudio_diag(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
   auto pDeviceEx = reinterpret_cast<ma_device_ex*>(pDevice);

   // First call: log what scheduling policy we actually landed on so we know
   // whether AAudio actually gave us SCHED_FIFO.
   if (!pDeviceEx->threadPriorityAttempted)
   {
      pDeviceEx->threadPriorityAttempted = true;
      const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
      const int policy = sched_getscheduler(0);
      const int nice = getpriority(PRIO_PROCESS, 0);
      PLOGI << "[AudioDiag " << pDeviceEx->diagLabel << "] AAudio data callback thread tid=" << tid
            << " policy=" << policy << " (0=OTHER,1=FIFO,2=RR)"
            << " nice=" << nice;
      // Lazily compute the expected period from frameCount the first time we see it.
      const ma_uint32 sampleRate = pDevice->playback.internalSampleRate;
      if (sampleRate > 0 && pDeviceEx->diagPeriodNs == 0)
         pDeviceEx->diagPeriodNs = static_cast<uint64_t>(frameCount) * 1000000000ULL / sampleRate;
   }

   const uint64_t entryNs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
         std::chrono::steady_clock::now().time_since_epoch()).count());

   // Run the actual mixing on AAudio's realtime thread.
   ma_engine_data_callback_internal(pDevice, pOutput, pInput, frameCount);

   const uint64_t exitNs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
         std::chrono::steady_clock::now().time_since_epoch()).count());
   const uint64_t mixNs = exitNs - entryNs;
   const uint64_t gapNs = pDeviceEx->diagLastCallbackNs ? entryNs - pDeviceEx->diagLastCallbackNs : 0;
   pDeviceEx->diagLastCallbackNs = entryNs;

   if (pDeviceEx->diagWindowStartNs == 0)
      pDeviceEx->diagWindowStartNs = entryNs;

   pDeviceEx->diagCallbackCount++;
   if (mixNs > pDeviceEx->diagMaxMixNs) pDeviceEx->diagMaxMixNs = mixNs;
   if (gapNs > pDeviceEx->diagMaxGapNs) pDeviceEx->diagMaxGapNs = gapNs;

   const uint64_t slowThresholdNs = pDeviceEx->diagPeriodNs ? (pDeviceEx->diagPeriodNs / 4) : 1000000;
   const uint64_t lateThresholdNs = pDeviceEx->diagPeriodNs ? (pDeviceEx->diagPeriodNs * 3 / 2) : 24000000;
   const bool isSlow = mixNs > slowThresholdNs;
   const bool isLate = gapNs > lateThresholdNs;
   if (isSlow) pDeviceEx->diagSlowCallbacks++;
   if (isLate) pDeviceEx->diagLateGaps++;

   if (isSlow || isLate)
   {
      const uint64_t sinceLastImmediateNs = entryNs - pDeviceEx->diagLastImmediateLogNs;
      if (sinceLastImmediateNs > 250000000ULL)
      {
         pDeviceEx->diagLastImmediateLogNs = entryNs;
         PLOGI << "[AudioDiag " << pDeviceEx->diagLabel << "] "
               << (isSlow ? "SLOW " : "")
               << (isLate ? "LATE " : "")
               << "mix=" << (mixNs / 1000) << "us"
               << " gap=" << (gapNs / 1000) << "us"
               << " period=" << (pDeviceEx->diagPeriodNs / 1000) << "us";
      }
   }

   const uint64_t windowNs = entryNs - pDeviceEx->diagWindowStartNs;
   if (windowNs >= 5000000000ULL)
   {
      if (pDeviceEx->diagSlowCallbacks > 0 || pDeviceEx->diagLateGaps > 0)
      {
         PLOGI << "[AudioDiag " << pDeviceEx->diagLabel << "] window cb=" << pDeviceEx->diagCallbackCount
               << " slow=" << pDeviceEx->diagSlowCallbacks
               << " late=" << pDeviceEx->diagLateGaps
               << " maxMix=" << (pDeviceEx->diagMaxMixNs / 1000) << "us"
               << " maxGap=" << (pDeviceEx->diagMaxGapNs / 1000) << "us"
               << " period=" << (pDeviceEx->diagPeriodNs / 1000) << "us";
      }
      pDeviceEx->diagWindowStartNs = entryNs;
      pDeviceEx->diagCallbackCount = 0;
      pDeviceEx->diagSlowCallbacks = 0;
      pDeviceEx->diagLateGaps = 0;
      pDeviceEx->diagMaxMixNs = 0;
      pDeviceEx->diagMaxGapNs = 0;
   }
}
#endif

static ma_result ma_device_init__sdl(ma_device* pDevice, const ma_device_config* pConfig, ma_device_descriptor* pDescriptorPlayback, ma_device_descriptor* pDescriptorCapture)
{
   if (pConfig->deviceType != ma_device_type_playback)
      return MA_DEVICE_TYPE_NOT_SUPPORTED;

   auto pDeviceEx = reinterpret_cast<ma_device_ex*>(pDevice);

   auto requestedDeviceId = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
   if (pConfig->playback.pDeviceID)
      requestedDeviceId = pConfig->playback.pDeviceID->custom.i;

   pDeviceEx->stream = SDL_OpenAudioDeviceStream(requestedDeviceId, nullptr, ma_audio_callback_playback__sdl, pDeviceEx);
   if (pDeviceEx->stream == nullptr)
   {
      PLOGE << "Failed to open SDL audio device (Error: " << SDL_GetError() << ')';
      return MA_FAILED_TO_OPEN_BACKEND_DEVICE;
   }
   pDeviceEx->deviceID = SDL_GetAudioStreamDevice(pDeviceEx->stream);
   int periodSizeInFrames;
   SDL_AudioSpec specs;
   SDL_GetAudioDeviceFormat(pDeviceEx->deviceID, &specs, &periodSizeInFrames);
   
   // Convert SDL format to miniaudio format
   ma_format deviceFormat;
   switch (specs.format)
   {
      case SDL_AUDIO_U8: deviceFormat = ma_format_u8; break;
      case SDL_AUDIO_S16: deviceFormat = ma_format_s16; break;
      case SDL_AUDIO_S32: deviceFormat = ma_format_s32; break;
      case SDL_AUDIO_F32: deviceFormat = ma_format_f32; break;
      default:
         PLOGI << "Unsupported SDL audio format " << SDL_GetAudioFormatName(specs.format) << " (0x" << std::hex << specs.format << std::dec << "), forcing to F32";
         specs.format = SDL_AUDIO_F32;
         if (!SDL_SetAudioStreamFormat(pDeviceEx->stream, nullptr, &specs))
         {
            PLOGE << "Failed to set audio stream format to F32";
            SDL_DestroyAudioStream(pDeviceEx->stream);
            return MA_FAILED_TO_OPEN_BACKEND_DEVICE;
         }
         deviceFormat = ma_format_f32;
         break;
   }

   // Update miniaudio descriptor with actual device settings
   pDescriptorPlayback->format = deviceFormat;
   pDescriptorPlayback->channels = specs.channels;
   pDescriptorPlayback->sampleRate = static_cast<ma_uint32>(specs.freq);
   pDescriptorPlayback->periodSizeInFrames = periodSizeInFrames;
   pDescriptorPlayback->periodCount = 1; // SDL doesn't use the notion of period counts, so just set to 1.

   // TODO check that the default channel map matches SDL channel map
   ma_channel_map_init_standard(ma_standard_channel_map_default, pDescriptorPlayback->channelMap, std::size(pDescriptorPlayback->channelMap), pDescriptorPlayback->channels);

   // Pre-size the mixing buffer so the audio callback never reallocates on the hot path.
   // SDL typically asks for one period at a time, but request bursts can occur on stream
   // restart; size for 4 periods which covers any realistic ask.
   const int frameBytes = ma_get_bytes_per_frame(deviceFormat, specs.channels);
   const size_t preallocBytes = static_cast<size_t>(periodSizeInFrames) * frameBytes * 4;
   pDeviceEx->buffer.reserve(preallocBytes);
   pDeviceEx->buffer.resize(preallocBytes);

   // Stash the expected callback period so the audio-thread diagnostic can flag
   // slow mixing (>50% of period) and late arrivals (>4x period).
   pDeviceEx->diagPeriodNs = specs.freq > 0
      ? (static_cast<uint64_t>(periodSizeInFrames) * 1000000000ULL / static_cast<uint64_t>(specs.freq))
      : 0;

   PLOGI << "Audio device initialized. Device: '" << SDL_GetAudioDeviceName(pDeviceEx->deviceID) << "', Freq : " << specs.freq << ", Format: " << SDL_GetAudioFormatName(specs.format) << ", Channels: " << specs.channels << ", Driver: " << SDL_GetCurrentAudioDriver() << ", period=" << periodSizeInFrames << "fr/" << (pDeviceEx->diagPeriodNs / 1000) << "us, prealloc=" << preallocBytes << "B";
   return MA_SUCCESS;
}

static ma_result ma_device_uninit__sdl(ma_device* pDevice)
{
   if (auto pDeviceEx = reinterpret_cast<ma_device_ex*>(pDevice); pDeviceEx->stream)
      SDL_DestroyAudioStream(pDeviceEx->stream);
   return MA_SUCCESS;
}

static ma_result ma_device_start__sdl(ma_device* pDevice)
{
   if (auto pDeviceEx = reinterpret_cast<ma_device_ex*>(pDevice); pDeviceEx->stream)
      SDL_ResumeAudioStreamDevice(pDeviceEx->stream);
   return MA_SUCCESS;
}

static ma_result ma_device_stop__sdl(ma_device* pDevice)
{
   if (auto pDeviceEx = reinterpret_cast<ma_device_ex*>(pDevice); pDeviceEx->stream)
      SDL_PauseAudioStreamDevice(pDeviceEx->stream);
   return MA_SUCCESS;
}

static ma_result ma_context_uninit__sdl(ma_context* pContext)
{
   SDL_QuitSubSystem(SDL_INIT_AUDIO);
   return MA_SUCCESS;
}

static ma_result ma_context_init__sdl(ma_context* pContext, const ma_context_config* pConfig, ma_backend_callbacks* pCallbacks)
{
   (void)pConfig;
   if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
      return MA_ERROR;
   pCallbacks->onContextInit = ma_context_init__sdl;
   pCallbacks->onContextUninit = ma_context_uninit__sdl;
   pCallbacks->onContextEnumerateDevices = ma_context_enumerate_devices__sdl;
   pCallbacks->onContextGetDeviceInfo = ma_context_get_device_info__sdl;
   pCallbacks->onDeviceInit = ma_device_init__sdl;
   pCallbacks->onDeviceUninit = ma_device_uninit__sdl;
   pCallbacks->onDeviceStart = ma_device_start__sdl;
   pCallbacks->onDeviceStop = ma_device_stop__sdl;
   return MA_SUCCESS;
}



namespace VPX
{

AudioPlayer::AudioPlayer(const string& backglassDevice, const string& playfieldDevice, SoundConfigTypes playfieldSoundMode)
   : m_soundMode3D(playfieldSoundMode)
{
#ifdef __ANDROID__
   // Android: both main mixing AND streamed audio (PinMAME, music, AltSound,
   // PuP) go through miniaudio's AAudio backend. Sound effects run on the
   // ma_engine that the AAudio backend opens; streamed audio rides on top
   // of the same engine via AudioStreamPlayer's custom data source. Every
   // audio sample on Android ultimately mixes inside AAudio's SCHED_FIFO
   // data callback, which is what eliminates the SDL-worker preemption pops.
   (void)backglassDevice;
   (void)playfieldDevice;

   m_maContext = std::make_unique<ma_context>();
   static constexpr ma_backend backends[] = { ma_backend_aaudio };
   ma_context_init(backends, std::size(backends), nullptr, m_maContext.get());
   m_maContext->pUserData = this;
#else
   if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
      return;

   {
      int count;
      SDL_AudioDeviceID* pAudioList = SDL_GetAudioPlaybackDevices(&count);
      for (int i = 0; i < count; ++i)
      { // We identify by name as this is the only stable property (see https://github.com/libsdl-org/SDL/issues/12278)
         string name = SDL_GetAudioDeviceName(pAudioList[i]);
         if (!playfieldDevice.empty() && name == playfieldDevice)
            m_playfieldAudioDevice = pAudioList[i];
         if (!backglassDevice.empty() && name == backglassDevice)
            m_backglassAudioDevice = pAudioList[i];
      }
      SDL_free(pAudioList);
      if (m_playfieldAudioDevice == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)
      {
         PLOGI << "Table sound device was not found (" << playfieldDevice << "), using default: " << GetPlayfieldDeviceName().c_str();
      }
      if (m_backglassAudioDevice == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)
      {
         PLOGI << "Backglass sound device was not found (" << backglassDevice << "), using default: " << GetBackglassDeviceName().c_str();
      }
   }

   ma_context_config contextConfig;
   contextConfig = ma_context_config_init();
   contextConfig.custom.onContextInit = ma_context_init__sdl;

   m_maContext = std::make_unique<ma_context>();
   static constexpr ma_backend backends[] = { ma_backend_custom };
   ma_context_init(backends, std::size(backends), &contextConfig, m_maContext.get());
   m_maContext->pUserData = this;
#endif

#ifndef __ANDROID__
   struct SDLDeviceInfo
   {
      int id;
      ma_device_info dev;
   };
   auto selectDevice = [](ma_context* pContext, ma_device_type deviceType, const ma_device_info* pInfo, void* pUserData) {
      if (auto info = static_cast<SDLDeviceInfo*>(pUserData); pInfo->id.custom.i == info->id)
      {
         info->dev = *pInfo;
         return (ma_bool32)MA_FALSE;
      }
      return (ma_bool32)MA_TRUE;
   };
#endif

   auto initDevice = [&](std::unique_ptr<ma_device_ex>& outDevice, std::unique_ptr<ma_engine>& outEngine,
                         const char* label, [[maybe_unused]] int sdlDeviceID) {
      outDevice = std::make_unique<ma_device_ex>();
      outDevice->diagLabel = label;
      ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
      deviceConfig.playback.format = ma_format_f32;
      deviceConfig.noPreSilencedOutputBuffer = MA_TRUE;
      deviceConfig.noClip = MA_TRUE;
#ifdef __ANDROID__
      // Default playback device. AAudio LOW_LATENCY perfMode is the miniaudio
      // AAudio backend default, which is what we want — that's why AAudio
      // hands us a SCHED_FIFO data callback thread.
      deviceConfig.playback.pDeviceID = nullptr;
      // Tag the stream as interactive game audio. Android's developer guide
      // recommends this for any latency-sensitive playback so the audio
      // framework picks the most aggressive routing it can deliver. Has zero
      // effect on wired output (already on the fast path), but on BT some
      // HALs prefer game-tagged streams for whatever low-latency profile the
      // codec exposes.
      deviceConfig.aaudio.usage = ma_aaudio_usage_game;
      // Short, transient effects (flipper hits, bumper sounds) and music are
      // both legitimate here — sonification fits better since flipper hits
      // dominate volume-wise and benefit most from low-latency routing.
      deviceConfig.aaudio.contentType = ma_aaudio_content_type_sonification;
      const ma_result result = ma_device_init(m_maContext.get(), &deviceConfig, reinterpret_cast<ma_device*>(outDevice.get()));
#else
      SDLDeviceInfo deviceInfo { sdlDeviceID, {} };
      ma_context_get_device_info(m_maContext.get(), ma_device_type_playback, nullptr, &deviceInfo.dev);
      ma_context_enumerate_devices(m_maContext.get(), selectDevice, &deviceInfo);
      deviceConfig.playback.pDeviceID = &deviceInfo.dev.id;
      const ma_result result = ma_device_init(m_maContext.get(), &deviceConfig, reinterpret_cast<ma_device*>(outDevice.get()));
#endif

      if (result == MA_SUCCESS)
      {
         ma_engine_config engineConfig = ma_engine_config_init();
         engineConfig.pContext = m_maContext.get();
         engineConfig.pDevice = &outDevice->device;
         engineConfig.noAutoStart = MA_TRUE;
         outEngine = std::make_unique<ma_engine>();
         ma_engine_init(&engineConfig, outEngine.get());
#ifdef __ANDROID__
         // Use the diagnostic wrapper so we can verify SCHED_FIFO inheritance
         // and measure mixing time on AAudio's RT callback thread.
         outDevice->device.onData = ma_data_callback_aaudio_diag;
#else
         outDevice->device.onData = ma_engine_data_callback_internal;
#endif
         outDevice->device.pUserData = outEngine.get();
         ma_engine_start(outEngine.get());
      }
      else
      {
         PLOGE << "Failed to initialize miniaudio for " << label << " sounds";
         outDevice = nullptr;
      }
   };

   initDevice(m_backglassDevice, m_backglassEngine, "backglass",
#ifdef __ANDROID__
              0);
#else
              m_backglassAudioDevice);
#endif
   initDevice(m_playfieldDevice, m_playfieldEngine, "playfield",
#ifdef __ANDROID__
              0);
#else
              m_playfieldAudioDevice);
#endif
}

AudioPlayer::~AudioPlayer()
{
   m_soundPlayers.clear();
   m_audioStreams.clear();
   m_pendingDeleteAudioStreams.clear();
   m_music = nullptr;
   if (m_backglassEngine)
      ma_engine_uninit(m_backglassEngine.get());
   if (m_playfieldEngine)
      ma_engine_uninit(m_playfieldEngine.get());
   if (m_playfieldDevice)
      ma_device_uninit(&m_playfieldDevice->device);
   if (m_backglassDevice)
      ma_device_uninit(&m_backglassDevice->device);
   if (m_maContext)
      ma_context_uninit(m_maContext.get());
   if (m_backglassSDLDevice != 0)
      SDL_CloseAudioDevice(m_backglassSDLDevice);
   SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioPlayer::SetPaused(bool paused)
{
   // Map to miniaudio start/stop which hits the SDL custom backend at
   // AudioPlayer.cpp:165-177 (SDL_PauseAudioStreamDevice / SDL_ResumeAudioStreamDevice).
   // On Android this parks the audio callback thread so the OS can freeze the process.
   if (m_playfieldDevice)
   {
      if (paused)
         ma_device_stop(&m_playfieldDevice->device);
      else
         ma_device_start(&m_playfieldDevice->device);
   }
   if (m_backglassDevice)
   {
      if (paused)
         ma_device_stop(&m_backglassDevice->device);
      else
         ma_device_start(&m_backglassDevice->device);
   }
}

void AudioPlayer::SetMainVolume(float backglassVolume, float playfieldVolume)
{
   m_backglassVolume = backglassVolume;
   m_playfieldVolume = playfieldVolume;
   if (m_music)
      m_music->SetMainVolume(backglassVolume, playfieldVolume);
   for (const auto& [sound, players] : m_soundPlayers)
      for (auto& player : players)
         player->SetMainVolume(backglassVolume, playfieldVolume);
   for (const auto& player : m_audioStreams)
      player->SetMainVolume(backglassVolume);
}

AudioPlayer::AudioStreamID AudioPlayer::OpenAudioStream(const string& name, int frequency, int channels, bool isFloat)
{
#ifdef __ANDROID__
   if (!m_backglassEngine)
   {
      PLOGE << "OpenAudioStream: backglass engine not initialized";
      return nullptr;
   }
   std::unique_ptr<AudioStreamPlayer> audioStream = AudioStreamPlayer::Create(m_backglassEngine.get(), frequency, channels, isFloat);
#else
   if (m_backglassSDLDevice == 0)
   {
      SDL_AudioSpec deviceSpec;
      const bool hasDeviceSpec = SDL_GetAudioDeviceFormat(m_backglassAudioDevice, &deviceSpec, nullptr);
      m_backglassSDLDevice = SDL_OpenAudioDevice(m_backglassAudioDevice, hasDeviceSpec ? & deviceSpec : nullptr);
   }
   std::unique_ptr<AudioStreamPlayer> audioStream = AudioStreamPlayer::Create(m_backglassSDLDevice, frequency, channels, isFloat);
#endif
   if (audioStream == nullptr)
      return nullptr;
   AudioStreamID stream = std::move(audioStream);
   stream->SetMainVolume(m_backglassVolume);
   stream->SetName(name);
   m_audioStreams.push_back(stream);
   return stream;
}

bool AudioPlayer::IsOpened(const AudioStreamID& stream) const
{
   auto item = std::ranges::find_if(m_audioStreams, [stream](const std::shared_ptr<AudioStreamPlayer>& player) { return player == stream; });
   return item != m_audioStreams.end();
}

void AudioPlayer::EnqueueStream(const AudioStreamID& stream, uint8_t* buffer, int length) const {
   stream->Enqueue(buffer, length);
}

void AudioPlayer::SetStreamVolume(const AudioStreamID& stream, const float volume) const {
   stream->SetStreamVolume(volume);
}

void AudioPlayer::CloseAudioStream(const AudioStreamID& stream, bool afterEndOfStream)
{
   auto item = std::ranges::find_if(m_audioStreams, [stream](const std::shared_ptr<AudioStreamPlayer>& player) { return player == stream; });
   if (item != m_audioStreams.end())
   {
      // Keep a reference until enqueued data has been played
      if (afterEndOfStream && (*item)->GetQueuedSize() != 0)
      {
         m_pendingDeleteAudioStreams.push_back(*item);
         (*item)->FlushStream();
      }
      m_audioStreams.erase(item);
   }
   else
   {
      PLOGE << "AudioStream not found in AudioPlayer::CloseAudioStream()";
   }
}

bool AudioPlayer::PlayMusic(const string& filename)
{
   m_music = std::unique_ptr<SoundPlayer>(SoundPlayer::Create(this, filename));
   if (m_music)
   {
      m_music->SetVolume(m_musicVolume);
      m_music->SetMainVolume(m_backglassVolume, m_playfieldVolume);
   }
   return m_music != nullptr;
}

void AudioPlayer::PauseMusic()
{
   if (m_music) m_music->Pause();
}

void AudioPlayer::UnpauseMusic()
{
   if (m_music) m_music->Unpause();
}

float AudioPlayer::GetMusicPosition() const
{
   return m_music ? m_music->GetPosition() : 0.f;
}
   
void AudioPlayer::SetMusicPosition(float seconds)
{
   if (m_music) m_music->SetPosition(seconds);
}

void AudioPlayer::SetMusicVolume(const float volume)
{
   m_musicVolume = volume;
   if (m_music) m_music->SetVolume(volume);
}

bool AudioPlayer::IsMusicPlaying() const
{
   return m_music && m_music->IsPlaying();
}

void AudioPlayer::PlaySound(Sound* sound, float volumeOffset, const float randomPitch, const int pitch, float panOffset, float frontRearFadeOffset, const int loopcount, const bool useSame, const bool restart)
{
   SoundPlayer* player = nullptr;
   vector<std::unique_ptr<SoundPlayer>>& players = m_soundPlayers[sound];

   // Until 10.8, implementation would:
   // - for some reason, 'usesame' would only be processed for wav file:
   //   - if 'usesame' is true, search for the first player for the given sound and reuse it if any (even is it is playing), create a new one otherwise
   //   - if 'usesame' is false, always create a new player for the given sound
   // - if restart is false and selected sound player was already playing, settings would be applied without restarting the sound
   for (const auto& soundPlayer : players)
   {
      if (useSame || !soundPlayer->IsPlaying())
      {
         player = soundPlayer.get();
         break;
      }
   }

   if (player == nullptr)
   {
      player = SoundPlayer::Create(this, sound);
      if (player == nullptr)
         return;
      player->SetMainVolume(m_backglassVolume, m_playfieldVolume);
      players.push_back(std::unique_ptr<SoundPlayer>(player));
   }

   float pan = dequantizeSignedPercent(sound->GetPan()) + panOffset;

   if (restart)
      player->Stop();
   player->Play(
      dequantizeSignedPercent(sound->GetVolume()) + volumeOffset,
      randomPitch,
      pitch,
      m_mirrored ? -pan : pan,
      dequantizeSignedPercent(sound->GetFrontRearFade()) + frontRearFadeOffset,
      loopcount);
}

void AudioPlayer::StopSound(Sound* sound)
{
   const vector<std::unique_ptr<SoundPlayer>>& players = m_soundPlayers[sound];
   for (const auto& player : players)
      player->Stop();
}

SoundSpec AudioPlayer::GetSoundInformations(const Sound* const sound) const
{
   SoundSpec specs {};
   ma_decoder decoder;
   if (ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_unknown, 0, 0);
      ma_decoder_init_memory(sound->GetFileRaw(), sound->GetFileSize(), &decoderConfig, &decoder) != MA_SUCCESS)
      return specs;
   specs.nChannels = decoder.outputChannels;
   specs.sampleFrequency = decoder.outputSampleRate;

   ma_sound maSound;
   ma_sound_config config = ma_sound_config_init_2(m_backglassEngine.get());
   config.pDataSource = &decoder;
   if (ma_sound_init_ex(m_backglassEngine.get(), &config, &maSound))
      return specs;
   float length;
   ma_sound_get_length_in_seconds(&maSound, &length);
   specs.lengthInSeconds = length;
   ma_sound_uninit(&maSound);

   ma_decoder_uninit(&decoder);
   return specs;
}

vector<AudioPlayer::AudioDevice> AudioPlayer::EnumerateAudioDevices()
{
   if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      PLOGE << "SDL Init Audio failed: " << SDL_GetError();
      return vector<AudioDevice>();
   }
   int count;
   auto pAudioList = SDL_GetAudioPlaybackDevices(&count);
   vector<AudioDevice> audioDevices;
   for (int i = 0; i < count; ++i)
   {
      SDL_AudioSpec spec;
      SDL_GetAudioDeviceFormat(pAudioList[i], &spec, nullptr);
      const AudioDevice audioDevice = { SDL_GetAudioDeviceName(pAudioList[i]), static_cast<unsigned int>(spec.channels) };
      audioDevices.push_back(audioDevice);
   }
   SDL_QuitSubSystem(SDL_INIT_AUDIO);
   return audioDevices;
}

}

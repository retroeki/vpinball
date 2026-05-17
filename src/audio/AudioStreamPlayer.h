// license:GPLv3+

#pragma once

#include <SDL3/SDL_audio.h>
#include "plugins/MsgPluginManager.h"

// Forward-declare miniaudio types for the Android streaming path. They're
// opaque pointers in the header; the full headers are included in the .cpp.
#ifdef __ANDROID__
struct ma_engine;
#endif

namespace VPX
{

// Stream provided raw buffer audio to either:
//   * an SDL audio device (default path on every desktop / mobile platform), or
//   * a miniaudio engine on Android (so the data is mixed inside AAudio's
//     SCHED_FIFO data callback thread, same as our main sound effects, to
//     avoid the preemption-induced pops we used to get on SDL's audio worker).
class AudioStreamPlayer
{
public:
#ifdef __ANDROID__
   // AndroidStream is the miniaudio-backed implementation detail. Forward-
   // declared as public because the free C-style ma_data_source vtable
   // callbacks in the .cpp need to reinterpret_cast their pDataSource arg to
   // this type. Its full definition lives in AudioStreamPlayer.cpp.
   struct AndroidStream;

   static std::unique_ptr<AudioStreamPlayer> Create(ma_engine* engine, int frequency, int channels, bool isFloat);
   AudioStreamPlayer(ma_engine* engine, int frequency, int channels, bool isFloat);
#else
   static std::unique_ptr<AudioStreamPlayer> Create(SDL_AudioDeviceID sdlDevice, int frequency, int channels, bool isFloat);
   explicit AudioStreamPlayer(SDL_AudioStream* stream);
#endif
   ~AudioStreamPlayer();

   void Enqueue(const uint8_t* buffer, int length);
   void FlushStream();
   int GetQueuedSize() const;
   void SetName(string name) { m_name = std::move(name); }
   void SetStreamVolume(const float volume);
   void SetMainVolume(const float volume);

private:
#ifdef __ANDROID__
   std::unique_ptr<AndroidStream> m_android;
#else
   static void AudioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);
   SDL_AudioStream* const m_stream = nullptr;
   SDL_AudioSpec m_audioSpec;
#endif

   float m_mainVolume = 1.f;
   float m_streamVolume = 1.f;
   float m_throttling = 1.f;
   uint64_t m_streamedTotal = 0;
   uint64_t m_startTimestamp = 0;
   string m_name;
   bool m_resync = false;
};

}

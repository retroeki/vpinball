// license:GPLv3+

#include "core/stdafx.h"
#include "AudioStreamPlayer.h"

#ifdef __ANDROID__
// We only need the headers / type declarations here; miniaudio.c is compiled
// in AudioPlayer.cpp so the symbols are linked from this same translation
// unit set. Match AudioPlayer.cpp's backend defines exactly so struct
// layouts (ma_device's backend-specific union, in particular) agree across
// TUs and we don't trip ODR.
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_CUSTOM
#define MA_ENABLE_AAUDIO
#include "miniaudio/miniaudio.h"
#include <cstring>
#endif

namespace VPX
{

#ifdef __ANDROID__

// ─────────────────────────────────────────────────────────────────────────
//  Android streamed-audio path
//
//  Each AudioStreamPlayer becomes a custom ma_data_source backed by a
//  lock-free ring buffer. ma_sound wraps it and is attached to the backglass
//  ma_engine, so the data ends up mixed inside AAudio's privileged data-
//  callback thread (SCHED_FIFO) alongside the rest of the effects. No more
//  SDL audio worker thread on the streamed path, no more SCHED_OTHER
//  preemption pops on music / PinMAME / AltSound / PuP audio.
// ─────────────────────────────────────────────────────────────────────────

struct AudioStreamPlayer::AndroidStream
{
   ma_data_source_base ds;   // must be the first member; ma_data_source* casts to this struct
   ma_pcm_rb rb;
   ma_sound sound;
   ma_engine* engine = nullptr;
   ma_format format = ma_format_unknown;
   ma_uint32 channels = 0;
   ma_uint32 sampleRate = 0;
   ma_uint32 bytesPerFrame = 0;
   bool dsInit = false;
   bool rbInit = false;
   bool soundInit = false;

   // True iff the previous onRead call could not satisfy its full request and
   // had to fill silence — i.e. we're sitting on a paused source. Read once
   // at the top of each onRead and used to decide whether the next chunk of
   // real data needs a fade-in (see click-suppression block below).
   bool wasUnderrun = false;
};

// ─────────────────────────────────────────────────────────────────────────
// Click suppression at stream-on / stream-off boundaries.
//
// Background: when the user opens the in-game menu or enters Service Mode,
// SoftPause fires the script's `Paused` event. core.vbs:2277 automatically
// translates that into `Controller.Pause = True`, which halts PinMAME
// emulation. PinMAME stops producing audio; our ring buffer drains; our
// onRead callback starts filling silence. The instantaneous transition from
// "last real sample at mid-waveform amplitude" to "zero" is an audible
// click. The reverse happens on resume: zero -> fresh non-zero sample,
// another click. Without smoothing, every pause and every resume produced
// audible clicks (and bursts of them when the menu open / close animation
// triggered multiple state transitions in quick succession).
//
// Fix: cross-fade across the boundary. Just before silence fill begins, we
// ramp the last kFadeFrames frames of real data linearly from 1.0 -> 0.0.
// Just after silence ends, we ramp the first kFadeFrames real frames from
// 0.0 -> 1.0. The ramp is short enough to be inaudible as a fade but long
// enough to fully suppress the discontinuity (Gibbs / step response of the
// reconstruction filter).
//
// 256 frames was picked empirically: it's ~5.8 ms at 44.1 kHz, ~11.6 ms at
// 22.05 kHz (typical PinMAME WPC pre-DCS source rate, e.g. CFTBL). Both
// well below human perception of fade duration, well above the click
// threshold. Fully fixed the pause/resume click symptom on CFTBL — confirmed
// 2026-05-15 against in-game menu open/close and Service Mode transitions.
// ─────────────────────────────────────────────────────────────────────────
static constexpr ma_uint32 kFadeFrames = 256;

// Apply a linear amplitude ramp to nFrames of PCM, in-place. gFrom is the
// gain applied to the first frame, gTo to the last; intermediate frames
// interpolate linearly. Supports s16 and f32 (the only two formats the
// PinMAME / AltSound / PuP plugins ever produce). Other formats are a
// no-op rather than corrupting unknown sample widths.
static void ApplyLinearGainRamp(void* pData, ma_uint32 nFrames, ma_uint32 channels,
                                ma_format fmt, float gFrom, float gTo)
{
   if (nFrames == 0 || channels == 0) return;
   const float denom = (nFrames > 1) ? static_cast<float>(nFrames - 1) : 1.0f;
   if (fmt == ma_format_s16)
   {
      auto* p = static_cast<int16_t*>(pData);
      for (ma_uint32 i = 0; i < nFrames; ++i)
      {
         const float t = static_cast<float>(i) / denom;
         const float gain = gFrom + (gTo - gFrom) * t;
         for (ma_uint32 c = 0; c < channels; ++c)
         {
            int32_t v = static_cast<int32_t>(p[i * channels + c] * gain);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            p[i * channels + c] = static_cast<int16_t>(v);
         }
      }
   }
   else if (fmt == ma_format_f32)
   {
      auto* p = static_cast<float*>(pData);
      for (ma_uint32 i = 0; i < nFrames; ++i)
      {
         const float t = static_cast<float>(i) / denom;
         const float gain = gFrom + (gTo - gFrom) * t;
         for (ma_uint32 c = 0; c < channels; ++c)
            p[i * channels + c] *= gain;
      }
   }
}

static ma_result AndroidStream_onRead(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
{
   auto* s = reinterpret_cast<AudioStreamPlayer::AndroidStream*>(pDataSource);

   ma_uint64 totalRead = 0;
   auto* out = static_cast<uint8_t*>(pFramesOut);
   ma_uint64 remaining = frameCount;
   const bool wasUnderrun = s->wasUnderrun;

   while (remaining > 0)
   {
      ma_uint32 wanted = static_cast<ma_uint32>(
         remaining > 0xFFFFFFFFu ? 0xFFFFFFFFu : remaining);
      void* pReadBuffer = nullptr;
      const ma_result rc = ma_pcm_rb_acquire_read(&s->rb, &wanted, &pReadBuffer);
      if (rc != MA_SUCCESS || wanted == 0)
         break; // underrun — break out, we fill silence below
      std::memcpy(out + totalRead * s->bytesPerFrame, pReadBuffer, static_cast<size_t>(wanted) * s->bytesPerFrame);
      ma_pcm_rb_commit_read(&s->rb, wanted);
      totalRead += wanted;
      remaining -= wanted;
   }

   const bool nowUnderrunning = (remaining > 0);

   // Boundary fades — see the click-suppression comment block at kFadeFrames.

   // silence -> data (resume): ramp the first read frames up from 0.
   if (wasUnderrun && totalRead > 0)
   {
      const ma_uint32 n = static_cast<ma_uint32>(std::min<ma_uint64>(totalRead, kFadeFrames));
      ApplyLinearGainRamp(out, n, s->channels, s->format, 0.0f, 1.0f);
   }

   // data -> silence (pause): ramp the last read frames down to 0 before
   // the silence fill below takes over.
   if (nowUnderrunning && totalRead > 0)
   {
      const ma_uint32 n = static_cast<ma_uint32>(std::min<ma_uint64>(totalRead, kFadeFrames));
      const ma_uint64 fadeStart = totalRead - n;
      ApplyLinearGainRamp(out + fadeStart * s->bytesPerFrame, n, s->channels, s->format, 1.0f, 0.0f);
   }

   if (remaining > 0)
   {
      // Returning MA_AT_END would stop the sound, so we fill silence
      // instead — ma_engine keeps the node alive and we just resume
      // pushing real samples (with a fade-in) when data returns.
      ma_silence_pcm_frames(out + totalRead * s->bytesPerFrame, remaining, s->format, s->channels);
      totalRead += remaining;
   }

   s->wasUnderrun = nowUnderrunning;
   *pFramesRead = totalRead;
   return MA_SUCCESS;
}

static ma_result AndroidStream_onSeek(ma_data_source* /*pDataSource*/, ma_uint64 /*frameIndex*/)
{
   return MA_NOT_IMPLEMENTED;
}

static ma_result AndroidStream_onGetDataFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* /*pChannelMap*/, size_t /*channelMapCap*/)
{
   auto* s = reinterpret_cast<AudioStreamPlayer::AndroidStream*>(pDataSource);
   if (pFormat) *pFormat = s->format;
   if (pChannels) *pChannels = s->channels;
   if (pSampleRate) *pSampleRate = s->sampleRate;
   return MA_SUCCESS;
}

static ma_result AndroidStream_onGetCursor(ma_data_source* /*pDataSource*/, ma_uint64* pCursor)
{
   if (pCursor) *pCursor = 0;
   return MA_SUCCESS;
}

static ma_result AndroidStream_onGetLength(ma_data_source* /*pDataSource*/, ma_uint64* pLength)
{
   if (pLength) *pLength = 0; // 0 = infinite / unknown for streaming sources
   return MA_SUCCESS;
}

static ma_data_source_vtable g_AndroidStreamVtable = {
   AndroidStream_onRead,
   AndroidStream_onSeek,
   AndroidStream_onGetDataFormat,
   AndroidStream_onGetCursor,
   AndroidStream_onGetLength,
   nullptr, // onSetLooping
   0,       // flags
};

std::unique_ptr<AudioStreamPlayer> AudioStreamPlayer::Create(ma_engine* engine, int frequency, int channels, bool isFloat)
{
   if (!engine || frequency <= 0 || channels <= 0)
   {
      PLOGE << "AudioStreamPlayer::Create: invalid args (engine=" << engine << ", freq=" << frequency << ", channels=" << channels << ')';
      return nullptr;
   }
   auto player = std::make_unique<AudioStreamPlayer>(engine, frequency, channels, isFloat);
   if (!player->m_android || !player->m_android->soundInit)
   {
      PLOGE << "AudioStreamPlayer::Create: failed to construct miniaudio backing";
      return nullptr;
   }
   return player;
}

AudioStreamPlayer::AudioStreamPlayer(ma_engine* engine, int frequency, int channels, bool isFloat)
   : m_startTimestamp(SDL_GetTicks())
   , m_android(std::make_unique<AndroidStream>())
{
   auto& a = *m_android;
   a.engine = engine;
   a.format = isFloat ? ma_format_f32 : ma_format_s16;
   a.channels = static_cast<ma_uint32>(channels);
   a.sampleRate = static_cast<ma_uint32>(frequency);
   a.bytesPerFrame = ma_get_bytes_per_frame(a.format, a.channels);

   // Initialise the data source itself.
   ma_data_source_config dsCfg = ma_data_source_config_init();
   dsCfg.vtable = &g_AndroidStreamVtable;
   if (ma_data_source_init(&dsCfg, &a.ds) != MA_SUCCESS)
   {
      PLOGE << "AudioStreamPlayer: ma_data_source_init failed";
      return;
   }
   a.dsInit = true;

   // Ring buffer sized at 1 second of audio. Streamed sources (PinMAME ROM,
   // music, AltSound, PuP) push at 60Hz; 1s is well above the worst
   // preemption gaps we measured and never causes audible latency because
   // ma_engine drains as soon as data arrives.
   const ma_uint32 rbFrames = a.sampleRate;
   if (ma_pcm_rb_init(a.format, a.channels, rbFrames, nullptr, nullptr, &a.rb) != MA_SUCCESS)
   {
      PLOGE << "AudioStreamPlayer: ma_pcm_rb_init failed";
      ma_data_source_uninit(&a.ds);
      a.dsInit = false;
      return;
   }
   a.rbInit = true;

   // Tell miniaudio the source's native sample rate so it can resample to
   // the engine's rate without us doing it manually.
   ma_pcm_rb_set_sample_rate(&a.rb, a.sampleRate);

   // Skip 3D positioning — these are music / ROM streams that should play
   // straight to the device. Do NOT set MA_SOUND_FLAG_NO_PITCH: in miniaudio
   // the pitch node IS the sample-rate converter, so disabling it would skip
   // resampling when the source rate (e.g. PinMAME 22050) differs from the
   // engine rate (typically 48000), producing garbled audio.
   const ma_uint32 soundFlags = MA_SOUND_FLAG_NO_SPATIALIZATION;
   if (ma_sound_init_from_data_source(engine, &a.ds, soundFlags, nullptr, &a.sound) != MA_SUCCESS)
   {
      PLOGE << "AudioStreamPlayer: ma_sound_init_from_data_source failed";
      ma_pcm_rb_uninit(&a.rb);
      ma_data_source_uninit(&a.ds);
      a.rbInit = false;
      a.dsInit = false;
      return;
   }
   a.soundInit = true;

   ma_sound_set_volume(&a.sound, m_streamVolume * m_mainVolume);
   ma_sound_start(&a.sound);
}

AudioStreamPlayer::~AudioStreamPlayer()
{
   if (m_android)
   {
      auto& a = *m_android;
      if (a.soundInit) ma_sound_uninit(&a.sound);
      if (a.rbInit) ma_pcm_rb_uninit(&a.rb);
      if (a.dsInit) ma_data_source_uninit(&a.ds);
   }
}

void AudioStreamPlayer::Enqueue(const uint8_t* buffer, int length)
{
   if (!m_android || !m_android->rbInit || length <= 0) return;
   auto& a = *m_android;

   // Resync request from the timing logic: dump the queue and reset the
   // playback timestamp so we start fresh at "now".
   if (m_resync)
   {
      ma_pcm_rb_reset(&a.rb);
      m_startTimestamp = SDL_GetTicks();
      m_streamedTotal = 0;
      m_resync = false;
      PLOGI << "Audio stream sync was lost and reseted";
   }

   const ma_uint32 totalFrames = static_cast<ma_uint32>(length) / a.bytesPerFrame;
   ma_uint32 framesRemaining = totalFrames;
   const uint8_t* src = buffer;
   while (framesRemaining > 0)
   {
      ma_uint32 chunk = framesRemaining;
      void* dst = nullptr;
      const ma_result rc = ma_pcm_rb_acquire_write(&a.rb, &chunk, &dst);
      if (rc != MA_SUCCESS || chunk == 0)
      {
         // Buffer full: drop the rest rather than block the producer thread.
         break;
      }
      std::memcpy(dst, src, static_cast<size_t>(chunk) * a.bytesPerFrame);
      ma_pcm_rb_commit_write(&a.rb, chunk);
      src += chunk * a.bytesPerFrame;
      framesRemaining -= chunk;
   }
   m_streamedTotal += length;

   // Throttling / resync timing logic — used to live in the SDL get callback
   // on the audio worker thread. On the miniaudio path we have no equivalent
   // hook, so run it on the producer (PinMAME / music / etc) thread instead.
   const uint64_t nBytePerSec = static_cast<uint64_t>(a.sampleRate) * a.bytesPerFrame;
   if (nBytePerSec > 0)
   {
      const ma_uint32 framesQueued = ma_pcm_rb_available_read(&a.rb);
      const uint64_t queuedBytes = static_cast<uint64_t>(framesQueued) * a.bytesPerFrame;
      const uint64_t playedTS = (1000 * (m_streamedTotal - queuedBytes)) / nBytePerSec;
      const uint64_t nowTS = SDL_GetTicks() - m_startTimestamp;
      if (playedTS > nowTS)
      {
         m_startTimestamp += playedTS - nowTS;
      }
      else if (nowTS > playedTS)
      {
         const uint64_t deltaTS = nowTS - playedTS;
         if (queuedBytes > 1000 * nBytePerSec && deltaTS > 1000)
            m_resync = true;
      }
   }
}

void AudioStreamPlayer::FlushStream()
{
   // No-op on the miniaudio path: the ring buffer drains naturally as
   // ma_sound reads it. There's no equivalent of SDL_FlushAudioStream that
   // would force a one-shot emission.
}

int AudioStreamPlayer::GetQueuedSize() const
{
   if (!m_android || !m_android->rbInit) return 0;
   return static_cast<int>(ma_pcm_rb_available_read(const_cast<ma_pcm_rb*>(&m_android->rb))) * static_cast<int>(m_android->bytesPerFrame);
}

void AudioStreamPlayer::SetStreamVolume(const float volume)
{
   if (m_streamVolume == volume) return;
   m_streamVolume = volume;
   if (m_android && m_android->soundInit)
      ma_sound_set_volume(&m_android->sound, m_streamVolume * m_mainVolume);
}

void AudioStreamPlayer::SetMainVolume(const float volume)
{
   if (m_mainVolume == volume) return;
   m_mainVolume = volume;
   if (m_android && m_android->soundInit)
      ma_sound_set_volume(&m_android->sound, m_streamVolume * m_mainVolume);
}

#else // !__ANDROID__

// ─────────────────────────────────────────────────────────────────────────
//  Default (non-Android) path: stream to an SDL audio device. Unchanged.
// ─────────────────────────────────────────────────────────────────────────

std::unique_ptr<AudioStreamPlayer> AudioStreamPlayer::Create(SDL_AudioDeviceID sdlDevice, int frequency, int channels, bool isFloat)
{
   SDL_AudioSpec streamSpec;
   streamSpec.freq = frequency;
   streamSpec.format = isFloat ? SDL_AUDIO_F32 : SDL_AUDIO_S16;
   streamSpec.channels = channels;
   SDL_AudioSpec deviceSpec;
   SDL_GetAudioDeviceFormat(sdlDevice, &deviceSpec, nullptr);
   SDL_AudioStream* stream = SDL_CreateAudioStream(&streamSpec, &deviceSpec);
   if (stream)
   {
      SDL_BindAudioStream(sdlDevice, stream);
      SDL_ResumeAudioStreamDevice(stream);
      return std::make_unique<AudioStreamPlayer>(stream);
   }
   else
   {
      PLOGE << "Failed to create stream: " << SDL_GetError();
      return nullptr;
   }
}

AudioStreamPlayer::AudioStreamPlayer(SDL_AudioStream* stream)
   : m_stream(stream)
   #ifdef ENABLE_DX9
   , m_startTimestamp(msec())
   #else
   , m_startTimestamp(SDL_GetTicks())
   #endif
{
   assert(stream != nullptr);
   SDL_GetAudioStreamFormat(m_stream, &m_audioSpec, nullptr);
   SDL_SetAudioStreamGetCallback(m_stream, &AudioStreamCallback, this);
}

AudioStreamPlayer::~AudioStreamPlayer()
{
   SDL_DestroyAudioStream(m_stream);
}

void AudioStreamPlayer::Enqueue(const uint8_t* buffer, int length)
{
   if (m_resync)
   {
      SDL_ClearAudioStream(m_stream);
      #ifdef ENABLE_DX9
      m_startTimestamp = msec();
      #else
      m_startTimestamp = SDL_GetTicks();
      #endif
      m_streamedTotal = 0;
      m_resync = false;
      PLOGI << "Audio stream sync was lost and reseted";
   }
   SDL_PutAudioStreamData(m_stream, buffer, length);
   m_streamedTotal += length;
}

void AudioStreamPlayer::FlushStream()
{
   SDL_FlushAudioStream(m_stream);
}

int AudioStreamPlayer::GetQueuedSize() const
{
   return SDL_GetAudioStreamQueued(m_stream);
}

void AudioStreamPlayer::SetStreamVolume(const float volume)
{
   if (m_streamVolume != volume)
   {
      m_streamVolume = volume;
      SDL_SetAudioStreamGain(m_stream, m_streamVolume * m_mainVolume);
   }
}

void AudioStreamPlayer::SetMainVolume(const float volume)
{
   if (m_mainVolume != volume)
   {
      m_mainVolume = volume;
      SDL_SetAudioStreamGain(m_stream, m_streamVolume * m_mainVolume);
   }
}

void AudioStreamPlayer::AudioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
   auto const me = static_cast<AudioStreamPlayer*>(userdata);
   const unsigned int nQueueSize = max(0, SDL_GetAudioStreamQueued(stream) - total_amount);
   const uint64_t nBytePerSec = me->m_audioSpec.freq * (uint64_t)SDL_AUDIO_FRAMESIZE(me->m_audioSpec);
   const uint64_t sourceTS = (1000 * me->m_streamedTotal) / nBytePerSec;
   const uint64_t playedTS = (1000 * (me->m_streamedTotal - nQueueSize)) / nBytePerSec;
   #ifdef ENABLE_DX9
   const uint64_t nowTS = msec() - me->m_startTimestamp;
   #else
   const uint64_t nowTS = SDL_GetTicks() - me->m_startTimestamp;
   #endif
   float throttle = 1.f;
   if (playedTS > nowTS)
   {
      me->m_startTimestamp += playedTS - nowTS;
   }
   else if (nowTS > playedTS)
   {
      uint64_t deltaTS = nowTS - playedTS;
      if (nQueueSize > 1000 * nBytePerSec && deltaTS > 1000)
      {
         throttle = me->m_throttling;
         me->m_resync = true;
      }
   }
   if (me->m_throttling != throttle)
   {
      me->m_throttling = throttle;
      SDL_SetAudioStreamFrequencyRatio(me->m_stream, throttle);
      PLOGI << "PlayedTS: " << playedTS << "ms / NowTS: " << nowTS << "ms / Delta: " << (nowTS - playedTS) << "ms / Buffer: " << (sourceTS - playedTS) << "ms / Frequency ratio : " << throttle;
   }
}

#endif // !__ANDROID__

}

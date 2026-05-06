# Bluetooth Audio Lag — Investigation Notes

Read-only analysis of `C:\vpinball-master` audio stack. Parked for later. No fixes applied.

## Stack overview

```
Game code
  └─ VPX::AudioPlayer         src/audio/AudioPlayer.cpp
       ├─ miniaudio engine    (vendored, custom SDL3 backend defined in same file)
       │    └─ ma_device_ex   wraps an SDL_AudioStream
       └─ VPX::AudioStreamPlayer  src/audio/AudioStreamPlayer.cpp  (VPM / AltSound / PuP)
            └─ SDL_AudioStream
                 └─ SDL3 AAudio backend  external/.../SDL3/SDL/src/audio/aaudio/SDL_aaudio.c
                      └─ AAudio          ─→ Android audio HAL ─→ A2DP encoder ─→ BT pipe
```

Two miniaudio devices are created (backglass + playfield). VPM/AltSound also opens a third SDL device for streamed audio.

---

## Suspected causes, ranked

### 1. AAudio `LOW_LATENCY` mode forced on, BT can't honor it
- `external/android-arm64-v8a/Release/SDL3/SDL/src/audio/aaudio/SDL_aaudio.c:323-325` unconditionally calls `AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY)` (default `true` for `SDL_HINT_ANDROID_LOW_LATENCY_AUDIO`).
- BT output routes do not support AAudio's fast path. Android silently falls back to a normal/legacy path, typically with a far larger buffer than the app expects.
- Nothing under `src/` or `standalone/` overrides `SDL_HINT_ANDROID_LOW_LATENCY_AUDIO`. Verified via grep.
- **Fix idea:** at app start, check the active output device type (`AudioDeviceInfo.TYPE_BLUETOOTH_A2DP` / `TYPE_BLE_HEADSET` / `TYPE_BLE_SPEAKER`) from `SDLAudioManager` and call `SDL_SetHint(SDL_HINT_ANDROID_LOW_LATENCY_AUDIO, "0")` before SDL audio init if BT.

### 2. `AudioStreamPlayer` clock model ignores device-side latency
- `src/audio/AudioStreamPlayer.cpp:97-137` computes `playedTS = (m_streamedTotal − SDL_GetAudioStreamQueued()) / nBytePerSec`.
- `SDL_GetAudioStreamQueued` only reports bytes still in the SDL stream queue. It knows nothing about:
  - SDL3 AAudio mixbuf (3 buffers — `SDL_aaudio.c:366-367`)
  - AAudio device buffer
  - BT A2DP encoder buffer (≈SBC 150–250 ms, AAC 100–200, aptX 40, aptX-LL 30, LC3 30)
- Result: the VPM/AltSound clock thinks audio is playing several hundred ms ahead of where ears actually hear it. The catch-up branch at line 114 silently drags `m_startTimestamp` forward, masking the lag rather than reporting it. `AAudioStream_getTimestamp` is never called anywhere.
- **Fix idea:** plumb `AAudioStream_getTimestamp(...)` (or SDL's equivalent presentation-time API if available) into `playedTS` so drift math accounts for device latency.

### 3. Likely bug in resync threshold
- `src/audio/AudioStreamPlayer.cpp:119`:
  ```cpp
  if (nQueueSize > 1000 * nBytePerSec && deltaTS > 1000)
  ```
- `nBytePerSec = freq * frameSize`. `1000 * nBytePerSec` is **1000 seconds** of bytes — essentially unreachable.
- When BT really does fall behind, this branch never trips, so `m_resync` is never set, so `SDL_ClearAudioStream` is never called from `Enqueue`.
- **Fix idea:** likely intended `nQueueSize > nBytePerSec` (1 second of buffered audio).

### 4. Buffer stacking
End-to-end latency = sum of:
- BT codec buffer (codec-dependent, see §1)
- AAudio device buffer (large on non-fast-path)
- SDL3 triple-buffered mixbuf (`hidden->num_buffers = 3`, `SDL_aaudio.c:366-367`)
- miniaudio engine queue
- `SDL_AudioStream` queue
- VPX `AudioStreamPlayer` enqueue cadence

Each layer is designed assuming a low-latency direct path. Compounded over BT, it adds up.

### 5. Two/three independent audio streams on one BT pipe
- `src/audio/AudioPlayer.cpp:206-324` opens two miniaudio devices (backglass + playfield).
- `AudioPlayer::OpenAudioStream` (line 382-398) lazily opens a third SDL device (`m_backglassSDLDevice`) for streamed audio (VPM/PuP/AltSound).
- Each becomes its own AAudio stream. Android multiplexes them independently over the single BT route. They can drift relative to each other, which surfaces as music-vs-SFX desync.
- **Fix idea:** if backglass and playfield resolve to the same OS device, share a single ma_device.

### 6. `noPreSilencedOutputBuffer = MA_TRUE`
- `src/audio/AudioPlayer.cpp:268, 302`. On underrun the buffer is uninitialized rather than silent. BT error concealment may stretch frames to compensate, adding sporadic latency spikes / artifacts.
- Tradeoff vs CPU. Cheap to flip if it helps.

### 7. `periodSize` adopted blindly from device
- `src/audio/AudioPlayer.cpp:119-149` sets `pDescriptorPlayback->periodSizeInFrames = periodSizeInFrames` from `SDL_GetAudioDeviceFormat`, with `periodCount = 1`.
- On BT, AAudio still reports a small fast-path period size even though the real transport buffer is much larger. miniaudio's mixing graph runs at the reported period; no backpressure correction.

### 8. No graceful handling of BT hot-plug
- `standalone/android/app/src/main/java/org/libsdl/app/SDLAudioManager.java` forwards `onAudioDevicesAdded` / `onAudioDevicesRemoved` to native via `addAudioDevice` / `removeAudioDevice`.
- SDL responds with `RecoverAAudioDevice` (close + reopen the AAudio stream — `SDL_aaudio.c:178-213`).
- During recovery there's an audible gap. VPM keeps streaming bytes the whole time. The buggy resync threshold (§3) means we never reset the SDL queue afterward → permanent offset between source clock and output for the rest of the session.

### 9. Channel-collapse on BLE / LE-Audio (related symptom, not lag)
- `src/audio/SoundPlayer.cpp:90`: existing comment "Mono mode (surprisingly this happens, for example with bluetooth LE devices)" confirms BLE devices report `nOutChannels == 1`.
- SSF / 3D positional sound collapses to mono on BLE. Users on LE-Audio buds will report "sounds wrong" alongside lag.

---

## Quickest wins to try, in order

1. **Bypass `LOW_LATENCY` on BT routes.** Detect type at startup in `SDLAudioManager`, push the SDL hint before `SDL_InitSubSystem(SDL_INIT_AUDIO)`. Cheap, reversible.
2. **Fix the resync threshold** in `AudioStreamPlayer.cpp:119` (`1000 * nBytePerSec` → `nBytePerSec`). One-line change.
3. **Use AAudio presentation timestamp** for `playedTS` so drift math is honest about device latency.
4. **Coalesce same-device engines** so backglass + playfield share an `ma_device` when the OS device matches. Fixes music-vs-SFX drift on BT.
5. Re-evaluate `noPreSilencedOutputBuffer` and the lazy `m_backglassSDLDevice` open.

## Things to measure before/after each fix

- AAudio xrun count via `AAudioStream_getXRunCount` (currently unused — declared but not called in `SDL_aaudiofuncs.h:62`).
- `AAudioStream_getTimestamp` for ground-truth presentation latency.
- Logcat: SDL `aaudio:` / `SDLAudio` lines on BT connect/disconnect.
- A/B with `dumpsys media.audio_flinger` to see what HAL path the streams actually got.

## Out of scope

- Inherent BT codec latency. Cannot be fixed at app layer; only mitigated by recommending aptX-LL / LC3 / wired.
- Android-side audio policy. App can hint, OS decides.

## File reference index

| What | Where |
|---|---|
| AudioPlayer (engine setup, SDL backend) | `src/audio/AudioPlayer.cpp`, `src/audio/AudioPlayer.h` |
| AudioStreamPlayer (VPM/AltSound) | `src/audio/AudioStreamPlayer.cpp`, `src/audio/AudioStreamPlayer.h` |
| SoundPlayer (per-shot SFX, SSF mixer) | `src/audio/SoundPlayer.cpp`, `src/audio/SoundPlayer.h` |
| Java audio device hot-plug | `standalone/android/app/src/main/java/org/libsdl/app/SDLAudioManager.java` |
| SDL3 AAudio backend (vendored) | `external/android-arm64-v8a/Release/SDL3/SDL/src/audio/aaudio/SDL_aaudio.c` |
| AAudio function table | `external/android-arm64-v8a/Release/SDL3/SDL/src/audio/aaudio/SDL_aaudiofuncs.h` |

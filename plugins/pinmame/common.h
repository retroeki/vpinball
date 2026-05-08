// license:GPLv3+

#pragma once

#include <cassert>
#include <cstdarg>
#include <cstdio>

#include <string>
using std::string;

#include <vector>
using std::vector;

#include "libpinmame.h"

// Shared logging
#include "plugins/LoggingPlugin.h"

// Scriptable API
#include "plugins/ScriptablePlugin.h"

namespace PinMAME {

LPI_USE();
#define LOGD PinMAME::LPI_LOGD
#define LOGI PinMAME::LPI_LOGI
#define LOGW PinMAME::LPI_LOGW
#define LOGE PinMAME::LPI_LOGE

PSC_USE_ERROR();

#ifdef _MSC_VER
#define PATH_SEPARATOR_CHAR '\\'
#else
#define PATH_SEPARATOR_CHAR '/'
#endif

string find_case_insensitive_directory_path(const string& szPath);

// Runtime override for ROM audio output, defined in PinMAMEPlugin.cpp. Tables that overlay their
// own music via PlayMusic (e.g. JPSalas reskins like Aliens.vpx → sorcr_l2) call
// `Game.Settings.Value("sound") = 0` from VBScript; that ends up here so the audio callbacks
// can drop libpinmame buffers before they reach the SDL backglass stream. The global plugin
// "Sound.Enable Sound" setting still acts as a master toggle on top of this.
void SetRuntimeSoundEnabled(bool enabled);

// copies all characters of src incl. the null-terminator, BUT never more than dest_size-1, always null-terminates
inline void strncpy_s(char* const __restrict dest, const size_t dest_size, const char* const __restrict src)
{
   if (!dest || dest_size == 0)
      return;
   size_t i = 0;
   if (src)
      for (; i < dest_size-1 && src[i] != '\0'; ++i)
         dest[i] = src[i];
   dest[i] = '\0';
}

}

// license:GPLv3+

#include "GameSettings.h"

namespace PinMAME {

GameSettings::GameSettings() {

}

GameSettings::~GameSettings() {

}

int GameSettings::GetValue(const string& key) const
{
   return 0;
}

void GameSettings::SetValue(const string& key, int v)
{
   // Honor the "sound" key from VBScript: tables that overlay their own music
   // (e.g. Aliens.vpx → sorcr_l2) call `Game.Settings.Value("sound") = 0` to
   // silence the ROM emulation. Without this, libpinmame keeps emitting the
   // original ROM soundtrack on top of the table's PlayMusic layer.
   if (key == "sound")
      SetRuntimeSoundEnabled(v != 0);
}

}

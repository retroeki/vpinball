// license:GPLv3+

#pragma once

#include "common.h"
#include "plugins/ScriptablePlugin.h"

namespace PinMAME {

class GameSettings final
{
public:
   GameSettings();
   ~GameSettings();

   PSC_IMPLEMENT_REFCOUNT()

   void Clear() { /* Not yet implemented */ }
   // FIXME implement a dynamically typed variant for arbitrary per-game settings.
   // Today only `sound` is honored — see GameSettings.cpp.
   //ScriptVariant GetValue(const string& name) const { return {}; /* Not yet implemented */ }
   //void PutValue(const string& name, ScriptVariant v) { /* Not yet implemented */ }
   int GetValue(const string& key) const;
   void SetValue(const string& key, int v);
   void SetDisplayPosition(float newValX, float newValY, void* hWnd = nullptr) { /* Not yet implemented */ }
   void ShowSettingsDlg(void* hParentWnd = nullptr) { /* Not yet implemented */ }
};

}

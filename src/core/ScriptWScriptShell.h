// license:GPLv3+
//
// WScript.Shell shim for the standalone build.
//
// VPX community tables routinely use `CreateObject("WScript.Shell")` for:
//   - Registry settings probes (UltraDMD, B2S, plugin configs)
//   - `Sleep(ms)` waits
//   - `Run(cmd)` external program launches
//   - Environment-variable / SpecialFolders lookups
//
// On Android, Wine doesn't ship a WScript.Shell COM class — and even if it
// did, there's no Windows registry / process model to back it. The previous
// approach was script-text patching (PatchWScriptShell): match Subs that
// contain WshShell + RegWrite and stub them. Too aggressive — for tables
// like Sonic where the Sub also instantiates UltraDMD afterwards, the stub
// destroys the real init code and the table hangs.
//
// This shim provides a no-op IDispatch with the methods scripts actually
// touch:
//   .RegRead(name)                    -> Empty            (no key found)
//   .RegWrite(name, value, [type])    -> void             (silent no-op)
//   .RegDelete(name)                  -> void
//   .Run(cmd, [show], [wait])         -> 0                (success, no spawn)
//   .Exec(cmd)                        -> Empty            (no process object)
//   .Sleep(ms)                        -> real Sleep(ms)
//   .Wait(ms)                         -> real Sleep(ms)
//   .ExpandEnvironmentStrings(s)      -> s unchanged
//   .Environment(scope)               -> Empty
//   .SpecialFolders(name)             -> ""
//   .CurrentDirectory  (R/W)          -> ""               (silently swallowed)
//   .AppActivate(title)               -> False
//   .Popup(text, [...])               -> 0                (no UI)
//   .SendKeys(keys)                   -> void
//
// Wired in via def.cpp::external_create_object — same path as ScriptArrayList.

#pragma once

#include <oaidl.h>

// Returns a freshly-AddRef'd IDispatch implementing WScript.Shell.
// Caller owns the reference (typically held by the script's `Set` target).
// Returns nullptr on allocation failure.
IDispatch* CreateScriptWScriptShell();

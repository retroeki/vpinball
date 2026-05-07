// license:GPLv3+
//
// System.Collections.ArrayList shim for the standalone build.
//
// Many community VPX tables CreateObject("System.Collections.ArrayList") to
// hold per-game state — usually as a dynamic list of integer IDs (e.g. WoZ
// `modeHaunted.Add 49 / 50 / 51`). The .NET ArrayList COM class is not
// shipped with Wine on Android, so without this shim every such call returns
// CLASS_E_CLASSNOTAVAILABLE and the resulting `Set` leaves the variable as
// Nothing — which then crashes the table on the first method call.
//
// This implementation provides a minimal but spec-faithful IDispatch that
// covers the methods/properties community tables actually use:
//   .Add(item)              -> long  (returns the inserted index)
//   .Item(index)            -> variant  (default property, [propget/propput])
//   .Count                  -> long  ([propget])
//   .Clear()                -> void
//   .Contains(item)         -> bool
//   .IndexOf(item)          -> long  (-1 if not found)
//   .Remove(item)           -> void  (removes first match)
//   .RemoveAt(index)        -> void
//   .Insert(index, item)    -> void
//   .Sort()                 -> void
//   .Reverse()              -> void
//   .ToArray()              -> SAFEARRAY(VARIANT)
//   .Clone()                -> ArrayList copy
//   For Each item In list   -> via IEnumVARIANT (DISPID_NEWENUM)
//
// Wired in via def.cpp::external_create_object — the host fallback that
// catches CreateObject calls Wine's vbscript engine doesn't natively handle.

#pragma once

#include <oaidl.h>

// Returns a freshly-AddRef'd IDispatch implementing System.Collections.ArrayList.
// Caller owns the reference (typically held by the script's `Set` target).
// Returns nullptr on allocation failure.
IDispatch* CreateScriptArrayList();

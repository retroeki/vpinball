// license:GPLv3+
//
// Shell.Application shim for the standalone build.
//
// Some community tables (notably the Munsters Original 2020 series by
// jpsalas / friends) call `CreateObject("shell.application")` to access
// the Windows Explorer COM object, typically for:
//   - `objShell.NameSpace(path)` -> Folder object for enumerating files
//   - `Folder.ParseName(filename)` -> FolderItem for file metadata
//   - `Folder.GetDetailsOf(item, idx)` -> property strings (length,
//     codec, etc. — used to size PuP video assets)
//
// On Android there is no Windows Shell namespace. Without this shim the
// `CreateObject` call raises VBSE_CANT_CREATE_OBJECT at script load and
// the table dies before init. We've seen this on Munsters 1.05 and the
// VR ROOM cut v1.06b.
//
// This shim returns a no-op IDispatch where:
//   .NameSpace(path)               -> Empty (callers null-check)
//   .Open(path)                    -> Empty (no GUI)
//   .Explore(path)                 -> Empty
//   .ShellExecute(file, ...)       -> Empty
//   .Windows()                     -> Empty
//   .BrowseForFolder(...)          -> Empty
//   .MinimizeAll() / UndoMinimizeAll() -> no-op
//   .TileVertically / TileHorizontally / CascadeWindows -> no-op
//   .ControlPanelItem / TrayProperties / Help / FindFiles / Suspend / EjectPC / SetTime -> no-op
//
// Defensive callers (the Munsters pattern is `if (not objFolder is
// nothing) then ...`) degrade cleanly. Tables that hard-depend on
// shell.application results will fail downstream at the chained call —
// but that's no worse than the status quo (CreateObject failure today).
//
// Wired in via def.cpp::external_create_object alongside ScriptArrayList
// and ScriptWScriptShell.

#pragma once

#include <oaidl.h>

// Returns a freshly-AddRef'd IDispatch implementing Shell.Application.
// Caller owns the reference (typically held by the script's `Set` target).
// Returns nullptr on allocation failure.
IDispatch* CreateScriptShellApplication();

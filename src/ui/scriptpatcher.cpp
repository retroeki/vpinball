/**
 * @file scriptpatcher.cpp
 * @brief Main entry point for VBScript patching
 *
 * This is the main entry point for the ScriptPatcher. It coordinates
 * the various patching phases:
 * 1. Class emulation (scriptpatcher_classes.cpp)
 * 2. Wine compatibility patches (scriptpatcher_wine.cpp)
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <regex>
#include <sstream>

// Dictionary-based DropTarget with helper functions (Wine compatible)
// Wine VBScript can't handle chained indexing like arr(i)("key"), so we use helpers
const char* ScriptPatcher::DROP_TARGET_CLASS = R"(
Function DropTarget_Create(primary, secondary, prim, sw, animate, isDropped)
  Dim this_
  Set this_ = CreateObject("Scripting.Dictionary")
  Set this_("primary") = primary
  Set this_("secondary") = secondary
  Set this_("prim") = prim
  this_("sw") = sw
  this_("animate") = animate
  this_("isDropped") = isDropped
  Set DropTarget_Create = this_
End Function

' Get property from DTArray element - avoids Wine chaining issue
Function DTGet(arr, idx, propName)
  Dim obj
  Set obj = arr(idx)
  Select Case LCase(propName)
    Case "primary": Set DTGet = obj("primary")
    Case "secondary": Set DTGet = obj("secondary")
    Case "prim": Set DTGet = obj("prim")
    Case "sw": DTGet = obj("sw")
    Case "animate": DTGet = obj("animate")
    Case "isdropped": DTGet = obj("isDropped")
  End Select
End Function

' Get object property from DTArray element (returns object reference)
Function DTGetObj(arr, idx, propName)
  Dim obj
  Set obj = arr(idx)
  Select Case LCase(propName)
    Case "primary": Set DTGetObj = obj("primary")
    Case "secondary": Set DTGetObj = obj("secondary")
    Case "prim": Set DTGetObj = obj("prim")
  End Select
End Function

' Set property on DTArray element - avoids Wine chaining issue
Sub DTSet(arr, idx, propName, val)
  Dim obj
  Set obj = arr(idx)
  Select Case LCase(propName)
    Case "sw": obj("sw") = val
    Case "animate": obj("animate") = val
    Case "isdropped": obj("isDropped") = val
  End Select
End Sub
)";

// Dictionary-based StandupTarget with helper functions (Wine compatible)
const char* ScriptPatcher::STANDUP_TARGET_CLASS = R"(
Function StandupTarget_Create(primary, prim, sw, animate, target)
  Dim this_
  Set this_ = CreateObject("Scripting.Dictionary")
  Set this_("primary") = primary
  Set this_("prim") = prim
  this_("sw") = sw
  this_("animate") = animate
  this_("target") = target
  Set StandupTarget_Create = this_
End Function

' Get property from STArray element - avoids Wine chaining issue
Function STGet(arr, idx, propName)
  Dim obj
  Set obj = arr(idx)
  Select Case LCase(propName)
    Case "primary": Set STGet = obj("primary")
    Case "prim": Set STGet = obj("prim")
    Case "sw": STGet = obj("sw")
    Case "animate": STGet = obj("animate")
    Case "target": STGet = obj("target")
  End Select
End Function

' Get object property from STArray element (returns object reference)
Function STGetObj(arr, idx, propName)
  Dim obj
  Set obj = arr(idx)
  Select Case LCase(propName)
    Case "primary": Set STGetObj = obj("primary")
    Case "prim": Set STGetObj = obj("prim")
  End Select
End Function

' Set property on STArray element - avoids Wine chaining issue
Sub STSet(arr, idx, propName, val)
  Dim obj
  Set obj = arr(idx)
  Select Case LCase(propName)
    Case "sw": obj("sw") = val
    Case "animate": obj("animate") = val
    Case "target": obj("target") = val
  End Select
End Sub
)";

// Utility: Trim whitespace

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

std::string ScriptPatcher::PatchScript(const std::string& script) {
    std::string result = StripBOM(script);
    bool patched = result.length() != script.length();
    bool classEmulationApplied = false;
    PLOGI.printf("ScriptPatcher: Checking script (length=%zu)", result.length());

    // Class emulation (must run first)
    if (HasClassDefinitions(result)) {
        std::string before = result;
        result = EmulateClasses(result);
        if (result != before) {
            PLOGI.printf("ScriptPatcher: Applied class emulation");
            patched = true;
            classEmulationApplied = true;

            // Replace TypeName with VPX_SafeTypeName to avoid crashes on Dictionary objects
            std::regex typeNameRegex(R"(\bTypeName\s*\()", std::regex::icase);
            result = std::regex_replace(result, typeNameRegex, "VPX_SafeTypeName(");
            PLOGI.printf("ScriptPatcher: Replaced TypeName with VPX_SafeTypeName");

            // Fix nested single-line If statements (Wine VBScript doesn't support them)
            result = PatchNestedSingleLineIf(result);

            // Fix Dictionary boolean conditions: if varName("key") then -> if varName("key") <> 0 then
            // Wine VBScript doesn't support using Dictionary item directly as boolean
            // Match any variable name followed by dictionary access syntax
            std::regex dictBoolPattern(R"(\bif\s+(\w+\s*\(\s*"[^"]+"\s*\))\s+then\b)", std::regex::icase);
            result = std::regex_replace(result, dictBoolPattern, "if $1 <> 0 then");
        }
    }

    // DTArray/STArray
    if (UsesDTArray(result)) {
        PLOGI.printf("ScriptPatcher: Applying DTArray patches");
        result = InjectDropTargetClass(result);
        result = PatchDTArrayDefinitions(result);
        result = PatchDTArrayAccess(result);
        patched = true;
    }
    if (UsesSTArray(result)) {
        PLOGI.printf("ScriptPatcher: Applying STArray patches");
        result = InjectStandupTargetClass(result);
        result = PatchSTArrayDefinitions(result);
        result = PatchSTArrayAccess(result);
        patched = true;
    }

    // Other patches
    if (UsesControllerPause(result)) { result = PatchControllerPause(result); patched = true; }
    if (UsesPuPlayerPlaystopInPlayclear(result)) { result = PatchPuPlayerPlaystopInPlayclear(result); patched = true; }

    { std::string b = result; result = PatchAddScoreParentheses(result); if (result != b) patched = true; }
    { std::string b = result; result = PatchSetAlignedPositionParentheses(result); if (result != b) patched = true; }
    // Disabled for testing
    // { std::string b = result; result = PatchLineContinuationBeforeDot(result); if (result != b) patched = true; }
    // { std::string b = result; result = PatchSingleLineIfElse(result); if (result != b) patched = true; }
    // Disabled: PatchStringConcatenation has a broken regex that corrupts scripts
    // { std::string b = result; result = PatchStringConcatenation(result); if (result != b) patched = true; }

    // Wine VBScript Array Compatibility patches
    if (UsesProblematicArrays(result)) {
        PLOGI.printf("ScriptPatcher: Applying Wine array compatibility patches");
        result = PatchUBoundInConditions(result);
        result = PatchUBoundInForLoops(result);
        result = PatchAllUBound(result);
        result = PatchSafeUBoundArrayAccess(result);
        result = PatchLinearEnvelopeGuard(result);
        result = PatchBallArrayAccess(result);
        result = PatchBallLoopGuard(result);
        result = InjectWineArrayHelpers(result);
        result = PatchReDimWithUBound(result);
        result = Patch2DArrayAccess(result);
        result = PatchArrayElementAssignment(result);
        // Only apply Dict/Array transformation if class emulation was used
        // (these patterns only appear in emulated Dictionary-based classes)
        if (classEmulationApplied) {
            result = PatchDictArrayAccess(result);
            result = PatchArrayObjectPropertyAccess(result);
        }
        // DISABLED: PatchArrayObjectPropertyRead causes compile errors when VPX_GetArrObjProp is used as
        // argument in Sub calls. Wine VBScript's parser doesn't handle this well.
        // For native VPX objects, arr(idx).property works fine. Only emulated classes have issues.
        // result = PatchArrayObjectPropertyRead(result);
        // IMPORTANT: Inject VPX_SetArrObjProp and VPX_GetArrObjProp AFTER the transformations
        // to avoid the helpers' arr(idx).prop patterns being transformed into recursive calls
        result = InjectVPXSetArrObjProp(result);
        patched = true;
    }

    if (patched) {
        PLOGI.printf("ScriptPatcher: Complete (length=%zu)", result.length());
        // Debug: dump patched script to file for inspection
        FILE* debugFile = fopen("/storage/emulated/0/Download/patched_script.vbs", "w");
        if (debugFile) {
            fwrite(result.c_str(), 1, result.length(), debugFile);
            fclose(debugFile);
            PLOGI.printf("ScriptPatcher: Dumped patched script to /storage/emulated/0/Download/patched_script.vbs");
        }
    }
    return result;
}


#endif // __STANDALONE__


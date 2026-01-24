/**
 * @file scriptpatcher_wine.cpp
 * @brief Wine VBScript compatibility patches
 *
 * Wine's VBScript engine has various incompatibilities with standard VBScript:
 * - Array chaining issues (arr(i)(j))
 * - UBound on uninitialized arrays
 * - Ball object property access
 * - DTArray/STArray patterns
 *
 * This module provides patches to work around these issues.
 * Uses Google RE2 for regex operations (much faster than std::regex)
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <sstream>
#include <algorithm>

bool ScriptPatcher::UsesDTArray(const std::string& script) {
    static const RE2 p(R"((?i)DTArray\s*\(\s*\w+\s*\)\s*\(\s*\d+\s*\))");
    return RE2Search(script, p);
}


bool ScriptPatcher::UsesSTArray(const std::string& script) {
    static const RE2 p(R"((?i)STArray\s*\(\s*\w+\s*\)\s*\(\s*\d+\s*\))");
    return RE2Search(script, p);
}


std::string ScriptPatcher::InjectDropTargetClass(const std::string& script) {
    static const RE2 existing(R"((?i)Class\s+DropTarget)");
    if (RE2Search(script, existing)) return script;
    static const RE2 firstDef(R"((?i)(\r?\n)([ \t]*)(DT\d+\s*=\s*Array\s*\())");
    RE2Match m;
    if (RE2FindFirst(script, firstDef, m))
        return script.substr(0, m.position) + "\n" + DROP_TARGET_CLASS + script.substr(m.position);
    static const RE2 optExp(R"((?im)^\s*Option\s+Explicit\s*$)");
    if (RE2FindFirst(script, optExp, m))
        return script.substr(0, m.position + m.length) + "\n" + DROP_TARGET_CLASS + script.substr(m.position + m.length);
    return std::string(DROP_TARGET_CLASS) + script;
}


std::string ScriptPatcher::InjectStandupTargetClass(const std::string& script) {
    static const RE2 existing(R"((?i)Class\s+StandupTarget)");
    if (RE2Search(script, existing)) return script;
    static const RE2 firstDef(R"((?i)(\r?\n)([ \t]*)(ST\d+\s*=\s*Array\s*\())");
    RE2Match m;
    if (RE2FindFirst(script, firstDef, m))
        return script.substr(0, m.position) + "\n" + STANDUP_TARGET_CLASS + script.substr(m.position);
    static const RE2 optExp(R"((?im)^\s*Option\s+Explicit\s*$)");
    if (RE2FindFirst(script, optExp, m))
        return script.substr(0, m.position + m.length) + "\n" + STANDUP_TARGET_CLASS + script.substr(m.position + m.length);
    return std::string(STANDUP_TARGET_CLASS) + script;
}


std::string ScriptPatcher::PatchDTArrayDefinitions(const std::string& script) {
    std::string r = script;
    // Match DT followed by digits and optional suffix letters (DT1, DT18, DT18a, DT18b, etc.)
    // 5 args: primary, secondary, prim, sw, animate (default isDropped=false)
    static const RE2 p5(R"((?i)\b(DT\d+\w*)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))");
    r = RE2Replace(r, p5, "Set \\1 = DropTarget_Create(\\2, \\3, \\4, \\5, \\6, false)");
    // 6 args: primary, secondary, prim, sw, animate, isDropped
    static const RE2 p6(R"((?i)\b(DT\d+\w*)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))");
    r = RE2Replace(r, p6, "Set \\1 = DropTarget_Create(\\2, \\3, \\4, \\5, \\6, \\7)");
    return r;
}


std::string ScriptPatcher::PatchSTArrayDefinitions(const std::string& script) {
    std::string r = script;
    // Match ST followed by digits and optional suffix letters (ST1, ST18, ST18a, ST18b, etc.)
    // 5 args: primary, prim, sw, animate, target
    static const RE2 p(R"((?i)\b(ST\d+\w*)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))");
    r = RE2Replace(r, p, "Set \\1 = StandupTarget_Create(\\2, \\3, \\4, \\5, \\6)");
    return r;
}


std::string ScriptPatcher::PatchDTArrayAccess(const std::string& script) {
    std::string r = script;
    // Wine VBScript can't handle chained indexing like DTArray(i)("key")
    // Use DTGet/DTGetObj/DTSet helper functions instead

    // WRITE patterns: DTArray(i)(idx) = value -> DTSet DTArray, i, "prop", value
    // Must handle writes FIRST before reads to avoid partial transformations
    static const RE2 dw4(R"((?im)(^[ \t]*|:[ \t]*)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\)\s*=\s*([^\r\n:]+))");
    r = RE2Replace(r, dw4, "\\1DTSet DTArray, \\2, \"animate\", \\3");
    static const RE2 dw5(R"((?im)(^[ \t]*|:[ \t]*)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*5\s*\)\s*=\s*([^\r\n:]+))");
    r = RE2Replace(r, dw5, "\\1DTSet DTArray, \\2, \"isDropped\", \\3");
    static const RE2 dw3(R"((?im)(^[ \t]*|:[ \t]*)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\)\s*=\s*([^\r\n:]+))");
    r = RE2Replace(r, dw3, "\\1DTSet DTArray, \\2, \"sw\", \\3");

    // READ patterns for object properties: DTArray(i)(0/1/2) -> DTGetObj(DTArray, i, "prop")
    static const RE2 d0(R"((?i)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*0\s*\))");
    r = RE2Replace(r, d0, "DTGetObj(DTArray, \\1, \"primary\")");
    static const RE2 d1(R"((?i)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*1\s*\))");
    r = RE2Replace(r, d1, "DTGetObj(DTArray, \\1, \"secondary\")");
    static const RE2 d2(R"((?i)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\))");
    r = RE2Replace(r, d2, "DTGetObj(DTArray, \\1, \"prim\")");

    // READ patterns for value properties: DTArray(i)(3/4/5) -> DTGet(DTArray, i, "prop")
    static const RE2 d3(R"((?i)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\))");
    r = RE2Replace(r, d3, "DTGet(DTArray, \\1, \"sw\")");
    static const RE2 d4(R"((?i)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\))");
    r = RE2Replace(r, d4, "DTGet(DTArray, \\1, \"animate\")");
    static const RE2 d5(R"((?i)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*5\s*\))");
    r = RE2Replace(r, d5, "DTGet(DTArray, \\1, \"isDropped\")");

    // Dot notation WRITE: DTArray(i).animate = value -> DTSet DTArray, i, "animate", value
    static const RE2 dpw(R"((?im)(^[ \t]*|:[ \t]*)DTArray\s*\(\s*(\w+)\s*\)\s*\.\s*(sw|animate|isDropped)\s*=\s*([^\r\n:]+))");
    r = RE2Replace(r, dpw, "\\1DTSet DTArray, \\2, \"\\3\", \\4");

    // Dot notation READ for objects: DTArray(i).primary -> DTGetObj(DTArray, i, "primary")
    static const RE2 dpo(R"((?i)DTArray\s*\(\s*(\w+)\s*\)\s*\.\s*(primary|secondary|prim)\b)");
    r = RE2Replace(r, dpo, "DTGetObj(DTArray, \\1, \"\\2\")");

    // Dot notation READ for values: DTArray(i).sw -> DTGet(DTArray, i, "sw")
    static const RE2 dpv(R"((?i)DTArray\s*\(\s*(\w+)\s*\)\s*\.\s*(sw|animate|isDropped)\b)");
    r = RE2Replace(r, dpv, "DTGet(DTArray, \\1, \"\\2\")");

    return r;
}


std::string ScriptPatcher::PatchSTArrayAccess(const std::string& script) {
    std::string r = script;
    // Wine VBScript can't handle chained indexing like STArray(i)("key")
    // Use STGet/STGetObj/STSet helper functions instead

    // WRITE patterns: STArray(i)(idx) = value -> STSet STArray, i, "prop", value
    // Must handle writes FIRST before reads to avoid partial transformations
    static const RE2 sw2(R"((?im)(^[ \t]*|:[ \t]*)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\)\s*=\s*([^\r\n:]+))");
    r = RE2Replace(r, sw2, "\\1STSet STArray, \\2, \"sw\", \\3");
    static const RE2 sw3(R"((?im)(^[ \t]*|:[ \t]*)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\)\s*=\s*([^\r\n:]+))");
    r = RE2Replace(r, sw3, "\\1STSet STArray, \\2, \"animate\", \\3");
    static const RE2 sw4(R"((?im)(^[ \t]*|:[ \t]*)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\)\s*=\s*([^\r\n:]+))");
    r = RE2Replace(r, sw4, "\\1STSet STArray, \\2, \"target\", \\3");

    // READ patterns for object properties: STArray(i)(0/1) -> STGetObj(STArray, i, "prop")
    static const RE2 s0(R"((?i)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*0\s*\))");
    r = RE2Replace(r, s0, "STGetObj(STArray, \\1, \"primary\")");
    static const RE2 s1(R"((?i)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*1\s*\))");
    r = RE2Replace(r, s1, "STGetObj(STArray, \\1, \"prim\")");

    // READ patterns for value properties: STArray(i)(2/3/4) -> STGet(STArray, i, "prop")
    static const RE2 s2(R"((?i)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\))");
    r = RE2Replace(r, s2, "STGet(STArray, \\1, \"sw\")");
    static const RE2 s3(R"((?i)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\))");
    r = RE2Replace(r, s3, "STGet(STArray, \\1, \"animate\")");
    static const RE2 s4(R"((?i)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\))");
    r = RE2Replace(r, s4, "STGet(STArray, \\1, \"target\")");

    // Dot notation WRITE: STArray(i).animate = value -> STSet STArray, i, "animate", value
    static const RE2 spw(R"((?im)(^[ \t]*|:[ \t]*)STArray\s*\(\s*(\w+)\s*\)\s*\.\s*(sw|animate|target)\s*=\s*([^\r\n:]+))");
    r = RE2Replace(r, spw, "\\1STSet STArray, \\2, \"\\3\", \\4");

    // Dot notation READ for objects: STArray(i).primary -> STGetObj(STArray, i, "primary")
    static const RE2 spo(R"((?i)STArray\s*\(\s*(\w+)\s*\)\s*\.\s*(primary|prim)\b)");
    r = RE2Replace(r, spo, "STGetObj(STArray, \\1, \"\\2\")");

    // Dot notation READ for values: STArray(i).sw -> STGet(STArray, i, "sw")
    static const RE2 spv(R"((?i)STArray\s*\(\s*(\w+)\s*\)\s*\.\s*(sw|animate|target)\b)");
    r = RE2Replace(r, spv, "STGet(STArray, \\1, \"\\2\")");

    return r;
}


bool ScriptPatcher::UsesControllerPause(const std::string& script) {
    static const RE2 p(R"((?i)Controller\.Pause\s*=)");
    return RE2Search(script, p);
}


std::string ScriptPatcher::PatchControllerPause(const std::string& script) {
    std::string r = script;

    // First handle colon-separated statements (e.g., Sub Foo:Controller.Pause = True:End Sub)
    // These can't be safely commented out, so remove them entirely
    // Pattern matches :Controller.Pause = Value followed by :
    static const RE2 p1(R"((?i):[ \t]*Controller\.Pause\s*=\s*(True|False)[ \t]*:)");
    r = RE2Replace(r, p1, ":");

    // Then handle statements on their own lines (comment them out)
    static const RE2 p2(R"((?i)(\s*)(Controller\.Pause\s*=\s*(True|False)))");
    r = RE2Replace(r, p2, "\\1' \\2 ' Wine/Android");

    return r;
}


bool ScriptPatcher::UsesPuPlayerPlaystopInPlayclear(const std::string& script) {
    static const RE2 p(R"((?is)if\s+chan\s*=\s*pBackglass\s+Then[\s\S]*?PuPlayer\.playstop\s+pDMD)");
    return RE2Search(script, p);
}


std::string ScriptPatcher::PatchPuPlayerPlaystopInPlayclear(const std::string& script) {
    std::string r = script;
    static const RE2 p(R"((?i)(if\s+chan\s*=\s*pBackglass\s+Then\s*[\r\n]+)([ \t]*)(PuPlayer\.playstop\s+pDMD))");
    r = RE2Replace(r, p, "\\1\\2' \\3 ' Android");
    return r;
}

std::string ScriptPatcher::StripBOM(const std::string& script) {
    if (script.length() >= 3 && (unsigned char)script[0] == 0xEF && (unsigned char)script[1] == 0xBB && (unsigned char)script[2] == 0xBF) {
        PLOGI.printf("ScriptPatcher: Stripping UTF-8 BOM");
        return script.substr(3);
    }
    if (script.length() >= 2 && (unsigned char)script[0] == 0xFF && (unsigned char)script[1] == 0xFE) {
        PLOGI.printf("ScriptPatcher: Stripping UTF-16 LE BOM");
        return script.substr(2);
    }
    if (script.length() >= 2 && (unsigned char)script[0] == 0xFE && (unsigned char)script[1] == 0xFF) {
        PLOGI.printf("ScriptPatcher: Stripping UTF-16 BE BOM");
        return script.substr(2);
    }
    return script;
}


std::string ScriptPatcher::PatchAddScoreParentheses(const std::string& script) {
    std::string r = script;
    static const RE2 p(R"((?i)AddScore\s+\(([^)]+\([^)]*\)[^)]*)\)\s*\+\s*(\w+\([^)]*\)))");
    r = RE2Replace(r, p, "AddScore ((\\1)+\\2)");
    return r;
}


std::string ScriptPatcher::PatchSetAlignedPositionParentheses(const std::string& script) {
    std::string r = script;
    static const RE2 p(R"((?i)\.SetAlignedPosition\s+\(\(([^)]+)\)\*(\d+)\)\+(\d+)\s*,)");
    r = RE2Replace(r, p, ".SetAlignedPosition (((\\1)*\\2)+\\3),");
    return r;
}


std::string ScriptPatcher::PatchLineContinuationBeforeDot(const std::string& script) {
    std::string r = script;
    static const RE2 p(R"((?i)(\w|\))\s+_\s*\r?\n\s*\.)");
    r = RE2Replace(r, p, "\\1. _\n");
    return r;
}



// Wine VBScript Array Compatibility Helpers
// Wine's VBScript has issues with UBound on uninitialized arrays and 2D array access

bool ScriptPatcher::UsesProblematicArrays(const std::string& script) {
    // Check for patterns that cause Wine VBScript issues:
    // 1. UBound in If conditions before ReDim
    // 2. 2D array access
    static const RE2 p1(R"((?i)If\s+UBound\s*\()");
    static const RE2 p2(R"((?i)\w+\s*\(\s*\d+\s*,\s*\d+\s*\))");
    return RE2Search(script, p1) || RE2Search(script, p2);
}


std::string ScriptPatcher::InjectWineArrayHelpers(const std::string& script) {
    // Inject helper functions at the start of the script (after Option Explicit if present)
    // Check if Atn2 is USED in the script (called as a function)
    static const RE2 atn2UsedRegex(R"((?i)\bAtn2\s*\()");
    bool usesAtn2 = RE2Search(script, atn2UsedRegex);
    // Check if Atn2 is properly DEFINED (after newline + optional whitespace, not in comment)
    static const RE2 atn2DefRegex(R"((?i)(^|\n)\s*Function\s+Atn2\b)");
    bool hasAtn2 = RE2Search(script, atn2DefRegex);

    std::string helpers = R"(
' Wine VBScript Array Compatibility Helpers (Auto-injected by ScriptPatcher)
Function VPX_SafeUBound(arr)
    On Error Resume Next
    VPX_SafeUBound = -1
    If TypeName(arr) = "Dictionary" Then
        VPX_SafeUBound = arr.Count - 1
    Else
        VPX_SafeUBound = UBound(arr)
    End If
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_SafeUBound error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Function

' Safe array element access - returns Empty if index out of bounds
Function VPX_SafeGet(arr, idx)
    On Error Resume Next
    VPX_SafeGet = Empty
    If idx >= 0 Then VPX_SafeGet = arr(idx)
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_SafeGet(" & idx & ") error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Function

' Safe ball X property access - returns -99999 if ball is invalid/Nothing
Function VPX_BallX(ball)
    On Error Resume Next
    VPX_BallX = -99999
    If IsObject(ball) Then
        If Not ball Is Nothing Then VPX_BallX = ball.X
    End If
    On Error Goto 0
End Function

' Safe ball Y property access - returns -99999 if ball is invalid/Nothing
Function VPX_BallY(ball)
    On Error Resume Next
    VPX_BallY = -99999
    If IsObject(ball) Then
        If Not ball Is Nothing Then VPX_BallY = ball.Y
    End If
    On Error Goto 0
End Function

' Safe ball Z property access
Function VPX_BallZ(ball)
    On Error Resume Next
    VPX_BallZ = -99999
    If IsObject(ball) Then
        If Not ball Is Nothing Then VPX_BallZ = ball.Z
    End If
    On Error Goto 0
End Function

' Safe ball VelX property access
Function VPX_BallVelX(ball)
    On Error Resume Next
    VPX_BallVelX = 0
    If IsObject(ball) Then
        If Not ball Is Nothing Then VPX_BallVelX = ball.VelX
    End If
    On Error Goto 0
End Function

' Safe ball VelY property access
Function VPX_BallVelY(ball)
    On Error Resume Next
    VPX_BallVelY = 0
    If IsObject(ball) Then
        If Not ball Is Nothing Then VPX_BallVelY = ball.VelY
    End If
    On Error Goto 0
End Function

' Safe ball VelZ property access
Function VPX_BallVelZ(ball)
    On Error Resume Next
    VPX_BallVelZ = 0
    If IsObject(ball) Then
        If Not ball Is Nothing Then VPX_BallVelZ = ball.VelZ
    End If
    On Error Goto 0
End Function

' Safe ball ID property access
Function VPX_BallID(ball)
    On Error Resume Next
    VPX_BallID = -1
    If IsObject(ball) Then
        If Not ball Is Nothing Then VPX_BallID = ball.ID
    End If
    On Error Goto 0
End Function

' Safe ball Color property access
Function VPX_BallColor(ball)
    On Error Resume Next
    VPX_BallColor = 0
    If IsObject(ball) Then
        If Not ball Is Nothing Then VPX_BallColor = ball.Color
    End If
    On Error Goto 0
End Function


' Check if ball object is valid and usable
Function VPX_IsValidBall(ball)
    On Error Resume Next
    VPX_IsValidBall = False
    If IsEmpty(ball) Then Exit Function
    If IsNull(ball) Then Exit Function
    If Not IsObject(ball) Then Exit Function
    If ball Is Nothing Then Exit Function
    Dim testX : testX = ball.X
    If Err.Number = 0 Then VPX_IsValidBall = True
    Err.Clear
    On Error Goto 0
End Function

' Safe TypeName wrapper - avoids crash on Dictionary objects (emulated classes)
Function VPX_SafeTypeName(obj)
    On Error Resume Next
    Dim hasClass : hasClass = obj.Exists("__class__")
    If Err.Number = 0 Then
        If hasClass Then
            VPX_SafeTypeName = obj("__class__")
            On Error Goto 0
            Exit Function
        End If
        ' obj has Exists method but no __class__ - raw Dictionary
        VPX_SafeTypeName = "Dictionary"
        On Error Goto 0
        Exit Function
    End If
    Err.Clear
    On Error Goto 0
    ' Not a Dictionary - safe to call TypeName
    VPX_SafeTypeName = TypeName(obj)
End Function

Function VPX_SafeArrayGet(arr, idx)
    On Error Resume Next
    VPX_SafeArrayGet = Empty
    VPX_SafeArrayGet = arr(idx)
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_SafeArrayGet(" & idx & ") error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Function

Function VPX_SafeArray2DGet(arr, idx1, idx2)
    On Error Resume Next
    VPX_SafeArray2DGet = Empty
    VPX_SafeArray2DGet = arr(idx1, idx2)
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_SafeArray2DGet(" & idx1 & "," & idx2 & ") error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Function

Sub VPX_SafeArraySet(arr, idx, val)
    On Error Resume Next
    arr(idx) = val
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_SafeArraySet(" & idx & ") error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Sub

Sub VPX_SafeReDim(ByRef arr, newSize)
    On Error Resume Next
    ReDim Preserve arr(newSize)
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_SafeReDim(" & newSize & ") error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Sub

Function VPX_ArrObj(arr, idx)
    On Error Resume Next
    Set VPX_ArrObj = arr(idx)
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_ArrObj(" & idx & ") error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Function

' Helper for chained Dictionary/array assignment: dict("key")(idx) = val
Sub VPX_SetDictArrItem(dict, key, idx, val)
    On Error Resume Next
    Dim arr : arr = dict(key)
    arr(idx) = val
    dict(key) = arr
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_SetDictArrItem error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Sub

' Helper for chained Dictionary/array read: dict("key")(idx)
Function VPX_GetDictArrItem(dict, key, idx)
    On Error Resume Next
    VPX_GetDictArrItem = Empty
    Dim arr : arr = dict(key)
    VPX_GetDictArrItem = arr(idx)
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_GetDictArrItem error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Function
)";

    // Only add Atn2 if script doesn't define its own
    std::string atn2Helper = R"(
Function Atn2(y, x)
    Dim PI: PI = 3.14159265358979
    If x > 0 Then
        Atn2 = Atn(y / x)
    ElseIf x < 0 Then
        If y >= 0 Then
            Atn2 = Atn(y / x) + PI
        Else
            Atn2 = Atn(y / x) - PI
        End If
    Else
        If y > 0 Then
            Atn2 = PI / 2
        ElseIf y < 0 Then
            Atn2 = -PI / 2
        Else
            Atn2 = 0
        End If
    End If
End Function
)";

    // Inject Atn2 if it's USED but not properly DEFINED
    if (usesAtn2 && !hasAtn2) {
        helpers += atn2Helper;
        PLOGI.printf("ScriptPatcher: Injecting Atn2 helper (used but not defined)");
    } else if (hasAtn2) {
        PLOGI.printf("ScriptPatcher: Script already defines Atn2, skipping injection");
    }

    // Add nested array assignment helper (Wine cannot do arr(i)(j) = value directly)
    helpers += R"(
'VPX_SetNestedArrayElem - Wine workaround for nested array assignment
Sub VPX_SetNestedArrayElem(ByRef arr, idx1, idx2, value)
    Dim temp
    temp = arr(idx1)
    temp(idx2) = value
    arr(idx1) = temp
End Sub
)";
    helpers += "' End Wine VBScript Array Compatibility Helpers\n\n";

    std::string r = script;

    // Find insertion point - after Option Explicit or at start
    static const RE2 optionExplicit(R"((?i)(Option\s+Explicit[^\r\n]*[\r\n]+))");
    RE2Match match;
    if (RE2FindFirst(r, optionExplicit, match)) {
        r = r.substr(0, match.position + match.length) + helpers + r.substr(match.position + match.length);
    } else {
        // Insert at the very beginning
        r = helpers + r;
    }

    return r;
}


// Inject VPX_SetArrObjProp and VPX_GetArrObjProp AFTER array property transformations
std::string ScriptPatcher::InjectVPXSetArrObjProp(const std::string& script) {
    // These helpers must be injected AFTER PatchArrayObjectPropertyAccess and PatchArrayObjectPropertyRead run,
    // otherwise the arr(idx).prop patterns inside these helpers would be transformed
    // into recursive calls, causing infinite recursion.
    std::string helper = R"(
' VPX_GetArrObjProp - Injected after array property read transformation
Function VPX_GetArrObjProp(arr, idx, propName)
    On Error Resume Next
    VPX_GetArrObjProp = Empty
    Select Case LCase(propName)
        Case "x": VPX_GetArrObjProp = arr(idx).x
        Case "y": VPX_GetArrObjProp = arr(idx).y
        Case "z": VPX_GetArrObjProp = arr(idx).z
        Case "velx": VPX_GetArrObjProp = arr(idx).velx
        Case "vely": VPX_GetArrObjProp = arr(idx).vely
        Case "velz": VPX_GetArrObjProp = arr(idx).velz
        Case "id": VPX_GetArrObjProp = arr(idx).id
        Case "visible": VPX_GetArrObjProp = arr(idx).visible
        Case "state": VPX_GetArrObjProp = arr(idx).state
        Case "height": VPX_GetArrObjProp = arr(idx).height
        Case "mass": VPX_GetArrObjProp = arr(idx).mass
        Case "radius": VPX_GetArrObjProp = arr(idx).radius
        Case "name": VPX_GetArrObjProp = arr(idx).name
        Case "image": VPX_GetArrObjProp = arr(idx).image
        Case "bulbhaloheight": VPX_GetArrObjProp = arr(idx).BulbHaloHeight
        Case "intensity": VPX_GetArrObjProp = arr(idx).intensity
        Case "intensityscale": VPX_GetArrObjProp = arr(idx).intensityscale
        Case "opacity": VPX_GetArrObjProp = arr(idx).opacity
        Case "collidable": VPX_GetArrObjProp = arr(idx).collidable
        Case "rotation": VPX_GetArrObjProp = arr(idx).rotation
        Case "rotz": VPX_GetArrObjProp = arr(idx).rotz
        Case "rotx": VPX_GetArrObjProp = arr(idx).rotx
        Case "roty": VPX_GetArrObjProp = arr(idx).roty
        Case "size": VPX_GetArrObjProp = arr(idx).size
        Case "enabled": VPX_GetArrObjProp = arr(idx).enabled
        Case "timerinterval": VPX_GetArrObjProp = arr(idx).timerinterval
        Case "timerenabled": VPX_GetArrObjProp = arr(idx).timerenabled
        Case "uservalue": VPX_GetArrObjProp = arr(idx).uservalue
        Case "color": VPX_GetArrObjProp = arr(idx).color
        Case "falloff": VPX_GetArrObjProp = arr(idx).falloff
        Case "falloffpower": VPX_GetArrObjProp = arr(idx).falloffpower
        Case "blend": VPX_GetArrObjProp = arr(idx).blend
        Case "material": VPX_GetArrObjProp = arr(idx).material
        Case "isdropped": VPX_GetArrObjProp = arr(idx).isdropped
        Case "objrotz": VPX_GetArrObjProp = arr(idx).objrotz
        Case "objrotx": VPX_GetArrObjProp = arr(idx).objrotx
        Case "objroty": VPX_GetArrObjProp = arr(idx).objroty
        Case "transx": VPX_GetArrObjProp = arr(idx).transx
        Case "transy": VPX_GetArrObjProp = arr(idx).transy
        Case "transz": VPX_GetArrObjProp = arr(idx).transz
        Case "scalex": VPX_GetArrObjProp = arr(idx).scalex
        Case "scaley": VPX_GetArrObjProp = arr(idx).scaley
        Case "scalez": VPX_GetArrObjProp = arr(idx).scalez
    End Select
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_GetArrObjProp(" & idx & "," & propName & ") error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Function

' VPX_SetArrObjProp - Injected after array property access transformation
Sub VPX_SetArrObjProp(arr, idx, propName, val)
    On Error Resume Next
    Select Case LCase(propName)
        Case "visible": arr(idx).visible = val
        Case "x": arr(idx).x = val
        Case "y": arr(idx).y = val
        Case "z": arr(idx).z = val
        Case "height": arr(idx).height = val
        Case "opacity": arr(idx).opacity = val
        Case "state": arr(idx).state = val
        Case "bulbhaloheight": arr(idx).BulbHaloHeight = val
        Case "intensity": arr(idx).intensity = val
        Case "image": arr(idx).image = val
        Case "collidable": arr(idx).collidable = val
        Case "rotation": arr(idx).rotation = val
        Case "rotz": arr(idx).rotz = val
        Case "rotx": arr(idx).rotx = val
        Case "roty": arr(idx).roty = val
        Case "size": arr(idx).size = val
        Case "enabled": arr(idx).enabled = val
        Case "timerinterval": arr(idx).timerinterval = val
        Case "timerenabled": arr(idx).timerenabled = val
        Case "uservalue": arr(idx).uservalue = val
        Case "color": arr(idx).color = val
        Case "falloff": arr(idx).falloff = val
        Case "falloffpower": arr(idx).falloffpower = val
        Case "blend": arr(idx).blend = val
        Case "material": arr(idx).material = val
        Case "isdropped": arr(idx).isdropped = val
        Case "objrotz": arr(idx).objrotz = val
        Case "objrotx": arr(idx).objrotx = val
        Case "objroty": arr(idx).objroty = val
        Case "intensityscale": arr(idx).intensityscale = val
        Case "transx": arr(idx).transx = val
        Case "transy": arr(idx).transy = val
        Case "transz": arr(idx).transz = val
        Case "scalex": arr(idx).scalex = val
        Case "scaley": arr(idx).scaley = val
        Case "scalez": arr(idx).scalez = val
    End Select
    If Err.Number <> 0 Then Script.Print "VPX WARNING: VPX_SetArrObjProp(" & idx & "," & propName & ") error " & Err.Number & " - " & Err.Description : Err.Clear
    On Error Goto 0
End Sub
)";

    std::string r = script;

    // Find insertion point - after Option Explicit or at start
    static const RE2 optionExplicit(R"((?i)(Option\s+Explicit[^\r\n]*[\r\n]+))");
    RE2Match match;
    if (RE2FindFirst(r, optionExplicit, match)) {
        r = r.substr(0, match.position + match.length) + helper + r.substr(match.position + match.length);
    } else {
        // Insert at the very beginning
        r = helper + r;
    }

    return r;
}


std::string ScriptPatcher::PatchUBoundInConditions(const std::string& script) {
    std::string r = script;

    // Replace "If UBound(array)" with "If VPX_SafeUBound(array)"
    static const RE2 p(R"((?i)If\s+UBound\s*\(\s*(\w+)\s*\))");
    r = RE2Replace(r, p, "If VPX_SafeUBound(\\1)");

    return r;
}


std::string ScriptPatcher::PatchUBoundInForLoops(const std::string& script) {
    std::string r = script;

    // Replace "For x = ... to UBound(array)" with "For x = ... to VPX_SafeUBound(array)"
    // This handles uninitialized arrays that would throw on UBound()
    static const RE2 p(R"((?i)\bto\s+UBound\s*\(\s*(\w+)\s*\))");
    r = RE2Replace(r, p, "to VPX_SafeUBound(\\1)");

    return r;
}

std::string ScriptPatcher::PatchAllUBound(const std::string& script) {
    std::string r = script;
    // Replace ALL remaining uBound( calls with VPX_SafeUBound(
    static const RE2 p(R"((?i)\buBound\s*\()");
    r = RE2Replace(r, p, "VPX_SafeUBound(");
    return r;
}

std::string ScriptPatcher::PatchSafeUBoundArrayAccess(const std::string& script) {
    std::string r = script;
    // Replace arr(VPX_SafeUBound(arr)) with VPX_SafeGet(arr, VPX_SafeUBound(arr))
    // BUT only when NOT followed by = (i.e., when reading, not writing)
    // This prevents out-of-bounds when VPX_SafeUBound returns -1 for empty arrays
    // NOTE: RE2 doesn't support backreferences (\1) or negative lookahead (?!),
    // so we match both names and check for trailing = in the callback
    static const RE2 p(R"((?i)(\w+)\s*\(\s*VPX_SafeUBound\s*\(\s*(\w+)\s*\)\s*\))");
    r = RE2ReplaceWithCallback(r, p, [&r](const RE2Match& m) -> std::string {
        std::string arr1 = m.groups.size() > 0 ? m.groups[0] : "";
        std::string arr2 = m.groups.size() > 1 ? m.groups[1] : "";
        // Check if both array names match (case-insensitive)
        std::string lower1 = arr1, lower2 = arr2;
        std::transform(lower1.begin(), lower1.end(), lower1.begin(), ::tolower);
        std::transform(lower2.begin(), lower2.end(), lower2.begin(), ::tolower);
        if (lower1 != lower2) {
            return m.full_match;  // Names don't match, keep original
        }
        // Check if followed by = (assignment) - if so, don't transform
        // This replicates the original (?!\s*=) negative lookahead
        size_t afterMatch = m.position + m.length;
        while (afterMatch < r.length() && (r[afterMatch] == ' ' || r[afterMatch] == '\t')) {
            afterMatch++;
        }
        if (afterMatch < r.length() && r[afterMatch] == '=') {
            // Check it's not == (comparison)
            if (afterMatch + 1 >= r.length() || r[afterMatch + 1] != '=') {
                return m.full_match;  // This is an assignment, keep original
            }
        }
        return "VPX_SafeGet(" + arr1 + ", VPX_SafeUBound(" + arr2 + "))";
    });
    return r;
}

std::string ScriptPatcher::PatchLinearEnvelopeGuard(const std::string& script) {
    std::string r = script;
    // Add early-exit guard to LinearEnvelope function for empty arrays
    static const RE2 p(R"((?i)(Function\s+LinearEnvelope\s*\([^)]+\)\s*\r?\n)(\s*dim\s+y\b))");
    r = RE2Replace(r, p, "\\1    If VPX_SafeUBound(xKeyFrame) < 0 Then LinearEnvelope = 0 : Exit Function\n\\2");
    return r;
}

std::string ScriptPatcher::PatchBallArrayAccess(const std::string& script) {
    std::string r = script;

    // Ball arrays: gBOT (GetBallsOfTable) and BOT
    // Properties: X, Y, Z, VelX, VelY, VelZ, ID, Color
    // NOTE: RE2 doesn't support negative lookahead (?!), so we handle with callbacks

    // gBOT array
    static const RE2 gbot_x(R"((?i)\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*X\b)");
    r = RE2ReplaceWithCallback(r, gbot_x, [](const RE2Match& m) -> std::string {
        return "VPX_BallX(gBOT(" + m[1] + "))";
    });

    static const RE2 gbot_y(R"((?i)\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Y\b)");
    r = RE2ReplaceWithCallback(r, gbot_y, [](const RE2Match& m) -> std::string {
        return "VPX_BallY(gBOT(" + m[1] + "))";
    });

    static const RE2 gbot_z(R"((?i)\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Z\b)");
    r = RE2ReplaceWithCallback(r, gbot_z, [](const RE2Match& m) -> std::string {
        return "VPX_BallZ(gBOT(" + m[1] + "))";
    });

    static const RE2 gbot_velx(R"((?i)\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelX\b)");
    r = RE2ReplaceWithCallback(r, gbot_velx, [](const RE2Match& m) -> std::string {
        return "VPX_BallVelX(gBOT(" + m[1] + "))";
    });

    static const RE2 gbot_vely(R"((?i)\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelY\b)");
    r = RE2ReplaceWithCallback(r, gbot_vely, [](const RE2Match& m) -> std::string {
        return "VPX_BallVelY(gBOT(" + m[1] + "))";
    });

    static const RE2 gbot_velz(R"((?i)\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelZ\b)");
    r = RE2ReplaceWithCallback(r, gbot_velz, [](const RE2Match& m) -> std::string {
        return "VPX_BallVelZ(gBOT(" + m[1] + "))";
    });

    static const RE2 gbot_id(R"((?i)\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*ID\b)");
    r = RE2ReplaceWithCallback(r, gbot_id, [](const RE2Match& m) -> std::string {
        return "VPX_BallID(gBOT(" + m[1] + "))";
    });

    static const RE2 gbot_color(R"((?i)\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Color\b)");
    r = RE2ReplaceWithCallback(r, gbot_color, [](const RE2Match& m) -> std::string {
        return "VPX_BallColor(gBOT(" + m[1] + "))";
    });

    // BOT array (same patterns)
    static const RE2 bot_x(R"((?i)\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*X\b)");
    r = RE2ReplaceWithCallback(r, bot_x, [](const RE2Match& m) -> std::string {
        return "VPX_BallX(BOT(" + m[1] + "))";
    });

    static const RE2 bot_y(R"((?i)\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Y\b)");
    r = RE2ReplaceWithCallback(r, bot_y, [](const RE2Match& m) -> std::string {
        return "VPX_BallY(BOT(" + m[1] + "))";
    });

    static const RE2 bot_z(R"((?i)\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Z\b)");
    r = RE2ReplaceWithCallback(r, bot_z, [](const RE2Match& m) -> std::string {
        return "VPX_BallZ(BOT(" + m[1] + "))";
    });

    static const RE2 bot_velx(R"((?i)\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelX\b)");
    r = RE2ReplaceWithCallback(r, bot_velx, [](const RE2Match& m) -> std::string {
        return "VPX_BallVelX(BOT(" + m[1] + "))";
    });

    static const RE2 bot_vely(R"((?i)\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelY\b)");
    r = RE2ReplaceWithCallback(r, bot_vely, [](const RE2Match& m) -> std::string {
        return "VPX_BallVelY(BOT(" + m[1] + "))";
    });

    static const RE2 bot_velz(R"((?i)\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelZ\b)");
    r = RE2ReplaceWithCallback(r, bot_velz, [](const RE2Match& m) -> std::string {
        return "VPX_BallVelZ(BOT(" + m[1] + "))";
    });

    static const RE2 bot_id(R"((?i)\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*ID\b)");
    r = RE2ReplaceWithCallback(r, bot_id, [](const RE2Match& m) -> std::string {
        return "VPX_BallID(BOT(" + m[1] + "))";
    });

    static const RE2 bot_color(R"((?i)\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Color\b)");
    r = RE2ReplaceWithCallback(r, bot_color, [](const RE2Match& m) -> std::string {
        return "VPX_BallColor(BOT(" + m[1] + "))";
    });

    return r;
}

std::string ScriptPatcher::PatchBallLoopGuard(const std::string& script) {
    std::string r = script;

    // Add guard to For loops over gBOT: skip if ball is invalid
    // This is complex - instead, let's wrap entire loop bodies in On Error Resume Next
    // by adding it after For statements that iterate over gBOT
    static const RE2 p(R"((?i)(For\s+(\w+)\s*=\s*\d+\s+to\s+VPX_SafeUBound\s*\(\s*gBOT\s*\)\s*\r?\n))");
    r = RE2Replace(r, p, "\\1        On Error Resume Next\n");

    // Same for BOT
    static const RE2 p2(R"((?i)(For\s+(\w+)\s*=\s*\d+\s+to\s+VPX_SafeUBound\s*\(\s*BOT\s*\)\s*\r?\n))");
    r = RE2Replace(r, p2, "\\1        On Error Resume Next\n");

    return r;
}


std::string ScriptPatcher::PatchReDimWithUBound(const std::string& script) {
    std::string r = script;
    // For now, the SafeUBound should handle most cases
    return r;
}


std::string ScriptPatcher::Patch2DArrayAccess(const std::string& script) {
    std::string r = script;

    // RampBalls(0,0) pattern - this is checking if array element is falsy
    // Replace with a try-catch style approach or just default to a safe value
    static const RE2 p(R"((?i)if\s+not\s+(\w+)\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)\s+then)");
    r = RE2Replace(r, p, "If Not VPX_SafeArray2DGet(\\1, \\2, \\3) Then");

    return r;
}


std::string ScriptPatcher::PatchArrayElementAssignment(const std::string& script) {
    std::string r = script;

    // For known problematic arrays with True/False values - only at statement start
    // Use inline error handling instead of VPX_SafeArraySet
    static const RE2 p1(R"((?im)(^[ \t]*)(bBallInTrough|rolling)\s*\(\s*(\w+)\s*\)\s*=\s*(True|False))");
    r = RE2Replace(r, p1, "\\1On Error Resume Next : \\2(\\3) = \\4 : On Error Goto 0");

    // After Then/Else - use colon-separated inline
    static const RE2 p2(R"((?i)(\bThen[ \t]+|\bElse[ \t]+)(bBallInTrough|rolling)\s*\(\s*(\w+)\s*\)\s*=\s*(True|False))");
    r = RE2Replace(r, p2, "\\1On Error Resume Next : \\2(\\3) = \\4 : On Error Goto 0");

    return r;
}


// ============================================================================
// WINE BUG WORKAROUND: Nested array assignment
// Wine VBScript can READ nested arrays: value = arr(i)(j)
// But it CANNOT WRITE to them: arr(i)(j) = value
// Transform to helper call: VPX_SetNestedArrayElem arr, i, j, value
// ============================================================================

std::string ScriptPatcher::PatchNestedArrayAssignment(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Pattern: ArrayName(index1)(index2) = expression
    // Must be at statement start (line start, after :, after Then)
    // Capture: ArrayName, index1, index2, value
    static const RE2 p(R"((?im)(^[ \t]*|:[ \t]*|Then[ \t]+)(\w+)\s*\(\s*([^)]+)\s*\)\s*\(\s*([^)]+)\s*\)\s*=\s*([^\r\n:]+))");

    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        count++;
        return m[1] + "VPX_SetNestedArrayElem " + m[2] + ", " + m[3] + ", " + m[4] + ", " + m[5];
    });

    if (count > 0) {
        PLOGI.printf("ScriptPatcher: Transformed %d nested array assignments", count);
    }

    return r;
}


std::string ScriptPatcher::PatchDictArrayAccess(const std::string& script) {
    std::string r = script;

    // Wine VBScript doesn't support chained Dictionary/array access: dict("key")(idx)
    // Transform: varName("key")(idx) = value
    // To: VPX_SetDictArrItem varName, "key", idx, value

    // Pattern for assignment: dict("key")(idx) = value (at statement start)
    static const RE2 assignPattern(R"RE((?i)(^|\n)([ \t]*)(\w+)\s*\(\s*"([^"]+)"\s*\)\s*\(\s*([^)]+)\s*\)\s*=\s*([^\r\n:]+))RE");
    r = RE2Replace(r, assignPattern, "\\1\\2VPX_SetDictArrItem \\3, \"\\4\", \\5, \\6");

    // Also handle after colon (statement separator)
    static const RE2 colonPattern(R"RE((?i)(:[ \t]*)(\w+)\s*\(\s*"([^"]+)"\s*\)\s*\(\s*([^)]+)\s*\)\s*=\s*([^\r\n:]+))RE");
    r = RE2Replace(r, colonPattern, "\\1VPX_SetDictArrItem \\2, \"\\3\", \\4, \\5");

    // Transform READ access: dict("key")(idx) -> VPX_GetDictArrItem(dict, "key", idx)
    // Need to be careful not to match patterns already transformed to VPX_SetDictArrItem
    // RE2 doesn't support (?!), so we use callback to filter
    static const RE2 readPattern(R"RE((?i)\b(\w+)\s*\(\s*"([^"]+)"\s*\)\s*\(\s*([^)]+)\s*\))RE");
    r = RE2ReplaceWithCallback(r, readPattern, [](const RE2Match& m) -> std::string {
        std::string varName = m.groups.size() > 0 ? m.groups[0] : "";
        std::string varLower = varName;
        std::transform(varLower.begin(), varLower.end(), varLower.begin(), ::tolower);
        if (varLower == "vpx_setdictarritem") {
            return m.full_match;  // Don't transform VPX_SetDictArrItem calls
        }
        std::string key = m.groups.size() > 1 ? m.groups[1] : "";
        std::string idx = m.groups.size() > 2 ? m.groups[2] : "";
        return "VPX_GetDictArrItem(" + varName + ", \"" + key + "\", " + idx + ")";
    });

    PLOGI.printf("ScriptPatcher: Applied Dictionary/array access patch");
    return r;
}


std::string ScriptPatcher::PatchArrayObjectPropertyAccess(const std::string& script) {
    std::string r = script;

    // Wine VBScript doesn't support Array(idx).property = value syntax
    // Transform: ArrayName(idx).property = value
    // To: VPX_SetArrObjProp ArrayName, idx, "property", value

    // Pattern requires statement start position to avoid matching comparisons
    // IMPORTANT: Use [^,)]+ to skip 2D array accesses like arr(i,j).prop - only transform 1D arrays
    // RE2 doesn't support lookahead, capture trailing boundary and restore
    static const RE2 p(R"((?im)(^[ \t]*|:[ \t]*|\bThen[ \t]+|\bElse[ \t]+)(\w+)\s*\(\s*([^,)]+)\s*\)\s*\.(\w+)\s*=\s*([^:\r\n]+?)([ \t]*(?::|'|\bThen\b|\bElse\b|\r|\n|$)))");
    r = RE2Replace(r, p, "\\1VPX_SetArrObjProp \\2, \\3, \"\\4\", \\5\\6");

    return r;
}


std::string ScriptPatcher::PatchArrayObjectPropertyRead(const std::string& script) {
    std::string r = script;

    // Wine VBScript doesn't support Array(idx).property READ syntax either
    // Transform: ArrayName(idx).property
    // To: VPX_GetArrObjProp(ArrayName, idx, "property")

    // List of known VPX object properties that should be transformed
    static const std::unordered_set<std::string> vpxProperties = {
        "x", "y", "z", "velx", "vely", "velz", "id", "visible", "state",
        "height", "mass", "radius", "name", "image", "bulbhaloheight",
        "intensity", "opacity", "collidable", "rotation", "rotz", "rotx",
        "roty", "size", "enabled", "timerinterval", "timerenabled",
        "uservalue", "color", "falloff", "falloffpower", "blend",
        "material", "isdropped", "objrotz", "objrotx", "objroty",
        "transx", "transy", "transz", "scalex", "scaley", "scalez",
        "intensityscale"
    };

    // List of VBScript built-in functions to exclude
    static const std::unordered_set<std::string> excludedFunctions = {
        "abs", "atn", "cos", "sin", "tan", "exp", "log", "sqr", "int", "fix",
        "sgn", "rnd", "round", "hex", "oct", "chr", "asc", "len", "mid", "left",
        "right", "instr", "instrrev", "lcase", "ucase", "ltrim", "rtrim", "trim",
        "space", "string", "replace", "split", "join", "cint", "clng", "csng",
        "cdbl", "cstr", "cbool", "cdate", "dateadd", "datediff", "datepart",
        "dateserial", "datevalue", "day", "month", "year", "hour", "minute",
        "second", "now", "date", "time", "timer", "weekday", "monthname",
        "isarray", "isdate", "isempty", "isnull", "isnumeric", "isobject",
        "typename", "vartype", "array", "filter", "getref", "eval", "execute",
        "executeglobal", "msgbox", "inputbox", "createobject", "getobject",
        "rgb", "ubound", "lbound", "redim", "erase"
    };

    // IMPORTANT: Use [^,)]+ to skip 2D array accesses like arr(i,j).prop - only transform 1D arrays
    static const RE2 p(R"((?i)(\w+)\s*\(\s*([^,)]+)\s*\)\s*\.(\w+)\b)");

    r = RE2ReplaceWithCallback(r, p, [&](const RE2Match& m) -> std::string {
        std::string funcName = m[1];
        std::string funcNameLower = funcName;
        std::transform(funcNameLower.begin(), funcNameLower.end(), funcNameLower.begin(), ::tolower);

        std::string indexExpr = m[2];
        std::string propName = m[3];
        std::string propNameLower = propName;
        std::transform(propNameLower.begin(), propNameLower.end(), propNameLower.begin(), ::tolower);

        // Only transform if:
        // 1. Function name is NOT a VBScript built-in
        // 2. Function name is NOT already a VPX_ function
        // 3. Property name IS a known VPX property
        // 4. Index expression does NOT contain '(' (nested function calls)
        if (excludedFunctions.count(funcNameLower) > 0 ||
            funcNameLower.substr(0, 4) == "vpx_" ||
            vpxProperties.count(propNameLower) == 0 ||
            indexExpr.find('(') != std::string::npos) {
            // Keep original
            return m.full_match;
        } else {
            // Transform to VPX_GetArrObjProp
            return "(VPX_GetArrObjProp(" + funcName + ", " + indexExpr + ", \"" + propName + "\"))";
        }
    });

    return r;
}


std::string ScriptPatcher::PatchSingleLineIfElse(const std::string& script) {
    std::string r = script;
    // Use [ \t]+ instead of \s+ to avoid matching across newlines
    static const RE2 p(R"((?im)(Then[ \t]+[^'\r\n]+[ \t]+Else)[ \t]*('.*)?$)");
    r = RE2Replace(r, p, "\\1:\\2");
    return r;
}


std::string ScriptPatcher::PatchNestedSingleLineIf(const std::string& script) {
    std::string r = script;
    // Wine VBScript doesn't support nested single-line If:
    //   if COND1 then if COND2 then STATEMENT end if end if
    // Convert to:
    //   if COND1 then
    //       if COND2 then STATEMENT
    //   end if
    // Use [ \t] instead of \s to prevent matching across lines (newlines)
    static const RE2 p(R"((?i)(^|[\r\n])([ \t]*)(if[ \t]+[^\r\n]+?[ \t]+then)[ \t]+(if[ \t]+[^\r\n]+?[ \t]+then[ \t]+[^\r\n]+?)[ \t]+end[ \t]+if[ \t]+end[ \t]+if)");
    r = RE2Replace(r, p, "\\1\\2\\3\n\\2    \\4\n\\2end if");
    return r;
}




// Fix invalid single-line If...End If syntax
// VBScript single-line If should NOT have End If:
//   INVALID: If cond then stmt end if
//   VALID:   If cond then stmt
// Wine VBScript may crash or infinite loop on this invalid syntax
std::string ScriptPatcher::PatchSingleLineIfEndIf(const std::string& script) {
    std::string r = script;

    // Match: If ... then <statement> end if  (on single line, NO colons)
    // Example: "If GifCountr > 3 then GifCountr = 0 end If"
    // Example: "If(x <> "")Then y = 1 End If"  (no spaces around keywords)
    // Should become: "If GifCountr > 3 then GifCountr = 0"
    // Use \b word boundary to handle cases without spaces (e.g., "If(x)Then")
    // Also handle quoted strings in statements using (?:[^:\r\n"]|"[^"]*")*
    static const RE2 p(R"((?i)(if\b(?:[^:\r\n"]|"[^"]*")*?\bthen)\b((?:[^:\r\n"]|"[^"]*")+?)[ \t]+end[ \t]+if)");
    std::string before;
    int iterations = 0;
    const int maxIterations = 100;

    // Keep applying until no more matches (handles nested cases)
    while (r != before && iterations < maxIterations) {
        before = r;
        r = RE2Replace(r, p, "\\1 \\2");
        iterations++;
    }

    if (iterations > 1) {
        PLOGI.printf("ScriptPatcher: Fixed %d single-line If...End If patterns", iterations - 1);
    }

    return r;
}

std::string ScriptPatcher::PatchExecuteEval(const std::string& script) { return script; }

std::string ScriptPatcher::PatchStringConcatenation(const std::string& script) {
    std::string r = script;
    static const RE2 p(R"((?i)\(\s*(\([^)]+\))\s*&\s*")");
    r = RE2Replace(r, p, R"(("" & \1 & ")");
    return r;
}


// ============================================================================
// GAME-SPECIFIC SYNTAX FIXES
// ============================================================================

// Fix missing comma in DMDSettings_Setup calls (Game of Thrones table)
// INVALID:  DMDSettings_Setup DMDMenu(0) "STRING", ...
// VALID:    DMDSettings_Setup DMDMenu(0), "STRING", ...
// The issue is a missing comma after the DMDMenu(x) argument
std::string ScriptPatcher::PatchDMDSettingsSetupMissingComma(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Check if the pattern exists in the script first
    bool hasPattern = script.find("DMDSettings_Setup") != std::string::npos;
    PLOGI.printf("ScriptPatcher: PatchDMDSettingsSetupMissingComma called, hasPattern=%d", hasPattern);

    // Pattern: DMDSettings_Setup followed by DMDMenu(index) then immediately a string literal
    // without a comma in between. Add the missing comma.
    // Use simple string replacement instead of regex for reliability

    // Find and replace pattern: DMDSettings_Setup DMDMenu(N) " -> DMDSettings_Setup DMDMenu(N), "
    size_t pos = 0;
    while ((pos = r.find("DMDSettings_Setup DMDMenu(", pos)) != std::string::npos) {
        // Find the closing paren after the index
        size_t parenStart = pos + strlen("DMDSettings_Setup DMDMenu(");
        size_t parenEnd = r.find(')', parenStart);
        if (parenEnd != std::string::npos) {
            // Check if next non-whitespace char is a quote (missing comma case)
            size_t afterParen = parenEnd + 1;
            while (afterParen < r.length() && (r[afterParen] == ' ' || r[afterParen] == '\t')) {
                afterParen++;
            }
            if (afterParen < r.length() && r[afterParen] == '"') {
                // Insert comma after the closing paren
                r.insert(parenEnd + 1, ",");
                count++;
                PLOGI.printf("ScriptPatcher: Fixed DMDSettings_Setup at pos %zu", pos);
            }
        }
        pos = parenEnd + 1;
    }

    PLOGI.printf("ScriptPatcher: PatchDMDSettingsSetupMissingComma found %d issues", count);
    return r;
}


bool ScriptPatcher::UsesSlingshotCorrection(const std::string& script) {
    static const RE2 p(R"((?i)Class\s+(SlingshotCorrection|FlipperPolarity|FlipperPhysics|BumperPhysics))");
    return RE2Search(script, p);
}


std::string ScriptPatcher::PatchSlingshotCorrection(const std::string& script) {
    // Legacy - now handled by EmulateClasses
    return script;
}


// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

#endif // __STANDALONE__

// ============================================================================
// WINE BUG WORKAROUND: Remove unused class definitions
// Wine VBScript has bugs with certain class patterns. If a class is defined
// but never instantiated, we can safely comment it out to avoid triggering bugs.
// ============================================================================

std::string ScriptPatcher::RemoveUnusedClasses(const std::string& script) {
    std::string result = script;

    // Find all class definitions
    static const RE2 classStartRegex(R"((?im)^[ \t]*Class\s+(\w+))");
    static const RE2 classEndRegex(R"((?im)^[ \t]*End\s+Class)");

    std::vector<std::tuple<std::string, size_t, size_t>> unusedClasses;

    for (RE2MatchIterator it(result, classStartRegex); !it.done(); it.next()) {
        const RE2Match& m = it.current();
        std::string className = m.groups.size() > 0 ? m.groups[0] : "";
        size_t classStart = m.position;
        std::string afterClass = result.substr(classStart);
        RE2Match endMatch;
        if (RE2FindFirst(afterClass, classEndRegex, endMatch)) {
            size_t classEnd = classStart + endMatch.position + endMatch.length;

            // Check if this class is ever instantiated (New ClassName)
            RE2 newPattern("(?i)\\bNew\\s+" + EscapeRegex(className) + "\\b");
            if (!RE2Search(result, newPattern)) {
                unusedClasses.push_back({className, classStart, classEnd});
                PLOGI.printf("ScriptPatcher: Found unused class '%s' - will be REMOVED", className.c_str());
            }
        }
    }

    // Sort by position descending to avoid offset issues when modifying
    std::sort(unusedClasses.begin(), unusedClasses.end(),
              [](const auto& a, const auto& b) { return std::get<1>(a) > std::get<1>(b); });

    // Completely REMOVE unused classes (Wine crashes even on commented classes)
    for (const auto& [className, startPos, endPos] : unusedClasses) {
        // Full removal - replace entire class with single comment
        result = result.substr(0, startPos) + "' [WINE: Removed " + className + "]\n" + result.substr(endPos);
        PLOGI.printf("ScriptPatcher: REMOVED class '%s'", className.c_str());
    }

    return result;
}

// ============================================================================
// WINE BUG WORKAROUND: Remove duplicate vpmInit calls
// Some table scripts mistakenly call vpmInit Me twice, which corrupts flipper
// callback state. Keep only the first occurrence.
// ============================================================================

std::string ScriptPatcher::RemoveDuplicateVpmInit(const std::string& script) {
    std::string result = script;

    // Find all vpmInit calls (case insensitive)
    static const RE2 vpmInitRegex(R"((?i)\bvpmInit\s+[Mm]e\b)");

    auto matches = RE2FindAll(result, vpmInitRegex);

    if (matches.size() > 1) {
        PLOGI.printf("ScriptPatcher: Found %zu vpmInit calls - removing duplicates", matches.size());

        // Remove all but the first occurrence (process in reverse order)
        for (size_t i = matches.size() - 1; i > 0; --i) {
            size_t pos = matches[i].position;
            size_t len = matches[i].length;
            // Comment out the duplicate instead of removing to preserve line numbers
            result = result.substr(0, pos) + "' [WINE: Removed duplicate] " + result.substr(pos, len) + result.substr(pos + len);
            PLOGI.printf("ScriptPatcher: Commented out duplicate vpmInit at position %zu", pos);
        }
    }

    return result;
}

// ============================================================================
// WINE BUG WORKAROUND: Fix single-line If...Then...Else...End If syntax
// Wine VBScript is strict about single-line If syntax - End If is invalid
// for single-line If statements. Windows VBScript is more lenient.
// Pattern: If x Then y Else z End If -> If x Then y Else z
// ============================================================================

std::string ScriptPatcher::FixSingleLineIfEndIf(const std::string& script) {
    std::string result = script;

    // Wine VBScript is strict about single-line If syntax:
    // - Single-line If statements should NOT have "End If"
    // - Multi-line If statements MUST have "End If"
    //
    // Windows VBScript is lenient and accepts "End If" on single-line If.
    // We fix this by removing "End If" from single-line If statements.
    //
    // IMPORTANT: Use [ \t]+ instead of \s+ before "End If" to ensure we only
    // match End If on the SAME line. Using \s+ would match across newlines and
    // incorrectly remove End If that belongs to an outer multi-line If block.
    //
    // IMPORTANT: Use (?:[^:\r\n"]|"[^"]*")* to match content that may contain
    // colons INSIDE string literals. Plain [^:] would fail on:
    //   MsgBox "Error: something" End If
    // because it stops at the colon inside the string.

    // Helper pattern for content that may contain colons in strings:
    // (?:[^:\r\n"]|"[^"]*")* matches non-colon/newline/quote chars OR quoted strings

    // Pattern 1: If ... Then ... Else ... End If (with Else, same line)
    // Example: If x > 0 Then y = 1 Else y = 0 End If
    static const RE2 singleLineIfElseEndIf(R"((?i)(If\b(?:[^:\r\n"]|"[^"]*")*?\bThen\b(?:[^:\r\n"]|"[^"]*")*?\bElse\b(?:[^:\r\n"]|"[^"]*")+?)[ \t]+End[ \t]+If)");

    auto matches = RE2FindAll(result, singleLineIfElseEndIf);
    if (!matches.empty()) {
        PLOGI.printf("ScriptPatcher: Found %zu single-line If...Then...Else...End If patterns to fix", matches.size());
        for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
            if (it->groups.size() > 0) {
                result = result.substr(0, it->position) + it->groups[0] + result.substr(it->position + it->length);
                PLOGI.printf("ScriptPatcher: Fixed single-line If...Else...End If at position %zu", it->position);
            }
        }
    }

    // Pattern 2: If ... Then ... End If (without Else, same line)
    // Example: If TypeName(x) <> "String" Then MsgBox "error: details" End If
    // Must run AFTER Pattern 1 so Else cases are already handled
    static const RE2 singleLineIfThenEndIf(R"((?i)(If\b(?:[^:\r\n"]|"[^"]*")*?\bThen\b(?:[^:\r\n"]|"[^"]*")+?)[ \t]+End[ \t]+If)");

    matches = RE2FindAll(result, singleLineIfThenEndIf);
    if (!matches.empty()) {
        PLOGI.printf("ScriptPatcher: Found %zu single-line If...Then...End If patterns to fix", matches.size());
        for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
            if (it->groups.size() > 0) {
                result = result.substr(0, it->position) + it->groups[0] + result.substr(it->position + it->length);
                PLOGI.printf("ScriptPatcher: Fixed single-line If...Then...End If at position %zu", it->position);
            }
        }
    }

    return result;
}

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
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <regex>
#include <sstream>
#include <algorithm>

bool ScriptPatcher::UsesDTArray(const std::string& script) {
    std::regex p(R"(DTArray\s*\(\s*\w+\s*\)\s*\(\s*\d+\s*\))", std::regex::icase);
    return std::regex_search(script, p);
}


bool ScriptPatcher::UsesSTArray(const std::string& script) {
    std::regex p(R"(STArray\s*\(\s*\w+\s*\)\s*\(\s*\d+\s*\))", std::regex::icase);
    return std::regex_search(script, p);
}


std::string ScriptPatcher::InjectDropTargetClass(const std::string& script) {
    std::regex existing(R"(Class\s+DropTarget)", std::regex::icase);
    if (std::regex_search(script, existing)) return script;
    std::regex firstDef(R"((\r?\n)([ \t]*)(DT\d+\s*=\s*Array\s*\())", std::regex::icase);
    std::smatch m;
    if (std::regex_search(script, m, firstDef))
        return script.substr(0, m.position()) + "\n" + DROP_TARGET_CLASS + script.substr(m.position());
    std::regex optExp(R"(^\s*Option\s+Explicit\s*$)", std::regex::icase | std::regex::multiline);
    if (std::regex_search(script, m, optExp))
        return script.substr(0, m.position() + m.length()) + "\n" + DROP_TARGET_CLASS + script.substr(m.position() + m.length());
    return std::string(DROP_TARGET_CLASS) + script;
}


std::string ScriptPatcher::InjectStandupTargetClass(const std::string& script) {
    std::regex existing(R"(Class\s+StandupTarget)", std::regex::icase);
    if (std::regex_search(script, existing)) return script;
    std::regex firstDef(R"((\r?\n)([ \t]*)(ST\d+\s*=\s*Array\s*\())", std::regex::icase);
    std::smatch m;
    if (std::regex_search(script, m, firstDef))
        return script.substr(0, m.position()) + "\n" + STANDUP_TARGET_CLASS + script.substr(m.position());
    std::regex optExp(R"(^\s*Option\s+Explicit\s*$)", std::regex::icase | std::regex::multiline);
    if (std::regex_search(script, m, optExp))
        return script.substr(0, m.position() + m.length()) + "\n" + STANDUP_TARGET_CLASS + script.substr(m.position() + m.length());
    return std::string(STANDUP_TARGET_CLASS) + script;
}


std::string ScriptPatcher::PatchDTArrayDefinitions(const std::string& script) {
    std::string r = script;
    // Match DT followed by digits and optional suffix letters (DT1, DT18, DT18a, DT18b, etc.)
    // 5 args: primary, secondary, prim, sw, animate (default isDropped=false)
    std::regex p5(R"(\b(DT\d+\w*)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p5, "Set $1 = DropTarget_Create($2, $3, $4, $5, $6, false)");
    // 6 args: primary, secondary, prim, sw, animate, isDropped
    std::regex p6(R"(\b(DT\d+\w*)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p6, "Set $1 = DropTarget_Create($2, $3, $4, $5, $6, $7)");
    return r;
}


std::string ScriptPatcher::PatchSTArrayDefinitions(const std::string& script) {
    std::string r = script;
    // Match ST followed by digits and optional suffix letters (ST1, ST18, ST18a, ST18b, etc.)
    // 5 args: primary, prim, sw, animate, target
    std::regex p(R"(\b(ST\d+\w*)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p, "Set $1 = StandupTarget_Create($2, $3, $4, $5, $6)");
    return r;
}


std::string ScriptPatcher::PatchDTArrayAccess(const std::string& script) {
    std::string r = script;
    // Wine VBScript can't handle chained indexing like DTArray(i)("key")
    // Use DTGet/DTGetObj/DTSet helper functions instead

    // WRITE patterns: DTArray(i)(idx) = value -> DTSet DTArray, i, "prop", value
    // Must handle writes FIRST before reads to avoid partial transformations
    std::regex dw4(R"((^[ \t]*|:[ \t]*)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\)\s*=\s*([^\r\n:]+))", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, dw4, "$1DTSet DTArray, $2, \"animate\", $3");
    std::regex dw5(R"((^[ \t]*|:[ \t]*)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*5\s*\)\s*=\s*([^\r\n:]+))", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, dw5, "$1DTSet DTArray, $2, \"isDropped\", $3");
    std::regex dw3(R"((^[ \t]*|:[ \t]*)DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\)\s*=\s*([^\r\n:]+))", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, dw3, "$1DTSet DTArray, $2, \"sw\", $3");

    // READ patterns for object properties: DTArray(i)(0/1/2) -> DTGetObj(DTArray, i, "prop")
    std::regex d0(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*0\s*\))", std::regex::icase);
    r = std::regex_replace(r, d0, "DTGetObj(DTArray, $1, \"primary\")");
    std::regex d1(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*1\s*\))", std::regex::icase);
    r = std::regex_replace(r, d1, "DTGetObj(DTArray, $1, \"secondary\")");
    std::regex d2(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\))", std::regex::icase);
    r = std::regex_replace(r, d2, "DTGetObj(DTArray, $1, \"prim\")");

    // READ patterns for value properties: DTArray(i)(3/4/5) -> DTGet(DTArray, i, "prop")
    std::regex d3(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\))", std::regex::icase);
    r = std::regex_replace(r, d3, "DTGet(DTArray, $1, \"sw\")");
    std::regex d4(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\))", std::regex::icase);
    r = std::regex_replace(r, d4, "DTGet(DTArray, $1, \"animate\")");
    std::regex d5(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*5\s*\))", std::regex::icase);
    r = std::regex_replace(r, d5, "DTGet(DTArray, $1, \"isDropped\")");

    // Dot notation WRITE: DTArray(i).animate = value -> DTSet DTArray, i, "animate", value
    std::regex dpw(R"((^[ \t]*|:[ \t]*)DTArray\s*\(\s*(\w+)\s*\)\s*\.\s*(sw|animate|isDropped)\s*=\s*([^\r\n:]+))", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, dpw, "$1DTSet DTArray, $2, \"$3\", $4");

    // Dot notation READ for objects: DTArray(i).primary -> DTGetObj(DTArray, i, "primary")
    std::regex dpo(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\.\s*(primary|secondary|prim)\b)", std::regex::icase);
    r = std::regex_replace(r, dpo, "DTGetObj(DTArray, $1, \"$2\")");

    // Dot notation READ for values: DTArray(i).sw -> DTGet(DTArray, i, "sw")
    std::regex dpv(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\.\s*(sw|animate|isDropped)\b)", std::regex::icase);
    r = std::regex_replace(r, dpv, "DTGet(DTArray, $1, \"$2\")");

    return r;
}


std::string ScriptPatcher::PatchSTArrayAccess(const std::string& script) {
    std::string r = script;
    // Wine VBScript can't handle chained indexing like STArray(i)("key")
    // Use STGet/STGetObj/STSet helper functions instead

    // WRITE patterns: STArray(i)(idx) = value -> STSet STArray, i, "prop", value
    // Must handle writes FIRST before reads to avoid partial transformations
    std::regex sw2(R"((^[ \t]*|:[ \t]*)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\)\s*=\s*([^\r\n:]+))", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, sw2, "$1STSet STArray, $2, \"sw\", $3");
    std::regex sw3(R"((^[ \t]*|:[ \t]*)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\)\s*=\s*([^\r\n:]+))", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, sw3, "$1STSet STArray, $2, \"animate\", $3");
    std::regex sw4(R"((^[ \t]*|:[ \t]*)STArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\)\s*=\s*([^\r\n:]+))", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, sw4, "$1STSet STArray, $2, \"target\", $3");

    // READ patterns for object properties: STArray(i)(0/1) -> STGetObj(STArray, i, "prop")
    std::regex s0(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*0\s*\))", std::regex::icase);
    r = std::regex_replace(r, s0, "STGetObj(STArray, $1, \"primary\")");
    std::regex s1(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*1\s*\))", std::regex::icase);
    r = std::regex_replace(r, s1, "STGetObj(STArray, $1, \"prim\")");

    // READ patterns for value properties: STArray(i)(2/3/4) -> STGet(STArray, i, "prop")
    std::regex s2(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\))", std::regex::icase);
    r = std::regex_replace(r, s2, "STGet(STArray, $1, \"sw\")");
    std::regex s3(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\))", std::regex::icase);
    r = std::regex_replace(r, s3, "STGet(STArray, $1, \"animate\")");
    std::regex s4(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\))", std::regex::icase);
    r = std::regex_replace(r, s4, "STGet(STArray, $1, \"target\")");

    // Dot notation WRITE: STArray(i).animate = value -> STSet STArray, i, "animate", value
    std::regex spw(R"((^[ \t]*|:[ \t]*)STArray\s*\(\s*(\w+)\s*\)\s*\.\s*(sw|animate|target)\s*=\s*([^\r\n:]+))", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, spw, "$1STSet STArray, $2, \"$3\", $4");

    // Dot notation READ for objects: STArray(i).primary -> STGetObj(STArray, i, "primary")
    std::regex spo(R"(STArray\s*\(\s*(\w+)\s*\)\s*\.\s*(primary|prim)\b)", std::regex::icase);
    r = std::regex_replace(r, spo, "STGetObj(STArray, $1, \"$2\")");

    // Dot notation READ for values: STArray(i).sw -> STGet(STArray, i, "sw")
    std::regex spv(R"(STArray\s*\(\s*(\w+)\s*\)\s*\.\s*(sw|animate|target)\b)", std::regex::icase);
    r = std::regex_replace(r, spv, "STGet(STArray, $1, \"$2\")");

    return r;
}


bool ScriptPatcher::UsesControllerPause(const std::string& script) {
    std::regex p(R"(Controller\.Pause\s*=)", std::regex::icase);
    return std::regex_search(script, p);
}


std::string ScriptPatcher::PatchControllerPause(const std::string& script) {
    std::string r = script;

    // First handle colon-separated statements (e.g., Sub Foo:Controller.Pause = True:End Sub)
    // These can't be safely commented out, so remove them entirely
    // Pattern matches :Controller.Pause = Value followed by :
    std::regex p1(R"(:[ 	]*Controller\.Pause\s*=\s*(True|False)[ 	]*:)", std::regex::icase);
    r = std::regex_replace(r, p1, ":");

    // Then handle statements on their own lines (comment them out)
    std::regex p2(R"((\s*)(Controller\.Pause\s*=\s*(True|False)))", std::regex::icase);
    r = std::regex_replace(r, p2, "$1' $2 ' Wine/Android");

    return r;
}


bool ScriptPatcher::UsesPuPlayerPlaystopInPlayclear(const std::string& script) {
    std::regex p(R"(if\s+chan\s*=\s*pBackglass\s+Then[\s\S]*?PuPlayer\.playstop\s+pDMD)", std::regex::icase);
    return std::regex_search(script, p);
}


std::string ScriptPatcher::PatchPuPlayerPlaystopInPlayclear(const std::string& script) {
    std::string r = script;
    std::regex p(R"((if\s+chan\s*=\s*pBackglass\s+Then\s*[\r\n]+)([ \t]*)(PuPlayer\.playstop\s+pDMD))", std::regex::icase);
    r = std::regex_replace(r, p, "$1$2' $3 ' Android");
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
    std::regex p(R"(AddScore\s+\(([^)]+\([^)]*\)[^)]*)\)\s*\+\s*(\w+\([^)]*\)))", std::regex::icase);
    r = std::regex_replace(r, p, "AddScore (($1)+$2)");
    return r;
}


std::string ScriptPatcher::PatchSetAlignedPositionParentheses(const std::string& script) {
    std::string r = script;
    std::regex p(R"(\.SetAlignedPosition\s+\(\(([^)]+)\)\*(\d+)\)\+(\d+)\s*,)", std::regex::icase);
    r = std::regex_replace(r, p, ".SetAlignedPosition ((($1)*$2)+$3),");
    return r;
}


std::string ScriptPatcher::PatchLineContinuationBeforeDot(const std::string& script) {
    std::string r = script;
    std::regex p(R"((\w|\))\s+_\s*\r?\n\s*\.)", std::regex::icase);
    r = std::regex_replace(r, p, "$1. _\n");
    return r;
}



// Wine VBScript Array Compatibility Helpers
// Wine's VBScript has issues with UBound on uninitialized arrays and 2D array access

bool ScriptPatcher::UsesProblematicArrays(const std::string& script) {
    // Check for patterns that cause Wine VBScript issues:
    // 1. UBound in If conditions before ReDim
    // 2. 2D array access
    std::regex p1(R"(If\s+UBound\s*\()", std::regex::icase);
    std::regex p2(R"(\w+\s*\(\s*\d+\s*,\s*\d+\s*\))", std::regex::icase);
    return std::regex_search(script, p1) || std::regex_search(script, p2);
}


std::string ScriptPatcher::InjectWineArrayHelpers(const std::string& script) {
    // Inject helper functions at the start of the script (after Option Explicit if present)
    // Check if Atn2 is USED in the script (called as a function)
    bool usesAtn2 = std::regex_search(script, std::regex(R"(\bAtn2\s*\()", std::regex::icase));
    // Check if Atn2 is properly DEFINED (after newline + optional whitespace, not in comment)
    // C++ regex doesn't support multiline ^, so we check for \n or start of string
    bool hasAtn2 = std::regex_search(script, std::regex(R"((^|\n)\s*Function\s+Atn2\b)", std::regex::icase));

    std::string helpers = R"(
' Wine VBScript Array Compatibility Helpers (Auto-injected by ScriptPatcher)
Function VPX_SafeUBound(arr)
    On Error Resume Next
    VPX_SafeUBound = -1
    VPX_SafeUBound = UBound(arr)
    On Error Goto 0
End Function

' Safe array element access - returns Empty if index out of bounds
Function VPX_SafeGet(arr, idx)
    On Error Resume Next
    VPX_SafeGet = Empty
    If idx >= 0 Then VPX_SafeGet = arr(idx)
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
    On Error Goto 0
End Function

Function VPX_SafeArray2DGet(arr, idx1, idx2)
    On Error Resume Next
    VPX_SafeArray2DGet = Empty
    VPX_SafeArray2DGet = arr(idx1, idx2)
    On Error Goto 0
End Function

Sub VPX_SafeArraySet(arr, idx, val)
    On Error Resume Next
    arr(idx) = val
    On Error Goto 0
End Sub

Sub VPX_SafeReDim(ByRef arr, newSize)
    On Error Resume Next
    ReDim Preserve arr(newSize)
    On Error Goto 0
End Sub

Function VPX_ArrObj(arr, idx)
    On Error Resume Next
    Set VPX_ArrObj = arr(idx)
    On Error Goto 0
End Function

' Helper for chained Dictionary/array assignment: dict("key")(idx) = val
Sub VPX_SetDictArrItem(dict, key, idx, val)
    On Error Resume Next
    Dim arr : arr = dict(key)
    arr(idx) = val
    dict(key) = arr
    On Error Goto 0
End Sub

' Helper for chained Dictionary/array read: dict("key")(idx)
Function VPX_GetDictArrItem(dict, key, idx)
    On Error Resume Next
    VPX_GetDictArrItem = Empty
    Dim arr : arr = dict(key)
    VPX_GetDictArrItem = arr(idx)
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

    helpers += "' End Wine VBScript Array Compatibility Helpers\n\n";

    std::string r = script;
    
    // Find insertion point - after Option Explicit or at start
    std::regex optionExplicit(R"((Option\s+Explicit[^\r\n]*[\r\n]+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(r, match, optionExplicit)) {
        r = match.prefix().str() + match[0].str() + helpers + match.suffix().str();
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
    On Error Goto 0
End Sub
)";

    std::string r = script;

    // Find insertion point - after Option Explicit or at start
    std::regex optionExplicit(R"((Option\s+Explicit[^\r\n]*[\r\n]+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(r, match, optionExplicit)) {
        r = match.prefix().str() + match[0].str() + helper + match.suffix().str();
    } else {
        // Insert at the very beginning
        r = helper + r;
    }

    return r;
}


std::string ScriptPatcher::PatchUBoundInConditions(const std::string& script) {
    std::string r = script;

    // Replace "If UBound(array)" with "If VPX_SafeUBound(array)"
    //// Pattern: If UBound(varname) followed by comparison
    std::regex p(R"(If\s+UBound\s*\(\s*(\w+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p, "If VPX_SafeUBound($1)");

    return r;
}


std::string ScriptPatcher::PatchUBoundInForLoops(const std::string& script) {
    std::string r = script;

    // Replace "For x = ... to UBound(array)" with "For x = ... to VPX_SafeUBound(array)"
    // This handles uninitialized arrays that would throw on UBound()
    std::regex p(R"(\bto\s+UBound\s*\(\s*(\w+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p, "to VPX_SafeUBound($1)");

    return r;
}

std::string ScriptPatcher::PatchAllUBound(const std::string& script) {
    std::string r = script;
    // Replace ALL remaining uBound( calls with VPX_SafeUBound(
    std::regex p(R"(\buBound\s*\()", std::regex::icase);
    r = std::regex_replace(r, p, "VPX_SafeUBound(");
    return r;
}

std::string ScriptPatcher::PatchSafeUBoundArrayAccess(const std::string& script) {
    std::string r = script;
    // Replace arr(VPX_SafeUBound(arr)) with VPX_SafeGet(arr, VPX_SafeUBound(arr))
    // BUT only when NOT followed by = (i.e., when reading, not writing)
    // This prevents out-of-bounds when VPX_SafeUBound returns -1 for empty arrays
    std::regex p(R"((\w+)\s*\(\s*VPX_SafeUBound\s*\(\s*\1\s*\)\s*\)(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, p, "VPX_SafeGet($1, VPX_SafeUBound($1))");
    return r;
}

std::string ScriptPatcher::PatchLinearEnvelopeGuard(const std::string& script) {
    std::string r = script;
    // Add early-exit guard to LinearEnvelope function for empty arrays
    //// Pattern: Function LinearEnvelope(xInput, xKeyFrame, yLvl) followed by dim y
    std::regex p(R"((Function\s+LinearEnvelope\s*\([^)]+\)\s*\r?\n)(\s*dim\s+y\b))", std::regex::icase);
    r = std::regex_replace(r, p, "$1    If VPX_SafeUBound(xKeyFrame) < 0 Then LinearEnvelope = 0 : Exit Function\n$2");
    return r;
}

std::string ScriptPatcher::PatchBallArrayAccess(const std::string& script) {
    std::string r = script;
    
    // Ball arrays: gBOT (GetBallsOfTable) and BOT
    // Properties: X, Y, Z, VelX, VelY, VelZ, ID, Color
    
    // gBOT array
    std::regex gbot_x(R"(\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*X\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, gbot_x, "VPX_BallX(gBOT($1))");
    
    std::regex gbot_y(R"(\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Y\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, gbot_y, "VPX_BallY(gBOT($1))");
    
    std::regex gbot_z(R"(\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Z\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, gbot_z, "VPX_BallZ(gBOT($1))");
    
    std::regex gbot_velx(R"(\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelX\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, gbot_velx, "VPX_BallVelX(gBOT($1))");
    
    std::regex gbot_vely(R"(\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelY\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, gbot_vely, "VPX_BallVelY(gBOT($1))");
    
    std::regex gbot_velz(R"(\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelZ\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, gbot_velz, "VPX_BallVelZ(gBOT($1))");
    
    std::regex gbot_id(R"(\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*ID\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, gbot_id, "VPX_BallID(gBOT($1))");
    
    std::regex gbot_color(R"(\bgBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Color\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, gbot_color, "VPX_BallColor(gBOT($1))");
    
    // BOT array (same patterns)
    std::regex bot_x(R"(\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*X\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, bot_x, "VPX_BallX(BOT($1))");
    
    std::regex bot_y(R"(\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Y\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, bot_y, "VPX_BallY(BOT($1))");
    
    std::regex bot_z(R"(\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Z\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, bot_z, "VPX_BallZ(BOT($1))");
    
    std::regex bot_velx(R"(\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelX\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, bot_velx, "VPX_BallVelX(BOT($1))");
    
    std::regex bot_vely(R"(\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelY\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, bot_vely, "VPX_BallVelY(BOT($1))");
    
    std::regex bot_velz(R"(\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*VelZ\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, bot_velz, "VPX_BallVelZ(BOT($1))");
    
    std::regex bot_id(R"(\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*ID\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, bot_id, "VPX_BallID(BOT($1))");
    
    std::regex bot_color(R"(\bBOT\s*\(\s*(\w+)\s*\)\s*\.\s*Color\b(?!\s*=))", std::regex::icase);
    r = std::regex_replace(r, bot_color, "VPX_BallColor(BOT($1))");
    
    return r;
}

std::string ScriptPatcher::PatchBallLoopGuard(const std::string& script) {
    std::string r = script;
    
    // Add guard to For loops over gBOT: skip if ball is invalid
    // Pattern: For b = 0 to VPX_SafeUBound(gBOT) followed by newline
    // Insert: If Not VPX_IsValidBall(gBOT(b)) Then [continue logic]
    
    // This is complex - instead, let's wrap entire loop bodies in On Error Resume Next
    // by adding it after For statements that iterate over gBOT
    std::regex p(R"((For\s+(\w+)\s*=\s*\d+\s+to\s+VPX_SafeUBound\s*\(\s*gBOT\s*\)\s*\r?\n))", std::regex::icase);
    r = std::regex_replace(r, p, "$1        On Error Resume Next\n");
    
    // Same for BOT
    std::regex p2(R"((For\s+(\w+)\s*=\s*\d+\s+to\s+VPX_SafeUBound\s*\(\s*BOT\s*\)\s*\r?\n))", std::regex::icase);
    r = std::regex_replace(r, p2, "$1        On Error Resume Next\n");
    
    return r;
}


std::string ScriptPatcher::PatchReDimWithUBound(const std::string& script) {
    std::string r = script;
    
    //// Pattern: If UBound(arr) < val Then ReDim arr(val)
    // This pattern is problematic because UBound fails on uninitialized arrays
    // We've already patched UBound -> VPX_SafeUBound, so the If check should work
    // But we also need to ensure the arrays are initialized
    
    // For now, the SafeUBound should handle most cases
    return r;
}


std::string ScriptPatcher::Patch2DArrayAccess(const std::string& script) {
    std::string r = script;
    
    //// Pattern: if not ArrayName(0,0) then ... 
    // This is problematic in Wine VBScript - wrap in error handling by commenting out
    // or replacing with a safe version
    
    // For "if not ArrayName(num,num)" patterns, we need to be careful
    // Let's wrap specific known problematic patterns
    
    // RampBalls(0,0) pattern - this is checking if array element is falsy
    // Replace with a try-catch style approach or just default to a safe value
    std::regex p(R"(if\s+not\s+(\w+)\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)\s+then)", std::regex::icase);
    r = std::regex_replace(r, p, "If Not VPX_SafeArray2DGet($1, $2, $3) Then");
    
    return r;
}


std::string ScriptPatcher::PatchArrayElementAssignment(const std::string& script) {
    std::string r = script;

    //// Pattern: arrayName(index) = value where array might not be initialized
    // Wine VBScript has issues with array element assignments in certain contexts
    //
    // IMPORTANT: Only match ASSIGNMENTS at statement start, not comparisons in If conditions!
    // Statement start positions: line start, after :, after Then, after Else
    //
    // Example that must NOT be transformed (comparison in If condition):
    //   If rolling(b) = True Then ...
    // Example that SHOULD be transformed (assignment):
    //   rolling(b) = True
    //
    // Use inline error handling to avoid needing helper function at runtime:
    //   rolling(b) = False -> On Error Resume Next : rolling(b) = False : On Error Goto 0

    // For known problematic arrays with True/False values - only at statement start
    // Use inline error handling instead of VPX_SafeArraySet
    std::regex p1(R"((^[ \t]*)(bBallInTrough|rolling)\s*\(\s*(\w+)\s*\)\s*=\s*(True|False))",
                  std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, p1, "$1On Error Resume Next : $2($3) = $4 : On Error Goto 0");

    // After Then/Else - use colon-separated inline
    std::regex p2(R"((\bThen[ \t]+|\bElse[ \t]+)(bBallInTrough|rolling)\s*\(\s*(\w+)\s*\)\s*=\s*(True|False))",
                  std::regex::icase);
    r = std::regex_replace(r, p2, "$1On Error Resume Next : $2($3) = $4 : On Error Goto 0");

    // NOTE: ballvel/ballvelx/ballvely are now handled by class emulation
    // (they become CoRTracker_ballvel etc. as global arrays)
    return r;
}


std::string ScriptPatcher::PatchDictArrayAccess(const std::string& script) {
    std::string r = script;

    // Wine VBScript doesn't support chained Dictionary/array access: dict("key")(idx)
    // This pattern appears in emulated classes where properties are arrays
    // Transform: varName("key")(idx) = value
    // To: VPX_SetDictArrItem varName, "key", idx, value

    // Pattern for assignment: dict("key")(idx) = value (at statement start)
    // Note: C++ regex ^ only matches start of STRING, not line. Use (^|\n) instead.
    // Use custom raw string delimiter to avoid issues with quotes inside
    std::regex assignPattern(R"REGEX((^|\n)([ \t]*)(\w+)\s*\(\s*"([^"]+)"\s*\)\s*\(\s*([^)]+)\s*\)\s*=\s*([^\r\n:]+))REGEX",
                             std::regex::icase);
    r = std::regex_replace(r, assignPattern, "$1$2VPX_SetDictArrItem $3, \"$4\", $5, $6");

    // Also handle after colon (statement separator)
    std::regex colonPattern(R"REGEX((:[ \t]*)(\w+)\s*\(\s*"([^"]+)"\s*\)\s*\(\s*([^)]+)\s*\)\s*=\s*([^\r\n:]+))REGEX",
                             std::regex::icase);
    r = std::regex_replace(r, colonPattern, "$1VPX_SetDictArrItem $2, \"$3\", $4, $5");

    // Transform READ access: dict("key")(idx) -> VPX_GetDictArrItem(dict, "key", idx)
    // This handles cases like: LinearEnvelope(cor("ballvel")(aBall.id), ...)
    // Need to be careful not to match patterns already transformed to VPX_SetDictArrItem
    //// Pattern: word("string")(expr) but NOT preceded by VPX_SetDictArrItem
    std::regex readPattern(R"REGEX((\b(?!VPX_SetDictArrItem\b))(\w+)\s*\(\s*"([^"]+)"\s*\)\s*\(\s*([^)]+)\s*\))REGEX",
                           std::regex::icase);
    r = std::regex_replace(r, readPattern, "$1VPX_GetDictArrItem($2, \"$3\", $4)");

    PLOGI.printf("ScriptPatcher: Applied Dictionary/array access patch");
    return r;
}


std::string ScriptPatcher::PatchArrayObjectPropertyAccess(const std::string& script) {
    std::string r = script;

    // Wine VBScript doesn't support Array(idx).property = value syntax
    // Transform: ArrayName(idx).property = value
    // To: VPX_SetArrObjProp ArrayName, idx, "property", value
    //
    // VPX_SetArrObjProp is a helper sub that sets the property with error handling.
    // It uses a Select Case for common properties and Execute for unknown ones.
    //
    // IMPORTANT: Only match ASSIGNMENTS at statement start, not comparisons in If conditions!
    // Statement start positions: line start, after :, after Then, after Else
    //
    // Example that must NOT be transformed (comparison in If condition):
    //   If Glowing(b).state = 0 Then ...
    // Example that SHOULD be transformed (assignment after Then):
    //   If x Then Glowing(b).state = 1

    // Pattern requires statement start position to avoid matching comparisons
    // Value capture stops at : or Then/Else keywords or newline (non-greedy)
    std::regex p(R"((^[ \t]*|:[ \t]*|\bThen[ \t]+|\bElse[ \t]+)(\w+)\s*\(\s*([^)]+)\s*\)\s*\.(\w+)\s*=\s*([^:\r\n]+?)(?=[ \t]*(?::|'|\bThen\b|\bElse\b|\r|\n|$)))",
                 std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, p, "$1VPX_SetArrObjProp $2, $3, \"$4\", $5");

    return r;
}


std::string ScriptPatcher::PatchArrayObjectPropertyRead(const std::string& script) {
    std::string r = script;

    // Wine VBScript doesn't support Array(idx).property READ syntax either
    // Transform: ArrayName(idx).property
    // To: VPX_GetArrObjProp(ArrayName, idx, "property")
    //
    // IMPORTANT: Only transform if property is a KNOWN VPX object property.
    // This avoids breaking object method chains like Games(x).Settings

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

    //// Pattern: word(index).property - but we'll check exclusions in callback
    std::regex p(R"((\w+)\s*\(\s*([^)]+)\s*\)\s*\.(\w+)\b)", std::regex::icase);

    std::string result;
    std::sregex_iterator it(r.begin(), r.end(), p);
    std::sregex_iterator end;
    size_t lastPos = 0;

    while (it != end) {
        std::smatch match = *it;
        result += r.substr(lastPos, match.position() - lastPos);

        std::string funcName = match[1].str();
        std::string funcNameLower = funcName;
        std::transform(funcNameLower.begin(), funcNameLower.end(), funcNameLower.begin(), ::tolower);

        std::string indexExpr = match[2].str();
        std::string propName = match[3].str();
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
            result += match[0].str();
        } else {
            // Transform to VPX_GetArrObjProp
            // Wrap in extra parens to force expression evaluation (fixes Wine VBScript parser issue
            // when this appears as first argument in Sub call like: UpdateMaterial VPX_GetArrObjProp(...), ...)
            result += "(VPX_GetArrObjProp(" + match[1].str() + ", " + match[2].str() + ", \"" + match[3].str() + "\"))";
        }

        lastPos = match.position() + match.length();
        ++it;
    }
    result += r.substr(lastPos);

    return result;
}


std::string ScriptPatcher::PatchSingleLineIfElse(const std::string& script) {
    std::string r = script;
    // Use [ \t]+ instead of \s+ to avoid matching across newlines
    std::regex p(R"((Then[ \t]+[^'\r\n]+[ \t]+Else)[ \t]*('.*)?$)", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, p, "$1:$2");
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
    //// Pattern: if ... then if ... then ... end if end if (ALL ON ONE LINE)
    // Use [ \t] instead of \s to prevent matching across lines (newlines)
    std::regex p(R"((^|[\r\n])([ \t]*)(if[ \t]+[^\r\n]+?[ \t]+then)[ \t]+(if[ \t]+[^\r\n]+?[ \t]+then[ \t]+[^\r\n]+?)[ \t]+end[ \t]+if[ \t]+end[ \t]+if)",
                 std::regex::icase);
    r = std::regex_replace(r, p, "$1$2$3\n$2    $4\n$2end if");
    return r;
}


std::string ScriptPatcher::PatchExecuteEval(const std::string& script) { return script; }

std::string ScriptPatcher::PatchStringConcatenation(const std::string& script) {
    std::string r = script;
    std::regex p(R"(\(\s*(\([^)]+\))\s*&\s*")", std::regex::icase);
    r = std::regex_replace(r, p, R"(("" & $1 & ")");
    return r;
}


bool ScriptPatcher::UsesSlingshotCorrection(const std::string& script) {
    std::regex p(R"(Class\s+(SlingshotCorrection|FlipperPolarity|FlipperPhysics|BumperPhysics))", std::regex::icase);
    return std::regex_search(script, p);
}


std::string ScriptPatcher::PatchSlingshotCorrection(const std::string& script) {
    // Legacy - now handled by EmulateClasses
    return script;
}


// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

#endif // __STANDALONE__

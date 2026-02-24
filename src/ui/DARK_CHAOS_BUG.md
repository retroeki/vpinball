# Dark Chaos Segment Display Bug - Investigation Summary

## Problem Statement
14-segment LED displays on the "Dark Chaos" table work on Windows desktop but show as black/invisible on Android. The pregame text "DARK CHAOS" and other segment displays do not render.

## Duration
5 days of investigation (January 2025)

## Symptoms Observed
1. `Primitive::Render` logs show segments being SKIPPED with `color=0x0`
2. `Light::put_Color` called with `color=0` and `intensity=0` for all segment lights
3. Character lookups in `FOURTEEN_SEGMENTS` dictionary only find SPACE (32), not actual letters like 'D', 'A', 'R', 'K'
4. `GetRef` is called for many `Glf_*` functions but NEVER for `Glf_70` or `Glf_71` (the text functions for "DARK" and "CHAOS")

## GLF Framework Understanding
- GLF (Game Logic Framework) tables use a `glf_funcRefMap` dictionary to map text templates to function references
- `Glf_70` returns `"DARK "`, registered with key `"""DARK """`
- `Glf_71` returns `"CHAOS "`, registered with key `"""CHAOS """`
- `GlfInput` class wraps input values and uses `GetRef` for dynamic evaluation when `m_isGetRef = True`
- `Glf_ParseInput` function returns `Array(funcRef, value, True)` where element [2] indicates if GetRef should be used

## Root Cause Identified: Wine VBScript SAFEARRAY Corruption Bug
Arrays returned from functions have their element values corrupted to 0 after the function returns.

### Evidence:
- `Array()` creation logs showed correct values: `VT_BOOL=-1` (True)
- Array element access logs showed corrupted values: `VT_BOOL=0` (False)
- This affects both VT_BOOL and VT_I2 (integer) types
- The corruption happens between function return and caller's array access

## Attempted Fixes (All Failed)

### 1. Change True to Integer
Changed `Array(funcRef, value, True)` to `Array(funcRef, value, 1)`
- **Result**: Failed - integers also corrupted to 0

### 2. Change Comparison Operator
Changed `If m_isGetRef = True Then` to `If m_isGetRef <> 0 Then`
- **Result**: Failed - value is still 0, so condition fails

### 3. Check m_value Prefix Instead
Changed to `If Left(m_value, 4) = "Glf_" Then` to bypass the corrupted flag
- **Result**: Failed - GetRef still not called for Glf_70/71

### 4. Use InStr Instead of Left
Changed to `If InStr(m_value, "Glf_") = 1 Then`
- **Result**: Failed - same as above

### 5. Global Variable Workaround (Partial Success)
Bypassed SAFEARRAY entirely by using global variables:
```vbscript
Dim glf_last_value, glf_last_isGetRef

' In Glf_ParseInput, before Array return:
glf_last_value = funcRef : glf_last_isGetRef = True

' In GlfInput.init:
m_value = glf_last_value
m_isGetRef = glf_last_isGetRef
```

- **Result**: Partial success
  - Debug logs show `m_isGetRef=True` and `m_value=Glf_69`, `Glf_733` etc.
  - But Glf_70 and Glf_71 are STILL never called
  - Other Glf_ functions work, but not the segment text functions

## Current Hypothesis (Unproven)

The segment text "DARK" and "CHAOS" may follow a different code path than dynamic text:

1. **Dictionary key mismatch**: Keys are stored as `"""DARK """` (with quotes as part of the string). Lookups may use `"DARK "` (without extra quotes).

2. **Static vs Dynamic text**: The Glf_70/Glf_71 functions that return static text may never be called because:
   - The segment display code uses the literal text directly
   - Or there's a separate code path for pregame/static text

3. **Multiple dictionaries**: The `Dict::Exists` logs show `dictcount=1330` which suggests the lookup may be hitting a different dictionary (like `FOURTEEN_SEGMENTS` for character patterns) rather than `glf_funcRefMap`.

4. **Segment display initialization**: The segment display may be initialized before `glf_funcRefMap` is populated, causing lookups to fail.

## Files Modified During Investigation

### C++ Files:
- `standalone/inc/wine/dlls/vbscript/interp.c` - Array access logging
- `standalone/inc/wine/dlls/vbscript/global.c` - GetRef and Array() logging
- `standalone/inc/wine/dlls/scrrun/dictionary.c` - Dictionary Exists/Add logging
- `src/ui/simplescriptpatcher.cpp` - GLF Boolean Array patch

### Patch Applied (in SimpleScriptPatcher):
```cpp
// PatchGlfBooleanArray - Injects global variables to bypass SAFEARRAY corruption
// 1. Adds: Dim glf_last_value, glf_last_isGetRef
// 2. Sets globals before Glf_ParseInput returns
// 3. Changes m_value and m_isGetRef to use globals instead of parsedInput array
```

## What Still Needs Investigation

1. **Why Glf_70/Glf_71 specifically are never called** - Other Glf_ functions work
2. **The actual segment display code path** - How does text get from "DARK" to segment colors?
3. **Whether the issue is in VBScript or in the rendering pipeline**
4. **The exact point where segment colors become 0x0**

## Relevant Wine Bug Reports
- Bug 53766: VBScript SAFEARRAY issues
- Related: https://bugs.winehq.org/

## Conclusion
The Wine VBScript SAFEARRAY corruption bug was identified and partially worked around, but the segment displays still don't work. The Glf_70/Glf_71 functions for "DARK"/"CHAOS" text are never called despite the global variable workaround being applied. The root cause of why these specific functions aren't called remains unknown.

---

## Deep Script Analysis (January 21, 2025)

### Script Execution Flow Traced

Analyzed the full patched script (~2.9MB, 70k+ lines) to understand the exact code path.

#### Execution Order Confirmed:

1. **Module-level code runs first** (before any events):
   - Line 50015: `Dim glf_funcRefMap : Set glf_funcRefMap = CreateObject("Scripting.Dictionary")`
   - Line 66920: `glf_funcRefMap.Add """DARK """, "Glf_70"` ← **Key is registered here**
   - Line 66924: `glf_funcRefMap.Add """CHAOS   """, "Glf_71"`

2. **Table1_Init event fires** (after module-level code completes):
   - Line 492: `ConfigureGlfDevices` called
   - Line 493: `Glf_Init(Table1)` called

3. **ConfigureGlfDevices (line 4913)** creates modes:
   - Line 4977: `CreateAttractMode` called

4. **CreateAttractMode (line 6093)** configures segment displays:
   - Line 6517: `.Text = """DARK """` ← **Lookup happens here**
   - Line 6528: `.Text = """CHAOS   """`

#### Key Insight: The Dictionary Key SHOULD Exist Before Lookup

The execution order proves that `glf_funcRefMap.Add """DARK """, "Glf_70"` (line 66920) runs **before** `.Text = """DARK """` (line 6517) is executed. So the key should be in the dictionary.

### Code Path Analysis

#### 1. Text Property Setting (line 57811-57812):
```vbscript
Public Property Let Text(input)
    Set m_text = (new GlfInput)(input)
End Property
```

#### 2. GlfInput.init (line 52209-52225):
```vbscript
Public default Function init(input)
    m_raw = input
    Dim parsedInput : parsedInput = Glf_ParseInput(input)
    ' ... player/device state checks ...
    m_value = glf_last_value      ' Uses global workaround
    m_isGetRef = glf_last_isGetRef ' Uses global workaround
    Set Init = Me
End Function
```

#### 3. Glf_ParseInput (line 50829-50890):
```vbscript
Public Function Glf_ParseInput(value)
    If glf_funcRefMap.Exists(CStr(value)) Then    ' ← LINE 50833: KEY CHECK
        glf_last_value = glf_funcRefMap(CStr(value)) : glf_last_isGetRef = True
        Glf_ParseInput = Array(glf_funcRefMap(CStr(value)), value, True)
    Else
        ' Creates new dynamic function...
        glf_last_value = funcRef : glf_last_isGetRef = True
        Glf_ParseInput = Array(funcRef, value, True)
    End If
End Function
```

#### 4. GlfInput.Value Property (line 52190-52197):
```vbscript
Public Property Get Value()
    If Left(m_value, 4) = "Glf_" Then Debug.Print "GlfValue: m_value=" & m_value & " m_isGetRef=" & m_isGetRef
    If m_isGetRef = True Then
        Value = GetRef(m_value)(Null)  ' ← Calls Glf_70 which returns "DARK "
    Else
        Value = m_value
    End If
End Property
```

#### 5. Segment Display Rendering (line 63501):
```vbscript
Dim new_text : new_text = Glf_SegmentTextCreateCharacters(text.Value(), m_size, ...)
```

#### 6. Character Processing (line 64417-64418):
```vbscript
For i = Len(text) To 1 Step -1
    char_code = Asc(Mid(text, i, 1))  ' Gets ASCII code for each character
```

#### 7. Segment Lookup (line 64175-64176):
```vbscript
If segment_mapping.Exists(char("char_code")) Then
    Set mapping = segment_mapping(char("char_code"))  ' FOURTEEN_SEGMENTS lookup
```

### Root Cause Hypothesis: Wine Dictionary String Comparison Bug

The problem is at **line 50833**: `glf_funcRefMap.Exists(CStr(value))` returns `False` on Wine even though the key was added.

#### String Key Analysis:

In VBScript, `"""DARK """` as a literal produces:
- `"` (quote char, ASCII 34)
- `D` `A` `R` `K` ` ` (literal characters)
- `"` (quote char, ASCII 34)

So both the key (line 66920) and the lookup value (line 6517) should be identical: `"DARK "` (with embedded quotes).

#### Why Wine Might Fail:

1. **Wine's CStr() may normalize strings differently** - Could strip or modify quote characters
2. **Dictionary.Exists() binary comparison issue** - Wine might use different string comparison semantics
3. **Quote character encoding difference** - Wine might use a different code point for `"` (Chr(34))
4. **Unicode vs ANSI handling** - The embedded quotes might be stored differently

### Proposed New Fixes

#### Fix 1: Debug Byte-Level String Comparison
Add hex dump of key and lookup value:
```vbscript
Function Glf_StringToHex(s)
    Dim i, result
    result = ""
    For i = 1 To Len(s)
        result = result & Hex(Asc(Mid(s, i, 1))) & " "
    Next
    Glf_StringToHex = result
End Function

' In Glf_ParseInput before Exists check:
Debug.Print "Lookup: [" & value & "] Hex: " & Glf_StringToHex(CStr(value))
```

#### Fix 2: Direct Hardcoded Mapping for Static Text
Patch GlfInput.init to bypass dictionary for known static text:
```vbscript
' After Glf_ParseInput call in GlfInput.init:
If input = """DARK """ Then
    m_value = "Glf_70" : m_isGetRef = True
ElseIf input = """CHAOS   """ Then
    m_value = "Glf_71" : m_isGetRef = True
End If
```

#### Fix 3: Use Alternative Key Format
Change the key format to avoid embedded quotes:
```vbscript
' Instead of: glf_funcRefMap.Add """DARK """, "Glf_70"
' Use: glf_funcRefMap.Add "STATIC:DARK ", "Glf_70"
```

#### Fix 4: Force Dictionary CompareMode
Ensure dictionary uses text comparison:
```vbscript
glf_funcRefMap.CompareMode = 1  ' vbTextCompare
```

#### Fix 5: Iterate Keys to Find Match
If Exists fails, manually search:
```vbscript
Function FindInDict(dict, searchValue)
    Dim k
    For Each k In dict.Keys()
        If k = searchValue Then
            FindInDict = dict(k)
            Exit Function
        End If
    Next
    FindInDict = Empty
End Function
```

### Files Analyzed
- `C:\vpinball-master\src\ui\patched_script.vbs` (pulled from Android device)
  - Line 6517: `.Text = """DARK """` - where text is set
  - Line 50833: `glf_funcRefMap.Exists(CStr(value))` - where lookup fails
  - Line 52222-52223: Global variable workaround applied
  - Line 66920: `glf_funcRefMap.Add """DARK """, "Glf_70"` - where key is registered

### Next Steps
1. Add byte-level debug logging to confirm string encoding difference
2. Implement Fix 2 (direct hardcoded mapping) as a quick workaround
3. Investigate Wine's Scripting.Dictionary source code for comparison logic

---

## Runtime Debug Session (January 21, 2025 - Evening)

### Major Discovery: Dictionary Lookup WORKS!

Added hex dump debug logging and tested on Android. **The dictionary lookup is NOT the problem!**

#### Evidence from logs:
```
Dict::Exists key='"DARK "' found=1
Glf_ParseInput: value=["DARK "] hex=[22 44 41 52 4B 20 22 ] exists=True
SEGDEBUG Dict::get_Item funcref key='"DARK "' found=1 item_vt=8 dictcount=1330
Glf_ParseInput SET: glf_last_value=[Glf_70] glf_last_isGetRef=True
```

**Confirmed working:**
1. ✅ `glf_funcRefMap.Exists(CStr(value))` returns `True` for `"DARK "`
2. ✅ Hex encoding is correct: `22 44 41 52 4B 20 22` = `" D A R K   "`
3. ✅ `glf_last_value` is set to `"Glf_70"`
4. ✅ `glf_last_isGetRef` is set to `True`

### The REAL Problem: Text Never Reaches Segment Display

#### GlfInput.Value Results:
```
GlfInput.Value RESULT: [240] hex=[32 34 30 ]
GlfInput.Value RESULT: [55] hex=[35 35 ]
```

- `.Value` IS being called for some GlfInput objects (returns 240, 55 - numeric values)
- `.Value` is NEVER called returning `"DARK "` - no such log appears
- Either the "DARK" GlfInput objects never have `.Value` accessed, OR `m_value` doesn't contain `"Glf_70"` when accessed

#### SegmentTextCreate Input:
```
SegmentTextCreate INPUT: text=[        ] hex=[20 20 20 20 20 20 20 20 ]
SegmentTextCreate INPUT: text=[                ] hex=[20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 ]
```

- Segment display receives **ALL SPACES** (ASCII 0x20)
- The text `"DARK "` never reaches `Glf_SegmentTextCreateCharacters`

### Updated Hypothesis: Global Variable Race Condition

The global variable workaround may have a timing issue:

1. `Glf_ParseInput("DARK ")` is called
2. Sets `glf_last_value = "Glf_70"` ✅
3. **SOMETHING** overwrites `glf_last_value` before...
4. `GlfInput.init` reads `m_value = glf_last_value`

Possible culprits in GlfInput.init between ParseInput call and global read:
```vbscript
Dim parsedInput : parsedInput = Glf_ParseInput(input)  ' Sets globals
Dim playerState : playerState = Glf_CheckForGetPlayerState(input)  ' May call ParseInput?
' ... more code ...
m_value = glf_last_value  ' Reads globals - but are they still correct?
```

### Current Debug: Tracing m_value Assignment

Added logging to show what `m_value` actually receives:
```vbscript
m_value = glf_last_value : If Left(CStr(m_value), 4) = "Glf_" Then Debug.Print "GlfInput.init ASSIGNED: m_raw=[" & m_raw & "] m_value=[" & m_value & "]"
```

Expected log if working:
```
GlfInput.init ASSIGNED: m_raw=["DARK "] m_value=[Glf_70]
```

### Debug Patches Applied (simplescriptpatcher.cpp)

1. **PatchGlfDictionaryDebug** - Injected `Glf_StringToHex()` function and debug logging:
   - Before `Exists()` check: logs value, hex, and exists result
   - After `glf_last_value` set: logs the assigned value
   - After `GetRef()` call: logs the returned value
   - At `Glf_SegmentTextCreateCharacters`: logs input text

2. **Modified PatchGlfBooleanArray** - Added debug to `m_value` assignment to trace what value is actually stored

### Key Insight

The bug is **NOT** in the dictionary lookup. The dictionary correctly:
- Stores `"DARK "` → `"Glf_70"` mapping
- Returns `found=1` when queried
- Sets global variables correctly

The bug is **downstream** - somewhere between setting `glf_last_value="Glf_70"` and the segment display calling `.Value` on the GlfInput object.

---

## Deeper Investigation (January 21, 2025 - Late Evening)

### Confirmed: GlfInput.init Works Correctly

The assignment logging confirmed that `m_value` IS correctly set:
```
GlfInput.init ASSIGNED: m_raw=["DARK "] m_value=[Glf_70]
```

So the GlfInput object for "DARK" text is properly initialized with `m_value="Glf_70"`.

### But: The Object Is Never Used!

Added logging to `UpdateStack` which processes the text stack for display:
```
UpdateStack BEFORE Value: m_value=[Glf_10001]
UpdateStack AFTER Value: text_value=[        ]
```

**Critical finding**: When the segment display updates, it's using `Glf_10001`, `Glf_10002`, `Glf_10003` - these are **dynamically created functions** (numbers > 10000) that return spaces. NOT the pre-registered `Glf_70`!

### AddTextEntry Is Never Called

Added debug to `AddTextEntry` - the function that should add new text to the segment display:
- **Result**: `AddTextEntry CALLED:` log **never appears**

This means the "show_attract_title" event fires, but the segment display player callback never reaches `AddTextEntry`.

### Event System Trace

1. ✅ Timer `attract_display` fires (logs show tick events)
2. ✅ `SegmentPlayerEventHandler` is called (GetRef log appears)
3. ❌ `AddTextEntry` is never called

### Root Cause Found: Evaluate() Returns False

In `SegmentPlayerEventHandler` (line 57970):
```vbscript
If ownProps(2).GlfEvent.Evaluate() Then
    SegmentPlayer.Play ownProps(3), ownProps(2)
End If
```

The `Evaluate()` method checks the event condition (e.g., `devices.timers.attract_display.ticks == 1`). If it returns `False`, `SegmentPlayer.Play` is never called, and thus `AddTextEntry` is never reached.

**Hypothesis**: The condition evaluation is failing due to another Wine VBScript bug, possibly related to:
- Comparison operators
- Property access on objects
- The `ticks` value not being read correctly

### Current Debug: Testing Evaluate()

Added logging before the condition check:
```vbscript
Debug.Print "SegmentPlayerEventHandler: Evaluate()=" & ownProps(2).GlfEvent.Evaluate()
If ownProps(2).GlfEvent.Evaluate() Then
```

### Summary of Bug Chain

1. ✅ Dictionary lookup works
2. ✅ `glf_last_value` set to `"Glf_70"`
3. ✅ `GlfInput.init` assigns `m_value = "Glf_70"`
4. ✅ Timer events fire
5. ✅ `SegmentPlayerEventHandler` is called
6. ❓ `Evaluate()` returns False (testing now)
7. ❌ `SegmentPlayer.Play` never called
8. ❌ `AddTextEntry` never called
9. ❌ GlfInput with `m_value="Glf_70"` is never accessed
10. ❌ Segment display shows default spaces (`Glf_10001` etc.)

### Why Default Spaces Appear

The segment displays are initialized with default empty text entries during script setup. These have dynamically created functions (`Glf_10001`, etc.) that return `String(m_size, " ")` (spaces).

When the "show_attract_title" event should update the display with "DARK", `AddTextEntry` should be called to push the new text. But since `Evaluate()` fails, the update never happens, and the displays keep showing the default spaces.

---

## RESOLUTION FOUND (January 21, 2025 - Night)

### Final Root Cause: Wine VBScript Error 450 - VBSE_FUNC_ARITY_MISMATCH

After adding error handling around the `SegmentPlayerCallbackHandler` call, the actual error was revealed:

```
ERROR in SegmentPlayerCallbackHandler call: 450 - VBSE_FUNC_ARITY_MISMATCH
```

### The Culprit: Invalid VBScript Syntax in GLF Framework

In `GlfSegmentPlayerEventItem` class (line 57851-57852):
```vbscript
Public Property Get HasTransition() : HasTransition = (Not IsNull)(m_transition) : End Property
Public Property Get HasTransitionOut() : HasTransitionOut = (Not IsNull)(m_transition_out) : End Property
```

**The bug**: `(Not IsNull)(m_transition)` is **invalid VBScript syntax**!

This is being interpreted by Wine as:
1. `(Not IsNull)` - Try to negate the `IsNull` function reference
2. `(m_transition)` - Then call the result with `m_transition` as argument

This causes **Error 450: Arity Mismatch** because the result of `(Not IsNull)` cannot be called with an argument.

**Windows VBScript** is lenient and somehow interprets this as intended: `Not IsNull(m_transition)`
**Wine VBScript** is strict and throws an error

### The Fix: PatchParenthesizedNot

Added new patch function to `simplescriptpatcher.cpp`:

```cpp
// Fix parenthesized Not function calls - Wine VBScript arity mismatch bug
// Pattern: (Not IsNull)(m_transition) -> Not IsNull(m_transition)
std::string SimpleScriptPatcher::PatchParenthesizedNot(const std::string& script) {
    // Match pattern: (Not FunctionName)(argument)
    // Replace with: Not FunctionName(argument)
    static const RE2 p(R"(\(Not\s+(\w+)\)\s*\(([^)]+)\))");
    ...
}
```

### Instances Fixed by Patch

The patch found and fixed 3 instances:
1. `(Not IsEmpty)(Rampballs(x,1)` → `Not IsEmpty(Rampballs(x,1)`
2. `(Not IsNull)(m_transition)` → `Not IsNull(m_transition)`
3. `(Not IsNull)(m_transition_out)` → `Not IsNull(m_transition_out)`

### Complete Call Chain (Now Working)

1. ✅ Timer `attract_display` fires
2. ✅ `SegmentPlayerEventHandler` called
3. ✅ `Evaluate()` returns `True`
4. ✅ `GlfSegmentDisplayPlayer.Play` called
5. ✅ `SegmentPlayerCallbackHandler` called (no more Error 450!)
6. ✅ `segment_item.HasTransition()` works (syntax fixed!)
7. ✅ `AddTextEntry` called with "DARK " text
8. ✅ Segment displays render correctly!

### Summary

| Issue | Status |
|-------|--------|
| Wine SAFEARRAY corruption | ✅ Worked around with global variables |
| Dictionary lookup | ✅ Works correctly |
| GlfInput.init assignment | ✅ Works correctly |
| Event system (timers) | ✅ Works correctly |
| Evaluate() condition | ✅ Works correctly |
| HasTransition() syntax bug | ✅ **FIXED** with PatchParenthesizedNot |
| SegmentPlayerCallbackHandler | ✅ Now executes without error |
| AddTextEntry | ✅ Now called correctly |
| **Segment displays** | ✅ **NOW RENDERING!** |

### Files Modified

- `src/ui/simplescriptpatcher.cpp` - Added `PatchParenthesizedNot()` function
- `src/ui/simplescriptpatcher.h` - Added declaration

### Conclusion

After 5 days of investigation, the root cause was traced through multiple layers:
1. Initial hypothesis: Wine SAFEARRAY corruption (partially correct, worked around)
2. Second hypothesis: Dictionary lookup failure (incorrect, lookup works)
3. Third hypothesis: Evaluate() condition failure (incorrect, condition passes)
4. **Final answer**: Invalid VBScript syntax `(Not Func)(arg)` that Wine rejects with Error 450

The `PatchParenthesizedNot` fix converts this invalid syntax to valid VBScript, allowing the segment display callback chain to complete successfully.

**The Dark Chaos 14-segment LED displays now work on Android!**

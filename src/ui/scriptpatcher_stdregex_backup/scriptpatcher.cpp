/**
 * @file scriptpatcher.cpp
 * @brief Main entry point for VBScript patching for Wine/Android compatibility
 *
 * This is the main entry point for the ScriptPatcher. It coordinates
 * the various patching phases:
 * 1. Class emulation (scriptpatcher_classes.cpp)
 * 2. Wine compatibility patches (scriptpatcher_wine.cpp)
 *
 * =============================================================================
 * IMPORTANT LEARNINGS FROM DEBUGGING (January 2026)
 * =============================================================================
 *
 * 1. WINE VBSCRIPT DOES SUPPORT CLASSES
 *    - We initially assumed Wine VBScript didn't support the "Class" keyword
 *    - This was INCORRECT - Wine VBScript handles most classes fine
 *    - Class emulation should only be applied when absolutely necessary
 *    - Over-aggressive class emulation can BREAK tables that work natively
 *
 * 2. DATA EAST (DE) ROMS WORK FINE
 *    - DE ROMs (like Back to the Future) work correctly on Wine/Android
 *    - The PinMAME integration functions properly
 *
 * 3. DUPLICATE vpmInit CALLS BREAK FLIPPERS
 *    - Some table scripts mistakenly call "vpmInit Me" twice
 *    - This corrupts the SolCallback array and flipper state
 *    - Symptoms: Key input detected (event 7) but flippers don't move
 *    - Solution: RemoveDuplicateVpmInit() comments out duplicate calls
 *
 * 4. DEBUG LOGGING IN WINE VBSCRIPT
 *    - Added recursion depth tracking in standalone/inc/wine/dlls/vbscript/interp.c
 *    - Logs function names when recursion depth > 50
 *    - Aborts with E_FAIL if depth > 500 (prevents stack overflow crashes)
 *    - Helps identify which VBScript function causes infinite recursion
 *
 * 5. UNUSED CLASSES CAN CAUSE WINE CRASHES
 *    - Wine VBScript has bugs with certain class patterns
 *    - Even if a class is never instantiated, it can crash Wine
 *    - RemoveUnusedClasses() safely removes classes that are never "New"ed
 *
 * 6. SINGLE-LINE IF SYNTAX ISSUES
 *    - Invalid pattern: "If cond Then stmt End If" (End If not valid on single-line)
 *    - Valid pattern: "If cond Then stmt" (no End If on single-line)
 *    - PatchSingleLineIfEndIf() fixes these invalid patterns
 *
 * 7. NATIVE CLASSES MUST BE PROTECTED
 *    - Classes that pass "Me" to external code (like vpmTimer.addResetObj Me)
 *    - These MUST remain native VBScript classes, not Dictionary emulation
 *    - ExtractNativeClasses() identifies and protects these classes
 *
 * =============================================================================
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <regex>
#include <sstream>

// VPinballGetInternalPath is defined in VPinballLib.cpp
extern "C" const char* VPinballGetInternalPath();

// ============================================================================
// PATCH CONFIGURATION FLAGS
// ============================================================================
// Set these to false to disable specific patches for debugging.
// Master switch and individual patch controls for isolating issues.

struct ScriptPatcherConfig {
    bool enabled = true;                    // Master switch - disable ALL patching
    bool logAnalysisReport = true;          // Log script analysis before patching
    bool logPatchApplications = true;       // Log each patch as it's applied
    bool removeUnusedClasses = true;        // Remove classes that are never instantiated
    bool removeDuplicateVpmInit = true;     // Remove duplicate vpmInit Me calls
    bool classEmulation = true;             // Transform classes to Dictionary objects
    bool dtArrayPatches = true;             // DTArray/STArray compatibility
    bool wineArrayPatches = true;           // Wine array compatibility (UBound, etc.)
    bool singleLineIfPatches = true;        // Fix invalid single-line If patterns
};

static ScriptPatcherConfig g_patchConfig;

// ============================================================================
// SCRIPT ANALYSIS REPORT
// ============================================================================
// Analyzes script BEFORE patching to identify potential issues.
// Outputs warnings for common problems that cause Wine/Android failures.

static void AnalyzeScript(const std::string& script) {
    if (!g_patchConfig.logAnalysisReport) return;

    PLOGI.printf("ScriptPatcher: ========== SCRIPT ANALYSIS REPORT ==========");
    PLOGI.printf("ScriptPatcher: Script length: %zu characters", script.length());

    // Count vpmInit calls
    std::regex vpmInitRegex(R"(\bvpmInit\s+[Mm]e\b)", std::regex::icase);
    auto vpmInitBegin = std::sregex_iterator(script.begin(), script.end(), vpmInitRegex);
    auto vpmInitEnd = std::sregex_iterator();
    int vpmInitCount = std::distance(vpmInitBegin, vpmInitEnd);
    if (vpmInitCount > 1) {
        PLOGI.printf("ScriptPatcher: [!] WARNING - Found %d vpmInit calls (should be 1)", vpmInitCount);
    } else if (vpmInitCount == 1) {
        PLOGI.printf("ScriptPatcher: vpmInit calls: %d (OK)", vpmInitCount);
    } else {
        PLOGI.printf("ScriptPatcher: vpmInit calls: 0 (standalone table?)");
    }

    // Count Class definitions
    std::regex classRegex(R"(^[ \t]*Class\s+(\w+))", std::regex::icase | std::regex::multiline);
    std::vector<std::string> classNames;
    auto classBegin = std::sregex_iterator(script.begin(), script.end(), classRegex);
    auto classEnd = std::sregex_iterator();
    for (auto it = classBegin; it != classEnd; ++it) {
        classNames.push_back((*it)[1].str());
    }
    PLOGI.printf("ScriptPatcher: Class definitions found: %zu", classNames.size());

    // Detect ROM type from LoadVPM call
    std::regex loadVpmRegex("LoadVPM\s+\"[^\"]+\",\s*\"([^\"]+)\"", std::regex::icase);
    std::smatch vpmMatch;
    if (std::regex_search(script, vpmMatch, loadVpmRegex)) {
        PLOGI.printf("ScriptPatcher: ROM System: %s", vpmMatch[1].str().c_str());
    }

    // Detect cGameName
    std::regex gameNameRegex("Const\s+cGameName\s*=\s*\"([^\"]+)\"", std::regex::icase);
    std::smatch gameMatch;
    if (std::regex_search(script, gameMatch, gameNameRegex)) {
        PLOGI.printf("ScriptPatcher: ROM Name: %s", gameMatch[1].str().c_str());
    }

    // Count potential problem patterns - single line If...End If
    std::regex singleLineIfEndIf(R"(\bIf\b[^\n]+\bThen\b[^\n]+\bEnd\s+If\b)", std::regex::icase);
    auto ifBegin = std::sregex_iterator(script.begin(), script.end(), singleLineIfEndIf);
    auto ifEnd = std::sregex_iterator();
    int badIfCount = std::distance(ifBegin, ifEnd);
    if (badIfCount > 0) {
        PLOGI.printf("ScriptPatcher: [!] WARNING - Found %d invalid single-line If...End If", badIfCount);
    }

    // Check for DTArray/STArray usage
    bool usesDTArray = script.find("DTArray") != std::string::npos;
    bool usesSTArray = script.find("STArray") != std::string::npos;
    if (usesDTArray) PLOGI.printf("ScriptPatcher: Uses DTArray (drop targets)");
    if (usesSTArray) PLOGI.printf("ScriptPatcher: Uses STArray (standup targets)");

    PLOGI.printf("ScriptPatcher: ================================================");
}


// Array-based DropTarget helper functions (Wine compatible)
// Wine VBScript can't handle chained array access DTArray(i)(j) but CAN handle:
//   temp = arr(idx) : result = temp(j)
// DTArray elements are plain VBScript Arrays: Array(primary, secondary, prim, sw, animate, isDropped)
//                                    indices:       0         1        2    3      4         5
const char* ScriptPatcher::DROP_TARGET_CLASS = R"(
' Get value from DTArray element by property name
Function DTGet(arr, idx, propName)
  Dim temp, propIdx
  temp = arr(idx)
  Select Case LCase(propName)
    Case "primary": propIdx = 0
    Case "secondary": propIdx = 1
    Case "prim": propIdx = 2
    Case "sw": propIdx = 3
    Case "animate": propIdx = 4
    Case "isdropped": propIdx = 5
    Case Else: DTGet = Empty : Exit Function
  End Select
  DTGet = temp(propIdx)
End Function

' Get object from DTArray element by property name
Function DTGetObj(arr, idx, propName)
  Dim temp, propIdx
  temp = arr(idx)
  Select Case LCase(propName)
    Case "primary": propIdx = 0
    Case "secondary": propIdx = 1
    Case "prim": propIdx = 2
    Case Else: Set DTGetObj = Nothing : Exit Function
  End Select
  Set DTGetObj = temp(propIdx)
End Function

' Set value on DTArray element by property name
Sub DTSet(arr, idx, propName, val)
  Dim temp, propIdx
  Select Case LCase(propName)
    Case "sw": propIdx = 3
    Case "animate": propIdx = 4
    Case "isdropped": propIdx = 5
    Case Else: Exit Sub
  End Select
  temp = arr(idx)
  temp(propIdx) = val
  arr(idx) = temp
End Sub
)";

// Array-based StandupTarget helper functions (Wine compatible)
// Wine VBScript can't handle chained array access STArray(i)(j) but CAN handle:
//   temp = arr(idx) : result = temp(j)
// STArray elements are plain VBScript Arrays: Array(primary, prim, sw, animate, target)
//                                    indices:       0      1    2      3       4
const char* ScriptPatcher::STANDUP_TARGET_CLASS = R"(
' Get value from STArray element by property name
Function STGet(arr, idx, propName)
  Dim temp, propIdx
  temp = arr(idx)
  Select Case LCase(propName)
    Case "primary": propIdx = 0
    Case "prim": propIdx = 1
    Case "sw": propIdx = 2
    Case "animate": propIdx = 3
    Case "target": propIdx = 4
    Case Else: STGet = Empty : Exit Function
  End Select
  STGet = temp(propIdx)
End Function

' Get object from STArray element by property name
Function STGetObj(arr, idx, propName)
  Dim temp, propIdx
  temp = arr(idx)
  Select Case LCase(propName)
    Case "primary": propIdx = 0
    Case "prim": propIdx = 1
    Case Else: Set STGetObj = Nothing : Exit Function
  End Select
  Set STGetObj = temp(propIdx)
End Function

' Set value on STArray element by property name
Sub STSet(arr, idx, propName, val)
  Dim temp, propIdx
  Select Case LCase(propName)
    Case "sw": propIdx = 2
    Case "animate": propIdx = 3
    Case "target": propIdx = 4
    Case Else: Exit Sub
  End Select
  temp = arr(idx)
  temp(propIdx) = val
  arr(idx) = temp
End Sub
)";

// Utility: Trim whitespace

// ============================================================================
// NATIVE CLASS PROTECTION
// ============================================================================

std::vector<ScriptPatcher::NativeClassInfo> ScriptPatcher::ExtractNativeClasses(std::string& script) {
    std::vector<NativeClassInfo> nativeClasses;

    std::regex classStartRegex(R"(^[ \t]*Class\s+(\w+))", std::regex::icase | std::regex::multiline);
    std::regex classEndRegex(R"(^[ \t]*End\s+Class)", std::regex::icase | std::regex::multiline);
    std::regex addResetObjPattern(R"(\bvpmTimer\s*\.\s*addResetObj\s+Me\b)", std::regex::icase);

    std::sregex_iterator it(script.begin(), script.end(), classStartRegex);
    std::sregex_iterator end;

    std::vector<std::tuple<std::string, size_t, size_t>> classRanges;

    while (it != end) {
        std::string className = (*it)[1].str();
        size_t classStart = (*it).position();
        std::string afterClass = script.substr(classStart);
        std::smatch endMatch;
        if (std::regex_search(afterClass, endMatch, classEndRegex)) {
            size_t classEnd = classStart + endMatch.position() + endMatch.length();
            std::string classBody = script.substr(classStart, classEnd - classStart);
            if (std::regex_search(classBody, addResetObjPattern)) {
                classRanges.push_back({className, classStart, classEnd});
                PLOGI.printf("ScriptPatcher: Found native class '%s' (uses vpmTimer.addResetObj Me)", className.c_str());
            }
        }
        ++it;
    }

    std::sort(classRanges.begin(), classRanges.end(),
              [](const auto& a, const auto& b) { return std::get<1>(a) > std::get<1>(b); });

    for (const auto& [className, startPos, endPos] : classRanges) {
        NativeClassInfo info;
        info.name = className;
        info.fullText = script.substr(startPos, endPos - startPos);
        info.placeholder = "' __NATIVE_CLASS_PLACEHOLDER_" + className + "__";
        script = script.substr(0, startPos) + info.placeholder + "\n" + script.substr(endPos);
        nativeClasses.push_back(info);
        PLOGI.printf("ScriptPatcher: Extracted native class '%s' (%zu chars)", className.c_str(), info.fullText.length());
    }

    return nativeClasses;
}

std::string ScriptPatcher::RestoreNativeClasses(const std::string& script,
                                                 const std::vector<NativeClassInfo>& nativeClasses) {
    std::string result = script;
    for (const auto& info : nativeClasses) {
        size_t pos = result.find(info.placeholder);
        if (pos != std::string::npos) {
            result = result.substr(0, pos) + info.fullText + result.substr(pos + info.placeholder.length());
            PLOGI.printf("ScriptPatcher: Restored native class '%s'", info.name.c_str());
        }
    }
    return result;
}


// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

std::string ScriptPatcher::PatchScript(const std::string& script) {
    // Master switch - disable ALL patching when needed for debugging
    if (!g_patchConfig.enabled) {
        PLOGI.printf("ScriptPatcher: DISABLED - Returning unpatched script");
        return StripBOM(script);
    }

    std::string result = StripBOM(script);
    bool patched = result.length() != script.length();
    bool classEmulationApplied = false;

    // Run analysis report BEFORE any patching
    AnalyzeScript(result);

    PLOGI.printf("ScriptPatcher: Beginning patch process...");

    // STEP 0: Remove unused class definitions (Wine workaround)
    // Wine VBScript has bugs with certain class patterns that can cause crashes
    // even when the class is never instantiated. Comment out unused classes.
    {
        std::string before = result;
        result = RemoveUnusedClasses(result);
        if (result != before) {
            patched = true;
            PLOGI.printf("ScriptPatcher: Removed unused class definitions (Wine workaround)");
        }
    }

    // STEP 0.5: Remove duplicate vpmInit calls (Wine workaround)
    // Some table scripts mistakenly call vpmInit Me twice which breaks flippers
    {
        std::string before = result;
        result = RemoveDuplicateVpmInit(result);
        if (result != before) {
            patched = true;
        }
    }


    // STEP 1: Extract native classes BEFORE any processing
    // These classes pass Me to external code and must remain 100% native
    std::vector<NativeClassInfo> nativeClasses = ExtractNativeClasses(result);
    if (!nativeClasses.empty()) {
        patched = true;
        PLOGI.printf("ScriptPatcher: Extracted %zu native classes for protection", nativeClasses.size());
    }


    // STEP 2: Class emulation (for remaining classes)
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

    // DTArray/STArray - Array-based approach (keep as plain arrays, use helper functions)
    // Wine can't handle chained array access DTArray(i)(j), but CAN handle:
    //   temp = arr(idx) : result = temp(j)
    // So we inject helper functions and transform access patterns, but DON'T convert to Dictionary
    if (UsesDTArray(result)) {
        PLOGI.printf("ScriptPatcher: Applying DTArray patches (array-based)");
        result = InjectDropTargetClass(result);
        result = PatchDTArrayAccess(result);
        patched = true;
    }
    if (UsesSTArray(result)) {
        PLOGI.printf("ScriptPatcher: Applying STArray patches (array-based)");
        result = InjectStandupTargetClass(result);
        result = PatchSTArrayAccess(result);
        patched = true;
    }

    // Other patches
    if (UsesControllerPause(result)) { result = PatchControllerPause(result); patched = true; }
    if (UsesPuPlayerPlaystopInPlayclear(result)) { result = PatchPuPlayerPlaystopInPlayclear(result); patched = true; }

    { std::string b = result; result = PatchAddScoreParentheses(result); if (result != b) patched = true; }
    { std::string b = result; result = PatchSetAlignedPositionParentheses(result); if (result != b) patched = true; }
    // Fix invalid single-line If...End If patterns (Wine VBScript crashes on these)
    { std::string b = result; result = PatchSingleLineIfEndIf(result); if (result != b) patched = true; }
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
        result = PatchNestedArrayAssignment(result);
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

    // FINAL STEP: Restore native classes (untransformed) back into the script
    if (!nativeClasses.empty()) {
        result = RestoreNativeClasses(result, nativeClasses);
        PLOGI.printf("ScriptPatcher: Restored %zu native classes", nativeClasses.size());
    }

    if (patched) {
        PLOGI.printf("ScriptPatcher: Complete (length=%zu)", result.length());
        // Dump patched script to app's internal directory for error reporting
        // This file is replaced each time a new table loads
        // Use VPinballGetInternalPath() which is set by the Android app to get the correct
        // com.retroeki.app internal folder (SDL_GetBasePath returns org.vpinball.app path)
        const char* internalPath = VPinballGetInternalPath();
        PLOGI.printf("ScriptPatcher: VPinballGetInternalPath returned: %s", internalPath ? internalPath : "NULL");
        if (internalPath) {
            std::string scriptPath = std::string(internalPath) + "/patched_script.vbs";
            PLOGI.printf("ScriptPatcher: Attempting to write to: %s", scriptPath.c_str());
            FILE* debugFile = fopen(scriptPath.c_str(), "w");
            if (debugFile) {
                fwrite(result.c_str(), 1, result.length(), debugFile);
                fclose(debugFile);
                PLOGI.printf("ScriptPatcher: Script saved to %s", scriptPath.c_str());
            } else {
                PLOGE.printf("ScriptPatcher: Failed to open file for writing: %s (errno=%d)", scriptPath.c_str(), errno);
            }
        } else {
            PLOGI.printf("ScriptPatcher: Internal path not set - skipping script dump");
        }
    }
    return result;
}


#endif // __STANDALONE__


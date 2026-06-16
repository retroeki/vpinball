/**
 * @file simplescriptpatcher.cpp
 * @brief Simplified Wine VBScript compatibility patches for Android/Wine
 *
 * A minimal script patcher that addresses ONLY confirmed Wine VBScript bugs
 * without the complexity of full class emulation.
 *
 * Philosophy:
 * - Less transformation = less breakage
 * - Only patch what Wine actually fails on
 * - Reuse existing helper wrappers (VPX_SafeUBound, etc.)
 *
 * =============================================================================
 * LESSONS LEARNED FROM DEBUGGING SESSIONS (January 2026)
 * =============================================================================
 *
 * 1. FOR EACH CONVERSION (PatchSingleLineForEach)
 *    - Wine VBScript does NOT support single-line For Each syntax
 *    - Initial fix: Convert For Each to indexed For loop
 *    - PROBLEM: Using `var = array(i)` fails for COM objects - needs `Set var = array(i)`
 *    - PROBLEM: Wrapping with `On Error Resume Next` caused INFINITE LOOPS/ANR
 *      when UBound() failed on undefined arrays. VBScript behavior under error
 *      suppression with For loops is unpredictable.
 *    - SOLUTION: Use `If IsArray(collection) Then` guard instead of error handling.
 *      This cleanly skips undefined arrays without risk of infinite loops.
 *
 * 2. RENDERINGMODE AND TESTVRONDT (PatchRenderingMode, PatchTestVRonDT)
 *    - These are VR-related variables that may not be defined on Android
 *    - Initial fix for RenderingMode: Check if already defined via `Dim` or `Const`
 *    - PROBLEM: Pattern `RenderingMode\s*=` matched comparisons like `If RenderingMode = 2`
 *      causing the patcher to think it was already defined as an assignment
 *    - SOLUTION: Only check for `Dim` or `Const` declarations, not assignments
 *
 *    - TestVRonDT had a different issue: Script had `Const TestVRonDT = false`
 *      declared AFTER its first use (invalid VBScript, but scripts do this)
 *    - SOLUTION: Remove the Const declaration entirely and replace all usages with False
 *
 * 3. B2SSETDATA ERROR HANDLING (PatchControllerChangedLamps patterns 3a/3b/3c)
 *    - Controller.B2SSetData is a B2S (Backglass Server) method that may fail on Android
 *    - PROBLEM: B2SSetData appears in multiple contexts:
 *      a) Start of line: `    Controller.B2SSetData 179,0`
 *      b) After colon: `sw73.IsDropped = 0:Controller.B2SSetData 179,0`
 *      c) After Then: `If bFlag=0 Then Controller.B2SSetData bg_id,1`
 *    - Initial fix only handled (a), causing crashes for (b) and (c)
 *    - SOLUTION: Three separate patterns to catch all cases, each wrapping with
 *      `On Error Resume Next : <call> : On Error Goto 0`
 *
 * 4. GENERAL REGEX GOTCHAS
 *    - Always anchor patterns with `^` when matching line starts, or you'll match
 *      commented lines (e.g., `' For Each` would match without anchor)
 *    - Use `(?im)` for case-insensitive multiline, `(?ims)` adds single-line (. matches \n)
 *    - VBScript uses both `:` (statement separator) and newlines - handle both
 *    - Wine expects CRLF line endings - always normalize at the end
 *
 * 5. TESTING STRATEGY
 *    - Always pull patched_script.vbs from device to verify transformations
 *    - Script errors show line numbers in PATCHED script, not original
 *    - ANR (Application Not Responding) often means infinite loop from bad patch
 *    - "Description unavailable" errors usually mean undefined object/method
 *
 * 6. DTARRAY/STARRAY INITIALIZATION TIMING (PatchDTArray, PatchSTArray)
 *    - Some tables call DoDTAnim/DoSTAnim (via timers) BEFORE initializing DTArray/STArray
 *    - Example: Star Wars table calls DoSTAnim at line 3902 but STArray = Array(...)
 *      isn't defined until line 4148
 *    - Error manifests as "Description unavailable" at line where STArray(i).animate is accessed
 *    - SOLUTION: Add `If Not IsArray(DTArray/STArray) Then Exit Sub` guard at start of
 *      DoDTAnim/DoSTAnim functions. This safely exits if array isn't initialized yet.
 *    - This is safe for other tables because IsArray() returns True once array is defined.
 *
 * 7. GAME OF THRONES LE (Stern 2015) VPW 1.2 - PARSER "MISSING COMMA" (February 2026)
 *    Wine's parser (parser.y make_call_expression) fails with "Missing comma" for
 *    three distinct syntax patterns. All trigger the same E_FAIL in the bison grammar.
 *
 *    a) (new ClassName)(args) - 14+ instances (DropTarget, StandupTarget classes)
 *       Wine parser sees )(  as missing comma between arguments.
 *       Fix (PatchNewClassCall): Split into temp variable:
 *         Set DT7 = (new DropTarget)(a,b) → Set ssp_newobj = new DropTarget : Set DT7 = ssp_newobj(a,b)
 *       NOTE: Previously marked as "handled in compile.c" but parser fails BEFORE compiler runs.
 *
 *    b) arr(x)(y) chained indexing - 20+ instances (DSSources, PictoPops, MysteryAwards)
 *       Wine parser cannot handle consecutive )( in expression context.
 *       Fix (Patch2DArrayAccess): Use helper function:
 *         PictoPops(val)(3) → ssp_idx(PictoPops(val), 3)
 *       CAVEAT: Must coordinate with PatchSelectCaseArrayAccess - ssp_idx must be in
 *       its skip list or the Select Case patch incorrectly splits ssp_idx(arr(x), y).
 *
 *    c) SubName (expr)+rest - Wine greedily parses (expr) as Arguments
 *       Wine: AddScore (a*b)+c → AddScore(a*b) then sees +c as extra args → "Missing comma"
 *       Windows VBScript correctly parses (a*b)+c as a single expression argument.
 *       Fix (PatchAmbiguousCallParens): Wrap in extra parens:
 *         AddScore (a*b)+c → AddScore ((a*b)+c)
 *       GOTCHAS:
 *         - Must trim \r before detecting end of expression (CRLF line endings)
 *         - Must exclude VBScript keywords (if, while, and, or, not, etc.) from
 *           identifier matching or `if (expr)*rest then` gets incorrectly wrapped
 *
 *    d) (Not func)(arg) - Wine arity mismatch bug
 *       Fix (PatchParenthesizedNot): (not bInlanes)(0) → not bInlanes(0)
 *       GOTCHA: Regex must use (?i) flag - GOT script uses lowercase `not`,
 *       original pattern only matched capital `Not`.
 *
 * 8. DUPLICATE vpmInit Me (PatchDuplicateVpmInit)
 *    - Back to the Future (Data East 1990) has two vpmInit Me calls
 *    - The second call corrupts flipper callback state in Wine VBScript
 *    - Fix: Comment out all but the first vpmInit Me occurrence
 *
 * 9. DARK CHAOS (Apophis 2025) 2.0 - GetRef(name)(args) REGRESSION (February 2026)
 *    GetRef returns a function reference, and (args) invokes it. Wine's parser
 *    fails on the )( between them. Patch2DArrayAccess incorrectly wrapped these
 *    as ssp_idx(GetRef(name), args) which fails as a standalone statement because
 *    VBScript forbids func(a, b) syntax at statement level without Call keyword.
 *    Also, many GetRef patterns with nested args (e.g., GetRef(cb)(Array(x,y)))
 *    weren't matched by the regex at all, leaving raw )( for Wine.
 *    Fix (PatchGetRefCall): Split into temp variable (runs before Patch2DArrayAccess):
 *      GetRef(name)(args) → Set ssp_ref = GetRef(name) : ssp_ref(args)
 *    CAVEAT: Patch2DArrayAccess must also skip GetRef to avoid double-patching.
 *
 * Tables fixed in this session:
 * - Rollercoaster Tycoon (Stern 2002) - For Each, RenderingMode, TestVRonDT
 * - WoZ (Original 2018) - B2SSetData in multiple contexts
 * - Led Zeppelin Pinball 2.5 - WshShell
 * - Beavis and Butt-head Pinballed - vpmKeyDown (controller.vbs issue)
 * - Star Wars (Data East 1992) - DoSTAnim called before STArray initialization
 * - Game of Thrones LE (Stern 2015) VPW 1.2 - (new Class)(args), arr(x)(y),
 *   SubName (expr)+rest, (Not func)(arg) - all "Missing comma" parser failures
 * - Back to the Future (Data East 1990) - duplicate vpmInit Me breaks flippers
 * - Dark Chaos (Apophis 2025) 2.0 - GetRef(name)(args) regression from GOT fix
 * =============================================================================
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "simplescriptpatcher.h"
#include "scriptpatcher_internal.h"
#include "ui/tablepatches.h"
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cerrno>

// VPinballGetInternalPath is defined in VPinballLib.cpp
extern "C" const char* VPinballGetInternalPath();

// Static member initialization
std::string SimpleScriptPatcher::s_patchReport;
bool SimpleScriptPatcher::s_needsDropTargetClass = false;
bool SimpleScriptPatcher::s_needsStandupTargetClass = false;

void SimpleScriptPatcher::LogPatch(const std::string& description, int count) {
    if (count > 0) {
        s_patchReport += "  - " + description;
        if (count > 1) {
            s_patchReport += " (" + std::to_string(count) + " instances)";
        }
        s_patchReport += "\n";
        PLOGI.printf("SimpleScriptPatcher: %s (%d)", description.c_str(), count);
    }
}

std::string SimpleScriptPatcher::GetLastPatchReport() {
    return s_patchReport;
}

std::string SimpleScriptPatcher::StripBOM(const std::string& script) {
    if (script.length() >= 3 &&
        (unsigned char)script[0] == 0xEF &&
        (unsigned char)script[1] == 0xBB &&
        (unsigned char)script[2] == 0xBF) {
        PLOGI.printf("SimpleScriptPatcher: Stripped UTF-8 BOM");
        return script.substr(3);
    }
    if (script.length() >= 2 &&
        (unsigned char)script[0] == 0xFF &&
        (unsigned char)script[1] == 0xFE) {
        PLOGI.printf("SimpleScriptPatcher: Stripped UTF-16 LE BOM");
        return script.substr(2);
    }
    if (script.length() >= 2 &&
        (unsigned char)script[0] == 0xFE &&
        (unsigned char)script[1] == 0xFF) {
        PLOGI.printf("SimpleScriptPatcher: Stripped UTF-16 BE BOM");
        return script.substr(2);
    }
    return script;
}

// Normalize line endings to CRLF (Wine VBScript expects Windows line endings)
std::string SimpleScriptPatcher::NormalizeLineEndings(const std::string& script) {
    std::string result;
    result.reserve(script.size() + script.size() / 10);

    for (size_t i = 0; i < script.size(); ++i) {
        if (script[i] == '\r') {
            if (i + 1 < script.size() && script[i + 1] == '\n') {
                result += "\r\n";
                ++i;
            } else {
                result += "\r\n";
            }
        } else if (script[i] == '\n') {
            result += "\r\n";
        } else {
            result += script[i];
        }
    }
    return result;
}

// Sanitize non-ASCII characters that can cause Wine VBScript lexer issues
std::string SimpleScriptPatcher::SanitizeNonAscii(const std::string& script) {
    std::string result;
    result.reserve(script.size());
    int nonAsciiCount = 0;

    for (size_t i = 0; i < script.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(script[i]);
        if (c > 127) {
            result += ' ';
            nonAsciiCount++;
        } else {
            result += script[i];
        }
    }

    if (nonAsciiCount > 0) {
        PLOGI.printf("SimpleScriptPatcher: Sanitized %d non-ASCII bytes", nonAsciiCount);
    }
    return result;
}

// Dump script to file for debugging/error reporting
void SimpleScriptPatcher::DumpScript(const std::string& script, const std::string& filename) {
    const char* internalPath = VPinballGetInternalPath();
    if (internalPath) {
        std::string scriptPath = std::string(internalPath) + "/" + filename;
        FILE* debugFile = fopen(scriptPath.c_str(), "w");
        if (debugFile) {
            fwrite(script.c_str(), 1, script.length(), debugFile);
            fclose(debugFile);
            PLOGI.printf("SimpleScriptPatcher: Script saved to %s", scriptPath.c_str());
        } else {
            PLOGE.printf("SimpleScriptPatcher: Failed to write %s (errno=%d)", scriptPath.c_str(), errno);
        }
    } else {
        PLOGI.printf("SimpleScriptPatcher: Internal path not set - skipping script dump");
    }
}

// =============================================================================
// Bug 54177: Sub call fails when argument expression contains multiplication
// Pattern: SubName (expr)*number  ->  SubName number*(expr)
// =============================================================================
std::string SimpleScriptPatcher::PatchMultiplicationInSubCall(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Match: Callee (expr)*number at statement position
    // Transform to: Callee number*(expr)
    // Example: AddScore (Score+100)*2 -> AddScore 2*(Score+100)
    //
    // The callee permits dotted method chains with one level of inner-paren args, so
    // patterns like `DMDScene.GetImage("Dig" & i).SetBounds (i-20)*8, ...` (DUCKTALES,
    // FNAF) and `vpmTimer.AddTimer (ii-1)*200, ...` (Indy Hannibal) are matched. The
    // inner-paren content `[^()]*` does not handle nested parens — a chain whose
    // method args contain another `(...)` (e.g. Defender's `cstr(i)`) is left to
    // PatchAmbiguousCallParens.
    //
    // IMPORTANT: We must NOT match array accesses like BGArray(x,y) = value
    // The pattern uses [^),]+ to exclude expressions with commas (multi-index arrays).
    // The trailing-char guard below additionally blocks ( = ) which indicate array
    // access / assignment / outer expression. A trailing `,` is allowed because it is
    // the multi-arg sub-call form (which is the *common* Bug 54177 case).
    static const RE2 p(R"((?im)(^[ \t]*|:[ \t]*)(\w+(?:\.\w+(?:\([^()]*\))?)*)[ \t]+\(([^),]+)\)\s*\*\s*(\d+)([ \t]*[^\r\n]*))");

    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        std::string trailing = m[5];
        // Don't transform if followed by ( or = or )
        // These indicate array access/assignment or we're inside a larger expression
        if (!trailing.empty()) {
            size_t firstNonSpace = trailing.find_first_not_of(" \t");
            if (firstNonSpace != std::string::npos) {
                char nextChar = trailing[firstNonSpace];
                if (nextChar == '(' || nextChar == '=' || nextChar == ')') {
                    // This is likely an array access or inside expression - don't transform
                    return std::string(m[0]);  // Return original unchanged
                }
            }
        }
        count++;
        return m[1] + m[2] + " " + m[4] + "*(" + m[3] + ")" + trailing;
    });

    LogPatch("Bug 54177: Reordered multiplication in sub calls", count);
    return r;
}

// =============================================================================
// Bug 54291: UBound on Empty array causes issues with On Error Resume Next
// DISABLED: Human patches don't wrap UBound - leave as-is
// =============================================================================
std::string SimpleScriptPatcher::PatchUBound(const std::string& script) {
    // DISABLED - human patches don't wrap UBound, and wrapping may cause issues
    // The bug only affects UBound on truly Empty arrays with On Error Resume Next
    // Most scripts don't hit this edge case
    return script;
}

// =============================================================================
// Bug 55006/55037: REMOVED - Wine parser now handles these patterns natively
// Single-line If-Else and related patterns are now supported by the parser
// =============================================================================

// =============================================================================
// AlwaysOnTop Sub: Windows-only functionality using PowerShell
// Replace with stub on Android since it can't work and has complex syntax
// that Wine VBScript parser struggles with (line continuations + If-Else)
// =============================================================================
std::string SimpleScriptPatcher::PatchAlwaysOnTop(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Match Sub AlwaysOnTop with any content until End Sub
    // The Sub contains PowerShell commands that only work on Windows
    static const RE2 p(R"((?is)\bSub\s+AlwaysOnTop\s*\([^)]*\).*?End\s+Sub)");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        count++;
        // Simple stub - just empty Sub body
        return "Sub AlwaysOnTop(appName, regExpTitle, setOnTop)\r\nEnd Sub";
    });

    if (count > 0) {
        LogPatch("Replaced Windows-only AlwaysOnTop Sub with stub", count);
    }
    return r;
}

// =============================================================================
// WScript.Shell: Windows-only COM object
// Stub out entire subs that use WScript.Shell since they can't work on Android
// =============================================================================
std::string SimpleScriptPatcher::PatchWScriptShell(const std::string& script) {
    std::string r = script;
    int count = 0;

    // First, stub the entire Delay sub that uses WScript.Shell for delays
    // This pattern is common in VPX tables for timing: Sub Delay(seconds) ... wshShell ... End Sub
    static const RE2 delayPattern(R"((?is)(Sub\s+Delay\s*\(\s*\w+\s*\))[^\r\n]*.*?End\s+Sub)");
    r = RE2ReplaceWithCallback(r, delayPattern, [&count](const RE2Match& m) -> std::string {
        // Check if this sub uses wshShell
        std::string body = m[0];
        if (body.find("wshShell") != std::string::npos || body.find("WshShell") != std::string::npos ||
            body.find("WSHELL") != std::string::npos || body.find("WScript.Shell") != std::string::npos) {
            count++;
            PLOGI.printf("PatchWScriptShell: Stubbing Delay sub that uses WScript.Shell");
            // Return a stub that does nothing - Delay can't work on Android
            return std::string(m[1]) + "\r\n\t' Stubbed for Android - WScript.Shell delay not available\r\nEnd Sub";
        }
        return std::string(m[0]);
    });

    // Stub any sub that uses WshShell.RegWrite (common for UltraDMD settings)
    // Pattern: Sub XxxSettings... WshShell.RegWrite... End Sub
    static const RE2 regWriteSubPattern(R"((?is)(Sub\s+\w*(?:Settings|UltraDMD|DMD)\w*\s*(?:\([^)]*\))?)[^\r\n]*.*?End\s+Sub)");
    r = RE2ReplaceWithCallback(r, regWriteSubPattern, [&count](const RE2Match& m) -> std::string {
        std::string body = m[0];
        // Only stub if it uses WshShell for registry operations
        if ((body.find("WshShell") != std::string::npos || body.find("wshShell") != std::string::npos) &&
            body.find("RegWrite") != std::string::npos) {
            count++;
            PLOGI.printf("PatchWScriptShell: Stubbing sub with WshShell.RegWrite");
            return std::string(m[1]) + "\r\n\t' Stubbed for Android - WScript.Shell registry access not available\r\nEnd Sub";
        }
        return std::string(m[0]);
    });

    // Comment out standalone CreateObject("WScript.Shell") lines
    static const RE2 p(R"((?i)(Set\s+\w+\s*=\s*CreateObject\s*\(\s*"WScript\.Shell"\s*\)))");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        count++;
        return "' DISABLED ON ANDROID: " + std::string(m[1]);
    });

    // Comment out remaining wshShell method calls that weren't in stubbed subs
    // This catches any stray calls outside of recognized sub patterns
    static const RE2 wshCallPattern(R"((?i)(^[ \t]*)((?:wsh|WSH)Shell\.\w+[^\r\n]*))");
    r = RE2ReplaceWithCallback(r, wshCallPattern, [&count](const RE2Match& m) -> std::string {
        count++;
        return std::string(m[1]) + "' DISABLED ON ANDROID: " + std::string(m[2]);
    });

    if (count > 0) {
        LogPatch("Stubbed/disabled Windows-only WScript.Shell usage", count);
    }
    return r;
}

// =============================================================================
// (new ClassName)(args) pattern - Wine doesn't support chained call on new object
// Transform: Set x = (new Class)(a,b,c) -> Set x = new Class : x.Init a,b,c
// NOTE: This is now handled in Wine VBScript compiler (compile.c)
// The compiler detects (new ClassName)(args) and calls Init method explicitly
// =============================================================================
std::string SimpleScriptPatcher::PatchNewClassCall(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Wine VBScript parser cannot handle (new ClassName)(args) syntax
    // which calls the default member of a newly created object.
    // The PARSER fails with "Missing comma" before the compiler ever runs.
    // Fix: Split into temp variable creation + default member call via temp var.
    // Set var = (new ClassName)(args)
    //   → Set ssp_newobj = new ClassName : Set var = ssp_newobj(args)
    // ssp_newobj(args) calls the default member (typically Init) which returns Me.
    // Dim ssp_newobj is declared in InjectHelpers.
    static const RE2 p(R"((?im)^([ \t]*)(Set\s+\w+\s*=\s*)\(new\s+(\w+)\)\(([^)\r\n]*)\)\s*(?:'[^\r\n]*)?\r?$)");

    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        std::string indent = m[1];
        std::string setVarEq = m[2];  // "Set DT7 = " etc.
        std::string className = m[3]; // "DropTarget"
        std::string args = m[4];      // "target7, target7a, ..."

        // Skip when args are empty: (new Class)() is just object creation with
        // grouping parens, NOT a default member call. Wine handles this fine.
        // Transforming to ssp_newobj() would incorrectly call a default member
        // that may not exist, breaking object initialization.
        if (args.empty() || args.find_first_not_of(" \t") == std::string::npos) {
            return std::string(m[0]);  // Return unchanged
        }

        count++;
        PLOGI.printf("PatchNewClassCall: (new %s)(%s) -> temp var",
            className.c_str(), args.substr(0, 50).c_str());
        return indent + "Set ssp_newobj = new " + className + " : " +
               setVarEq + "ssp_newobj(" + args + ")";
    });

    if (count > 0) {
        LogPatch("Fixed (new ClassName)(args) syntax for Wine parser", count);
    }
    return r;
}

// =============================================================================
// Bug 56480: Line continuation underscore issues
// Fix: word _\n.property -> word. _\n property
// =============================================================================
std::string SimpleScriptPatcher::PatchLineContinuation(const std::string& script) {
    std::string r = script;

    // Line continuation before dot - move dot before the continuation
    static const RE2 p(R"((?i)(\w|\))\s+_\s*\r?\n\s*\.)");
    std::string before = r;
    r = RE2Replace(r, p, "\\1. _\n");

    int count = (r != before) ? 1 : 0;
    LogPatch("Bug 56480: Fixed line continuation before dot", count);
    return r;
}

// =============================================================================
// Bug 58051: Direct access to Dictionary.Keys or Dictionary.Items fails
// Pattern: dict.Keys(idx) -> temp array approach
// =============================================================================
std::string SimpleScriptPatcher::PatchDictionaryAccess(const std::string& script) {
    std::string r = script;
    int count = 0;

    // This is a complex pattern - for now, just flag it
    // Real fix would require injecting helper functions
    static const RE2 p(R"((?i)\.Keys\s*\(\s*\d+\s*\)|\.Items\s*\(\s*\d+\s*\))");
    if (RE2Search(r, p)) {
        PLOGI.printf("SimpleScriptPatcher: WARNING - Script uses direct Dictionary.Keys/Items indexing (Bug 58051)");
        // TODO: Transform to use temp array
    }

    return r;
}

// =============================================================================
// Bug 58056: Directly indexing Split() return value yields Empty
// Pattern: Split(str, delim)(idx) -> temp = Split(...) : result = temp(idx)
// =============================================================================
std::string SimpleScriptPatcher::PatchSplitIndexing(const std::string& script) {
    std::string r = script;

    // Detect the pattern - transformation would require context analysis
    static const RE2 p(R"((?i)\bSplit\s*\([^)]+\)\s*\(\s*\d+\s*\))");
    if (RE2Search(r, p)) {
        PLOGI.printf("SimpleScriptPatcher: WARNING - Script uses direct Split() indexing (Bug 58056)");
        // TODO: Transform to use temp variable
    }

    return r;
}

// =============================================================================
// Bug 55093: Boolean Not without parentheses
// Pattern: <> Not expr -> <> (Not expr)
// Pattern: = Not expr -> = (Not expr)
// =============================================================================
std::string SimpleScriptPatcher::PatchBooleanNot(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Fix: expr <> Not other -> expr <> (Not other)
    static const RE2 p1(R"((?i)(<>|=)\s*(Not\s+\w+))");
    r = RE2ReplaceWithCallback(r, p1, [&count](const RE2Match& m) -> std::string {
        count++;
        return m[1] + " (" + m[2] + ")";
    });

    if (count > 0) {
        LogPatch("Bug 55093: Added parentheses around Not expressions", count);
    }
    return r;
}

// =============================================================================
// Double dot fix - common typo in VBS scripts
// Pattern: obj..method -> obj.method
// =============================================================================
std::string SimpleScriptPatcher::PatchDoubleDot(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Replace double dots with single dot
    static const RE2 p(R"(\.\.)");
    std::string before = r;
    r = RE2Replace(r, p, ".");

    // Count how many replacements
    size_t pos1 = 0, pos2 = 0;
    while ((pos1 = before.find("..", pos1)) != std::string::npos) { count++; pos1 += 2; }

    if (count > 0) {
        LogPatch("Fixed double dots (..) -> (.)", count);
    }
    return r;
}

// =============================================================================
// GLF Boolean Array Bug - Wine VBScript corrupts VT_BOOL in SAFEARRAY
// Pattern: Array(..., True) -> Array(..., 1)
//          If m_isGetRef = True Then -> If m_isGetRef <> 0 Then
// =============================================================================
std::string SimpleScriptPatcher::PatchGlfBooleanArray(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Wine SAFEARRAY bug: array element values get corrupted to 0/empty after function return.
    // Workaround: Use global variables to pass BOTH the value and isGetRef flag, bypassing the array entirely.
    //
    // Strategy:
    // 1. Inject global variable declarations: Dim glf_last_value, glf_last_isGetRef
    // 2. In Glf_ParseInput, set BOTH globals BEFORE the array return
    // 3. Change m_value = parsedInput(0) and m_isGetRef = parsedInput(2) to use globals

    // Step 1: Inject global variables after a known global declaration in GLF scripts
    static const RE2 p_inject(R"((?i)(Dim\s+glf_funcRefMap\b[^\r\n]*))");
    r = RE2ReplaceWithCallback(r, p_inject, [&count](const RE2Match& m) -> std::string {
        count++;
        PLOGI.printf("GLF patch: Injecting globals after: %s", m[1].substr(0, 50).c_str());
        return std::string(m[1]) + "\r\nDim glf_last_value, glf_last_isGetRef ' Wine SAFEARRAY bug workaround";
    });

    // Step 2: Set BOTH globals before Glf_ParseInput returns with Array(funcRef, value, 1/True)
    // Match: Glf_ParseInput = Array(X, Y, 1) where X is the funcRef we need to capture
    // First pattern: Array(glf_funcRefMap(CStr(value)), value, 1)
    static const RE2 p_set1(R"((?i)([\t ]*)(Glf_ParseInput\s*=\s*Array\s*\()([^,]+)(,\s*)([^,]+)(,\s*)(1|True)(\s*\)))");
    r = RE2ReplaceWithCallback(r, p_set1, [&count](const RE2Match& m) -> std::string {
        count++;
        std::string indent = m[1];
        std::string prefix = m[2];  // "Glf_ParseInput = Array("
        std::string funcRef = m[3]; // first arg (funcRef)
        std::string comma1 = m[4];  // ", "
        std::string value = m[5];   // second arg (value)
        std::string comma2 = m[6];  // ", "
        std::string flag = m[7];    // "1" or "True"
        std::string suffix = m[8];  // ")"
        PLOGI.printf("GLF patch: Setting globals before Array, funcRef=%s", funcRef.substr(0, 40).c_str());
        // Set both globals before the Array line
        return indent + "glf_last_value = " + funcRef + " : glf_last_isGetRef = True\r\n" +
               indent + prefix + funcRef + comma1 + value + comma2 + flag + suffix;
    });

    // Step 3: Change m_value = parsedInput(0) to use the global
    static const RE2 p_use_value(R"((?i)m_value\s*=\s*parsedInput\s*\(\s*0\s*\))");
    r = RE2ReplaceWithCallback(r, p_use_value, [&count](const RE2Match& m) -> std::string {
        count++;
        PLOGI.printf("GLF patch: Replacing parsedInput(0) with global");
        return "m_value = glf_last_value";
    });

    // Step 4: Change m_isGetRef = parsedInput(2) to use the global
    static const RE2 p_use_flag(R"((?i)m_isGetRef\s*=\s*parsedInput\s*\(\s*2\s*\))");
    r = RE2ReplaceWithCallback(r, p_use_flag, [&count](const RE2Match& m) -> std::string {
        count++;
        PLOGI.printf("GLF patch: Replacing parsedInput(2) with global");
        return "m_isGetRef = glf_last_isGetRef";
    });

    if (count > 0) {
        LogPatch("GLF Boolean Array Bug: Use global variable to bypass SAFEARRAY corruption", count);
    }
    return r;
}


// =============================================================================
// Inline statement fix: REMOVED - Wine parser now handles these patterns natively
// =============================================================================

// =============================================================================
// GetRef Call: GetRef(name)(args) fails in Wine VBScript parser
// Wine's parser treats )( as "Missing comma". This is distinct from arr(x)(y)
// because GetRef returns a function reference and (args) invokes it.
// Additionally, ssp_idx(GetRef(...), args) fails when used as a standalone
// statement because VBScript forbids func(a, b) syntax at statement level.
// Fix: Split into temp variable + call to avoid )( pattern:
//   GetRef(name)(args)       → Set ssp_ref = GetRef(name) : ssp_ref(args)
//   x = GetRef(name)(args)   → Set ssp_ref = GetRef(name) : x = ssp_ref(args)
//   Set x = GetRef(n)(args)  → Set ssp_ref = GetRef(n) : Set x = ssp_ref(args)
// Uses balanced parenthesis tracking to handle arbitrary nesting depth in args.
// Must run BEFORE Patch2DArrayAccess to prevent incorrect ssp_idx wrapping.
// =============================================================================
std::string SimpleScriptPatcher::PatchGetRefCall(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Quick check - no )( means no chained calls to fix
    if (r.find(")(") == std::string::npos)
        return r;

    // Also need GetRef somewhere in the script (case-insensitive)
    bool hasGetRef = false;
    for (size_t i = 0; i + 5 < r.size() && !hasGetRef; i++) {
        if (tolower(r[i]) == 'g' && tolower(r[i+1]) == 'e' && tolower(r[i+2]) == 't' &&
            tolower(r[i+3]) == 'r' && tolower(r[i+4]) == 'e' && tolower(r[i+5]) == 'f') {
            hasGetRef = true;
        }
    }
    if (!hasGetRef)
        return r;

    // Process line by line
    std::string result;
    result.reserve(r.size() + 2048);
    size_t lineStart = 0;

    while (lineStart < r.size()) {
        size_t lineEnd = r.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = r.size();
        std::string line = r.substr(lineStart, lineEnd - lineStart);

        // Skip comment lines
        size_t firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace == std::string::npos || line[firstNonSpace] == '\'') {
            result += line;
            if (lineEnd < r.size()) result += '\n';
            lineStart = lineEnd + 1;
            continue;
        }

        // Search for GetRef(...)(...) pattern using balanced paren tracking
        size_t searchPos = 0;
        while (searchPos + 5 < line.size()) {
            // Find "GetRef" case-insensitive
            size_t grPos = std::string::npos;
            for (size_t i = searchPos; i + 5 < line.size(); i++) {
                if (tolower(line[i]) == 'g' && tolower(line[i+1]) == 'e' &&
                    tolower(line[i+2]) == 't' && tolower(line[i+3]) == 'r' &&
                    tolower(line[i+4]) == 'e' && tolower(line[i+5]) == 'f') {
                    // Check word boundaries
                    if (i > 0 && (isalnum(line[i-1]) || line[i-1] == '_'))
                        continue;
                    if (i + 6 < line.size() && (isalnum(line[i+6]) || line[i+6] == '_'))
                        continue;
                    grPos = i;
                    break;
                }
            }
            if (grPos == std::string::npos)
                break;

            size_t afterGetRef = grPos + 6;
            if (afterGetRef >= line.size() || line[afterGetRef] != '(') {
                searchPos = afterGetRef;
                continue;
            }

            // Find matching ) for first ( using balanced paren tracking
            int depth = 1;
            size_t ci = afterGetRef + 1;
            while (ci < line.size() && depth > 0) {
                if (line[ci] == '(') depth++;
                else if (line[ci] == ')') depth--;
                ci++;
            }
            if (depth != 0) {
                searchPos = afterGetRef + 1;
                continue;
            }
            size_t firstClosePos = ci - 1;

            // Check if immediately followed by (
            if (ci >= line.size() || line[ci] != '(') {
                searchPos = ci;
                continue;
            }

            // Find matching ) for second ( using balanced paren tracking
            size_t secondOpenPos = ci;
            depth = 1;
            ci = secondOpenPos + 1;
            while (ci < line.size() && depth > 0) {
                if (line[ci] == '(') depth++;
                else if (line[ci] == ')') depth--;
                ci++;
            }
            if (depth != 0) {
                searchPos = secondOpenPos + 1;
                continue;
            }
            size_t secondClosePos = ci - 1;

            // Extract parts
            std::string getrefArg = line.substr(afterGetRef + 1, firstClosePos - afterGetRef - 1);
            std::string callArgs = line.substr(secondOpenPos + 1, secondClosePos - secondOpenPos - 1);

            // Get the indent
            size_t indentEnd = line.find_first_not_of(" \t");
            std::string indent = (indentEnd != std::string::npos) ? line.substr(0, indentEnd) : "";

            // Replace GetRef(arg1)(arg2) with ssp_ref(arg2) in the line,
            // then prepend "Set ssp_ref = GetRef(arg1) : " after indent
            std::string before = line.substr(0, grPos);
            std::string after = line.substr(secondClosePos + 1);
            std::string restOfLine = before.substr(indent.size()) + "ssp_ref(" + callArgs + ")" + after;
            line = indent + "Set ssp_ref = GetRef(" + getrefArg + ") : " + restOfLine;

            count++;
            PLOGI.printf("PatchGetRefCall: GetRef(%s)(%s) -> temp var",
                getrefArg.substr(0, 40).c_str(), callArgs.substr(0, 40).c_str());

            // One replacement per line to avoid position tracking issues
            break;
        }

        result += line;
        if (lineEnd < r.size()) result += '\n';
        lineStart = lineEnd + 1;
    }

    if (count > 0) {
        LogPatch("Fixed GetRef(name)(args) chained call for Wine parser using temp var", count);
    }

    return result;
}

// =============================================================================
// Chained Parentheses: arr(x)(y) fails in Wine VBScript parser
// Wine's parser treats )( as "missing comma" and sets E_FAIL.
// Fix: Replace arr(expr)(index) with ssp_idx(arr(expr), index)
// where ssp_idx is a helper: Function ssp_idx(a, i) : ssp_idx = a(i) : End Function
// This avoids the )( pattern by evaluating arr(expr) as a function argument first.
// =============================================================================
std::string SimpleScriptPatcher::Patch2DArrayAccess(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Quick check - if no )( in script, nothing to do
    if (r.find(")(") == std::string::npos)
        return r;

    // Match: identifier(args_with_optional_nesting)(simple_args)
    // Allows one level of nested parentheses in first args
    // e.g., MysteryAwards(MysteryVals(i))(0) -> ssp_idx(MysteryAwards(MysteryVals(i)), 0)
    // e.g., DSSources(iii)(0) -> ssp_idx(DSSources(iii), 0)
    // e.g., PictoPops(i)(2) -> ssp_idx(PictoPops(i), 2)
    // Also handles Split(str, delim)(0) and similar function-return indexing
    static const RE2 p(R"((\w+)\(([^()]*(?:\([^()]*\))?[^()]*)\)\(([^()]*)\))");

    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        std::string name = m[1];
        std::string args1 = m[2];
        std::string args2 = m[3];

        // Skip GetRef - it returns a function reference, not an array.
        // GetRef(name)(args) is handled by PatchGetRefCall which runs first.
        std::string lowerName = name;
        for (auto& c : lowerName) c = tolower(c);
        if (lowerName == "getref") {
            return std::string(m[0]);  // Return unchanged
        }

        count++;
        return "ssp_idx(" + name + "(" + args1 + "), " + args2 + ")";
    });

    if (count > 0) {
        LogPatch("Fixed chained parentheses arr(x)(y) for Wine parser using ssp_idx helper", count);
    }

    // Check for remaining )( patterns (deeper nesting or unusual syntax)
    if (r.find(")(") != std::string::npos) {
        PLOGI.printf("SimpleScriptPatcher: WARNING - Residual )( patterns remain after Patch2DArrayAccess");
    }

    return r;
}

// =============================================================================
// Ambiguous Call Parens: Wine parser fails on SubName (expr)OPERATOR rest
// Wine greedily parses (expr) as the call's Arguments, leaving OPERATOR rest
// as dangling ArgumentList_opt → "Missing comma" error.
// Windows VBScript treats (expr)OPERATOR rest as a single expression argument.
// Fix: Add extra parentheses to disambiguate:
//   AddScore (a*b)+c           → AddScore ((a*b)+c)
//   .Method ((i-1)*25)+14,x,y  → .Method (((i-1)*25)+14),x,y
// =============================================================================
std::string SimpleScriptPatcher::PatchAmbiguousCallParens(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Quick check: look for )OPERATOR pattern (excluding inside strings/comments)
    bool hasPattern = false;
    for (size_t p = 1; p < r.size(); p++) {
        if (r[p-1] == ')' && (r[p] == '+' || r[p] == '-' || r[p] == '&' ||
            r[p] == '*' || r[p] == '/' || r[p] == '\\' || r[p] == '^')) {
            hasPattern = true;
            break;
        }
    }
    if (!hasPattern)
        return r;

    // Process line by line
    std::string result;
    result.reserve(r.size() + 256);
    size_t lineStart = 0;

    while (lineStart < r.size()) {
        // Find end of line
        size_t lineEnd = r.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = r.size();
        std::string line = r.substr(lineStart, lineEnd - lineStart);

        // Skip comment lines
        size_t firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos && line[firstNonSpace] == '\'') {
            result += line;
            if (lineEnd < r.size()) result += '\n';
            lineStart = lineEnd + 1;
            continue;
        }

        // Check if line has )OPERATOR pattern
        bool lineNeedsFix = false;
        for (size_t p = 1; p < line.size(); p++) {
            if (line[p-1] == ')' && (line[p] == '+' || line[p] == '-' || line[p] == '&' ||
                line[p] == '*' || line[p] == '/' || line[p] == '\\' || line[p] == '^')) {
                lineNeedsFix = true;
                break;
            }
        }

        if (lineNeedsFix) {
            // Find the )OPERATOR position
            for (size_t p = 1; p < line.size(); p++) {
                char op = line[p];
                if (line[p-1] != ')' || (op != '+' && op != '-' && op != '&' &&
                    op != '*' && op != '/' && op != '\\' && op != '^'))
                    continue;

                size_t closeParenPos = p - 1;

                // Walk backward to find the matching (
                int depth = 0;
                size_t openParenPos = std::string::npos;
                for (size_t j = closeParenPos; ; ) {
                    if (line[j] == ')') depth++;
                    else if (line[j] == '(') {
                        depth--;
                        if (depth == 0) {
                            openParenPos = j;
                            break;
                        }
                    }
                    if (j == 0) break;
                    j--;
                }

                if (openParenPos == std::string::npos) continue;

                // Check if before openParen there's a space + identifier/member
                if (openParenPos == 0) continue;
                size_t beforeParen = openParenPos - 1;
                // Must have at least one space before (
                if (line[beforeParen] != ' ' && line[beforeParen] != '\t') continue;
                // Before the space, must be an identifier char
                while (beforeParen > 0 && (line[beforeParen] == ' ' || line[beforeParen] == '\t'))
                    beforeParen--;
                if (!isalnum(line[beforeParen]) && line[beforeParen] != '_')
                    continue;

                // Extract the identifier name to check against keywords
                size_t identEnd = beforeParen + 1;
                size_t identStart = beforeParen;
                while (identStart > 0 && (isalnum(line[identStart-1]) || line[identStart-1] == '_'))
                    identStart--;
                std::string ident = line.substr(identStart, identEnd - identStart);
                // Convert to lowercase for comparison
                std::string identLower = ident;
                for (auto& c : identLower) c = tolower(c);
                // Skip VBScript keywords - these use (expr) for grouping, not function calls
                if (identLower == "if" || identLower == "elseif" || identLower == "while" ||
                    identLower == "until" || identLower == "and" || identLower == "or" ||
                    identLower == "not" || identLower == "xor" || identLower == "mod" ||
                    identLower == "is" || identLower == "then" || identLower == "case" ||
                    identLower == "to" || identLower == "step" || identLower == "eqv" ||
                    identLower == "imp" || identLower == "like" || identLower == "select" ||
                    identLower == "return" || identLower == "wend" || identLower == "loop" ||
                    identLower == "do" || identLower == "for" || identLower == "each" ||
                    identLower == "dim" || identLower == "redim" || identLower == "const" ||
                    identLower == "set" || identLower == "let" || identLower == "end" ||
                    identLower == "sub" || identLower == "function" || identLower == "class" ||
                    identLower == "property" || identLower == "with" || identLower == "new" ||
                    identLower == "call" || identLower == "exit" || identLower == "on" ||
                    identLower == "typeof" || identLower == "cbool" || identLower == "cbyte" ||
                    identLower == "cint" || identLower == "clng" || identLower == "csng" ||
                    identLower == "cdbl" || identLower == "cstr" || identLower == "cdate")
                    continue;

                // This is the ambiguous pattern! Now find where the first argument ends.
                // It ends at a comma at depth 0 (after the operator expression) or at
                // end of statement (newline, colon, or line end).
                size_t exprStart = openParenPos; // the (
                size_t exprEnd = p; // start of operator
                int depth2 = 0;
                for (size_t k = p; k < line.size(); k++) {
                    char c = line[k];
                    if (c == '(') depth2++;
                    else if (c == ')') depth2--;
                    else if (c == ',' && depth2 == 0) {
                        exprEnd = k;
                        break;
                    }
                    else if (c == ':' && depth2 == 0) {
                        exprEnd = k;
                        break;
                    }
                    else if (c == '\'' && depth2 == 0) {
                        exprEnd = k;
                        break;
                    }
                    exprEnd = k + 1;
                }

                // Trim trailing whitespace (including \r) from the expression
                while (exprEnd > p && (line[exprEnd-1] == ' ' || line[exprEnd-1] == '\t' || line[exprEnd-1] == '\r'))
                    exprEnd--;

                // Insert extra parens: ( before the original ( and ) at exprEnd
                // Before: ... SubName (expr)+rest...
                // After:  ... SubName ((expr)+rest)...
                line.insert(exprEnd, ")");
                line.insert(openParenPos + 1, "(");
                count++;
                PLOGI.printf("PatchAmbiguousCallParens: Fixed at line position %zu: %s",
                    openParenPos, line.substr(openParenPos, 60).c_str());
                break; // Only fix first occurrence per line to avoid offset issues
            }
        }

        result += line;
        if (lineEnd < r.size()) result += '\n';
        lineStart = lineEnd + 1;
    }

    if (count > 0) {
        LogPatch("Fixed ambiguous call parentheses (Wine parser 'Missing comma')", count);
    }
    return result;
}

// =============================================================================
// DTArray patterns - Drop Target array compatibility
// Implements the human patching approach:
// 1. Inject DropTarget class
// 2. Convert DT1 = Array(...) to Set DT1 = (new DropTarget)(...)
// 3. Convert DTArray(i)(n) to DTArray(i).property
// =============================================================================
std::string SimpleScriptPatcher::PatchDTArray(const std::string& script) {
    // Check if script uses DTArray with chained access (case-insensitive — VBScript
    // identifiers are case-insensitive and AC-DC LUCI's BallSearch uses lowercase
    // `DTarray(i)(2)` while declaring `Dim DTArray` with capital A).
    static const RE2 dtArrayMention(R"((?i)\bDTArray\b)");
    if (!RE2Search(script, dtArrayMention)) {
        return script;  // No DTArray usage
    }

    // Check for chained access pattern DTArray(i)(n)
    static const RE2 checkPattern(R"((?i)DTArray\s*\([^)]+\)\s*\()");
    if (!RE2Search(script, checkPattern)) {
        return script;  // No chained access pattern
    }

    PLOGI.printf("SimpleScriptPatcher: Detected DTArray(i)(n) pattern - applying class injection");

    std::string r = script;
    int totalCount = 0;

    // Step 1a: Convert DT variable Array() initialization to class instantiation
    // Pattern: DT1 = Array(primary, secondary, prim, sw, animate[, isDropped])
    // 5-arg: Set DT1 = (new DropTarget)(args) - uses default init()
    // 6-arg: Set DT1 = (New DropTarget).Init6(args) - uses Init6() (no empty parens!)
    // Wine compiler fix in compile.c handles (new ClassName)(args) by calling Init explicitly
    // Match DT followed by digits and optional letters (DT1, DT54a) - NOT DTArray!
    // DT\d+\w* matches DT1, DT54, DT54a but not DTArray (which has no digit after DT)
    static const RE2 arrayInit(R"((?im)(^[ \t]*)(DT\d+\w*)\s*=\s*Array\s*\(([^)]+)\))");
    r = RE2ReplaceWithCallback(r, arrayInit, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        std::string args = m[3];
        // Count commas to determine if 5 or 6 args
        int commaCount = 0;
        for (char c : args) {
            if (c == ',') commaCount++;
        }
        if (commaCount >= 5) {
            // 6+ args - use Init6 (no empty parens - that would call default init with 0 args!)
            return m[1] + "Set " + m[2] + " = (New DropTarget).Init6(" + args + ")";
        }
        // 5 args (4 commas) - use default init via (new Class)(args)
        return m[1] + "Set " + m[2] + " = (new DropTarget)(" + args + ")";
    });

    // Step 1b: Convert inline Array() calls inside DTArray = Array(...) initialization
    // Some tables initialize DTArray directly with inline Array() instead of named DT variables:
    //   DTArray = Array(Array(dt1,dt1a,dt1p,1,0), Array(dt2,dt2a,dt2p,2,0), ...)
    // Convert to:
    //   DTArray = Array((new DropTarget)(dt1,dt1a,dt1p,1,0), (new DropTarget)(dt2,dt2a,dt2p,2,0), ...)
    // IMPORTANT: Only convert Array() calls that are inside DTArray = Array(...) to avoid breaking other code
    static const RE2 dtArrayInit(R"((?im)(^[ \t]*DTArray\s*=\s*Array\s*\()([^;'\r\n]+)(\)\s*$))");
    r = RE2ReplaceWithCallback(r, dtArrayInit, [&totalCount](const RE2Match& m) -> std::string {
        std::string prefix = m[1];  // "DTArray = Array("
        std::string contents = m[2]; // inner contents
        std::string suffix = m[3];  // ")"

        // Convert Array(...) to (new DropTarget)(...) within the contents
        static const RE2 innerArray(R"(Array\s*\(([^()]+)\))");
        std::string converted = RE2ReplaceWithCallback(contents, innerArray, [&totalCount](const RE2Match& inner) -> std::string {
            std::string args = inner[1];
            int commaCount = 0;
            for (char c : args) {
                if (c == ',') commaCount++;
            }
            // DT elements have 5 args (4 commas) or 6 args (5 commas) — 6th is isDropped
            if (commaCount == 4) {
                totalCount++;
                return "(new DropTarget)(" + args + ")";
            } else if (commaCount == 5) {
                totalCount++;
                // No empty parens - that would call default init with 0 args!
                return "(New DropTarget).Init6(" + args + ")";
            }
            return std::string(inner[0]);
        });

        if (converted != contents) {
            PLOGI.printf("SimpleScriptPatcher: Converted inline Array() in DTArray initialization");
        }
        return prefix + converted + suffix;
    });

    // Step 2: Convert DTArray(i)(n) to DTArray(i).property in a single pass.
    // Case-insensitive (?i) — VBScript identifiers are case-insensitive; some scripts
    // declare `Dim DTArray` but reference it as `DTarray` at call sites (AC-DC LUCI
    // BallSearch is the canonical example). Without (?i) we'd patch the init but miss
    // those call sites, leaving DropTarget objects being indexed → ARITY_MISMATCH on
    // their default `init(...)` member.
    // Index mapping: 0=primary, 1=secondary, 2=prim, 3=sw, 4=animate, 5=isDropped
    static const char* kDTProps[] = { ".primary", ".secondary", ".prim", ".sw", ".animate", ".isDropped" };
    static const RE2 dtIdxAll(R"((?i)DTArray\s*\(\s*([^)]+)\s*\)\s*\(\s*([0-5])\s*\))");
    r = RE2ReplaceWithCallback(r, dtIdxAll, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        int idx = m[2][0] - '0';
        return "DTArray(" + m[1] + ")" + kDTProps[idx];
    });

    if (totalCount > 0) {
        LogPatch("DTArray: Converted to DropTarget class pattern", totalCount);
        s_needsDropTargetClass = true;

        // Add IsArray guard to DoDTAnim function
        // This prevents crash when DoDTAnim is called before DTArray is initialized
        // Pattern: Sub DoDTAnim() -> Sub DoDTAnim() : If Not IsArray(DTArray) Then Exit Sub
        static const RE2 doDTAnimPattern(R"((?im)(Sub\s+DoDTAnim\s*\(\s*\))(\s*\r?\n))");
        std::string before = r;
        r = RE2Replace(r, doDTAnimPattern, "\\1\\2\tIf Not IsArray(DTArray) Then Exit Sub\\2");
        if (r != before) {
            LogPatch("Added IsArray guard to DoDTAnim", 1);
        }
    }

    return r;
}

// =============================================================================
// STArray patterns - Standup Target array compatibility
// Implements the human patching approach:
// 1. Inject StandupTarget class
// 2. Convert ST1 = Array(...) to Set ST1 = (new StandupTarget)(...)
// 3. Convert STArray(i)(n) to STArray(i).property
// =============================================================================
std::string SimpleScriptPatcher::PatchSTArray(const std::string& script) {
    // Check if script uses STArray with chained access (case-insensitive — same
    // mixed-case-call-site issue as DTArray, see PatchDTArray for context).
    static const RE2 stArrayMention(R"((?i)\bSTArray\b)");
    if (!RE2Search(script, stArrayMention)) {
        return script;  // No STArray usage
    }

    // Check for chained access pattern STArray(i)(n)
    static const RE2 checkPattern(R"((?i)STArray\s*\([^)]+\)\s*\()");
    if (!RE2Search(script, checkPattern)) {
        return script;  // No chained access pattern
    }

    PLOGI.printf("SimpleScriptPatcher: Detected STArray(i)(n) pattern - applying class injection");

    std::string r = script;
    int totalCount = 0;

    // Step 1a: Convert ST variable Array() initialization to class instantiation
    // Pattern: ST12 = Array(primary, prim, sw, animate[, id])
    // 4-arg: Set ST12 = (new StandupTarget)(args) - uses default init()
    // 5-arg: Set ST12 = (New StandupTarget).Init5(args) - uses Init5() (no empty parens!)
    // Match ST followed by digits and optional letters (ST12, ST18a, ST18b) - NOT STArray!
    static const RE2 arrayInit(R"((?im)(^[ \t]*)(ST\d+\w*)\s*=\s*Array\s*\(([^)]+)\))");
    r = RE2ReplaceWithCallback(r, arrayInit, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        std::string args = m[3];
        // Count commas to determine if 4 or 5 args
        int commaCount = 0;
        for (char c : args) {
            if (c == ',') commaCount++;
        }
        if (commaCount >= 4) {
            // 5+ args - use Init5 (no empty parens - that would call default init with 0 args!)
            return m[1] + "Set " + m[2] + " = (New StandupTarget).Init5(" + args + ")";
        }
        // 4 args (3 commas) - use default init via (new Class)(args)
        return m[1] + "Set " + m[2] + " = (new StandupTarget)(" + args + ")";
    });

    // Step 1b: Convert inline Array() calls inside STArray = Array(...) initialization
    // Some tables initialize STArray directly with inline Array() instead of named ST variables:
    //   STArray = Array(Array(sw18,sw18p,18,0), Array(sw19,sw19p,19,0), ...)
    // Convert to:
    //   STArray = Array((new StandupTarget)(sw18,sw18p,18,0), (new StandupTarget)(sw19,sw19p,19,0), ...)
    // IMPORTANT: Only convert Array() calls that are inside STArray = Array(...) to avoid breaking other code
    static const RE2 stArrayInit(R"((?im)(^[ \t]*STArray\s*=\s*Array\s*\()([^;'\r\n]+)(\)\s*$))");
    r = RE2ReplaceWithCallback(r, stArrayInit, [&totalCount](const RE2Match& m) -> std::string {
        std::string prefix = m[1];  // "STArray = Array("
        std::string contents = m[2]; // inner contents
        std::string suffix = m[3];  // ")"

        // Convert Array(...) to (new StandupTarget)(...) within the contents
        static const RE2 innerArray(R"(Array\s*\(([^()]+)\))");
        std::string converted = RE2ReplaceWithCallback(contents, innerArray, [&totalCount](const RE2Match& inner) -> std::string {
            std::string args = inner[1];
            int commaCount = 0;
            for (char c : args) {
                if (c == ',') commaCount++;
            }
            // ST elements have 4 args (3 commas) or 5 args (4 commas)
            if (commaCount == 3) {
                totalCount++;
                return "(new StandupTarget)(" + args + ")";
            } else if (commaCount == 4) {
                totalCount++;
                // No empty parens - that would call default init with 0 args!
                return "(New StandupTarget).Init5(" + args + ")";
            }
            return std::string(inner[0]);
        });

        if (converted != contents) {
            PLOGI.printf("SimpleScriptPatcher: Converted inline Array() in STArray initialization");
        }
        return prefix + converted + suffix;
    });

    // Step 2: Convert STArray(i)(n) to STArray(i).property in a single pass.
    // Case-insensitive (?i) — see PatchDTArray for the AC-DC LUCI mixed-case background.
    // Index mapping: 0=primary, 1=prim, 2=sw, 3=animate, 4=id (for 5-element arrays)
    static const char* kSTProps[] = { ".primary", ".prim", ".sw", ".animate", ".id" };
    static const RE2 stIdxAll(R"((?i)STArray\s*\(\s*([^)]+)\s*\)\s*\(\s*([0-4])\s*\))");
    r = RE2ReplaceWithCallback(r, stIdxAll, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        int idx = m[2][0] - '0';
        return "STArray(" + m[1] + ")" + kSTProps[idx];
    });

    if (totalCount > 0) {
        LogPatch("STArray: Converted to StandupTarget class pattern", totalCount);
        s_needsStandupTargetClass = true;

        // Add IsArray guard to DoSTAnim function
        // This prevents crash when DoSTAnim is called before STArray is initialized
        // (e.g., Star Wars table calls DoSTAnim at line 3902 before STArray = Array(...) at line 4148)
        // Pattern: Sub DoSTAnim() -> Sub DoSTAnim() : If Not IsArray(STArray) Then Exit Sub
        static const RE2 doSTAnimPattern(R"((?im)(Sub\s+DoSTAnim\s*\(\s*\))(\s*\r?\n))");
        std::string before = r;
        r = RE2Replace(r, doSTAnimPattern, "\\1\\2\tIf Not IsArray(STArray) Then Exit Sub\\2");
        if (r != before) {
            LogPatch("Added IsArray guard to DoSTAnim", 1);
        }
    }

    return r;
}

// =============================================================================
// Controller.Pause and Controller.Stop patch - Android/Wine doesn't have Controller before init
// Pattern: Controller.Pause = True/False/1/0 -> comment out
// Pattern: Controller.Stop -> remove from colon-separated or comment out
// =============================================================================
std::string SimpleScriptPatcher::PatchControllerPause(const std::string& script) {
    std::string r = script;
    int count = 0;

    // === Controller.Pause ===
    // Special case FIRST: `Sub X:Controller.Pause=Y:End Sub` — a single-line
    // sub whose ONLY body is Controller.Pause. If p1 below strips this naively,
    // we get `Sub X:End Sub` — an empty single-line Sub that Wine VBScript's
    // parser rejects (fails the whole script compile with a generic line-1
    // "Description unavailable"). Convert to a multi-line Sub with a comment
    // body instead, which every VBScript parser accepts. Observed in Medieval
    // Madness Bigus MOD 3.0 Table1_Paused / Table1_unPaused.
    static const RE2 pSubOnlyPause(
        R"((?i)(Sub\s+\w+(?:\s*\([^)]*\))?)\s*:\s*Controller\.Pause\s*=\s*(?:True|False|1|0)\s*:\s*(End\s+Sub))"
    );
    std::string before = r;
    r = RE2Replace(r, pSubOnlyPause, "\\1\r\n\t' Controller.Pause disabled for Android\r\n\\2");
    if (r != before) count++;

    // General case: colon-separated Controller.Pause in a sub with OTHER body.
    // Here removing just the Pause segment still leaves a valid non-empty sub.
    //   Example: Sub Foo:PlaySound "x":Controller.Pause=1:DoThing:End Sub
    //         → Sub Foo:PlaySound "x":DoThing:End Sub
    static const RE2 p1(R"((?i):[ \t]*Controller\.Pause\s*=\s*(True|False|1|0)[ \t]*:)");
    before = r;
    r = RE2Replace(r, p1, ":");
    if (r != before) count++;

    // Then handle statements on their own lines (comment them out)
    static const RE2 p2(R"((?i)(\s*)(Controller\.Pause\s*=\s*(True|False|1|0)))");
    before = r;
    r = RE2Replace(r, p2, "\\1' \\2 ' Disabled for Android");
    if (r != before) count++;

    // === Controller.Stop ===
    // Handle colon-separated (e.g., Sub Foo_Exit:Controller.Stop:End Sub)
    static const RE2 p3(R"((?i):[ \t]*Controller\.Stop[ \t]*:)");
    before = r;
    r = RE2Replace(r, p3, ":");
    if (r != before) count++;

    // Handle single-line If-Then with Controller.Stop (e.g., If B2SOn Then Controller.Stop)
    // Must replace with a valid statement, not just a comment
    static const RE2 p3b(R"((?i)(If\s+[^\r\n]+\s+Then\s+)Controller\.Stop)");
    before = r;
    r = RE2Replace(r, p3b, "\\1Exit Sub ' Controller.Stop disabled for Android");
    if (r != before) count++;

    // Handle Controller.Stop on its own line
    static const RE2 p4(R"((?i)(^[ \t]*)(Controller\.Stop)[ \t]*$)");
    before = r;
    r = RE2Replace(r, p4, "\\1' \\2 ' Disabled for Android");
    if (r != before) count++;

    if (count > 0) {
        LogPatch("Commented out Controller.Pause/Stop statements", count);
    }
    return r;
}

// =============================================================================
// PinUp Player file access patch - Android doesn't have these files
// Pattern: Code that reads DMDType from a PinUp Player ScreenType.txt file
// This is Windows-specific and will fail on Android. Comment out and set default.
// =============================================================================
std::string SimpleScriptPatcher::PatchPinUpPlayerFileAccess(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Comment out: Dim ObjFso: Set ObjFso = CreateObject("Scripting.FileSystemObject")
    static const RE2 p1(R"((?i)(^|\n)([ \t]*)(Dim\s+ObjFso\s*:\s*Set\s+ObjFso\s*=\s*CreateObject\s*\(\s*"Scripting\.FileSystemObject"\s*\)))");
    r = RE2ReplaceWithCallback(r, p1, [&count](const RE2Match& m) -> std::string {
        count++;
        return std::string(m[1]) + std::string(m[2]) + "' " + std::string(m[3]) + " ' Disabled for Android";
    });

    // Comment out: Dim ObjFile: Set ObjFile = ObjFso.OpenTextFile(...)
    static const RE2 p2(R"((?i)(^|\n)([ \t]*)(Dim\s+ObjFile\s*:\s*Set\s+ObjFile\s*=\s*ObjFso\.OpenTextFile\s*\([^)]*\)))");
    r = RE2ReplaceWithCallback(r, p2, [&count](const RE2Match& m) -> std::string {
        count++;
        return std::string(m[1]) + std::string(m[2]) + "' " + std::string(m[3]) + " ' Disabled for Android";
    });

    // Replace: Dim DMDType: DMDType = ObjFile.ReadLine -> Dim DMDType: DMDType = "1"
    static const RE2 p3(R"((?i)(Dim\s+DMDType\s*:\s*DMDType\s*=\s*)ObjFile\.ReadLine)");
    std::string before = r;
    r = RE2Replace(r, p3, "\\1\"1\" ' Default for Android (was ObjFile.ReadLine)");
    if (r != before) count++;

    if (count > 0) {
        LogPatch("Patched PinUp Player file access for Android", count);
    }
    return r;
}

// =============================================================================
// Fix parenthesized Not function calls - Wine VBScript arity mismatch bug
// Pattern: (Not IsNull)(m_transition) -> Not IsNull(m_transition)
// This is invalid VBScript syntax that Windows tolerates but Wine rejects
// with Error 450: VBSE_FUNC_ARITY_MISMATCH
// =============================================================================
std::string SimpleScriptPatcher::PatchParenthesizedNot(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Match pattern: (Not FunctionName)(argument)
    // Captures: FunctionName, argument
    // Replace with: Not FunctionName(argument)
    static const RE2 p(R"((?i)\(Not\s+(\w+)\)\s*\(([^)]+)\))");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        count++;
        PLOGI.printf("PatchParenthesizedNot: (Not %s)(%s) -> Not %s(%s)",
            std::string(m[1]).c_str(), std::string(m[2]).c_str(),
            std::string(m[1]).c_str(), std::string(m[2]).c_str());
        return "Not " + std::string(m[1]) + "(" + std::string(m[2]) + ")";
    });

    if (count > 0) {
        LogPatch("Fixed parenthesized Not function calls (Wine arity bug)", count);
    }
    return r;
}

// =============================================================================
// Fix forward reference to cGameName constant
// Pattern: If Right(cGamename,1)="c" Then ... uses cGameName before it's defined
// Wine VBScript doesn't handle forward references to constants like Windows does
// Wrap in error handling to allow script to continue
// =============================================================================
std::string SimpleScriptPatcher::PatchForwardConstantReference(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Match: If Right(cGamename,1)="c" Then CustomDMD=True (or similar)
    // This pattern uses cGameName before it's defined as a Const
    // Wrap in On Error Resume Next to handle the forward reference
    static const RE2 p(R"((?i)(If\s+Right\s*\(\s*cGamename\s*,\s*1\s*\)\s*=\s*"c"\s+Then\s+)(\w+\s*=\s*True))");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        count++;
        PLOGI.printf("PatchForwardConstantReference: Wrapping cGamename check in error handling");
        // Default to False and comment out the check - color ROM detection doesn't work with forward ref
        return "' " + std::string(m[0]) + " ' Disabled - cGameName forward reference issue on Android";
    });

    if (count > 0) {
        LogPatch("Fixed cGameName forward reference issue", count);
    }
    return r;
}

// =============================================================================
// SolCallback assignments - Wine VBScript fails because constants aren't defined yet
// The SolCallback array and constants like sBallRelease are defined in VPM helper scripts
// Wrap the SolCallback block in On Error Resume Next to allow compilation to continue
// =============================================================================
std::string SimpleScriptPatcher::PatchSolCallbackBlock(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Find the first SolCallback line and wrap the block in error handling
    // Pattern: Find "'Solenoid Call backs" comment or first SolCallback line
    // Insert On Error Resume Next before it
    static const RE2 commentPattern(R"((?i)('\*+\s*\r?\n'\s*Solenoid\s+Call\s*backs?\s*\r?\n'\*+))");
    RE2Match match;
    if (RE2FindFirst(r, commentPattern, match)) {
        // Found the comment block, insert error handling before it
        count++;
        PLOGI.printf("PatchSolCallbackBlock: Found Solenoid Callbacks comment block");
        std::string replacement = "On Error Resume Next ' Wine VBScript SolCallback fix\r\n" + std::string(match[0]);
        r = r.substr(0, match.position) + replacement + r.substr(match.position + match.length);
    } else {
        // Try finding first SolCallback line
        static const RE2 solCallbackPattern(R"((?i)(^|\r?\n)([ \t]*SolCallback\s*\())");
        if (RE2FindFirst(r, solCallbackPattern, match)) {
            count++;
            PLOGI.printf("PatchSolCallbackBlock: Found first SolCallback line");
            // Insert On Error Resume Next before the first SolCallback
            std::string replacement = std::string(match[1]) + "On Error Resume Next ' Wine VBScript SolCallback fix\r\n" + std::string(match[2]);
            r = r.substr(0, match.position) + replacement + r.substr(match.position + match.length);
        }
    }

    if (count > 0) {
        LogPatch("Added error handling for SolCallback block (Wine VPM compatibility)", count);
    }
    return r;
}

// =============================================================================
// Select Case with array element access - Wine VBScript doesn't support this
// Pattern: Select Case ArrayName(index) -> vpx_ssc_tmp = ArrayName(index) \n Select Case vpx_ssc_tmp
// Uses temp variable to work around Wine's Select Case limitations.
//
// IMPORTANT: emitted as TWO LINES, not colon-joined. A nested Select Case inside
// an outer Case body (e.g. Bigus MOD Medieval Madness PrimStandupTgtMove) is
// fine as multi-line but breaks Wine's parser when the inner Select is opened
// via `:` on the same line as a preceding statement. Same structural lesson as
// the single-line For Each fix (c28a06c): never emit patcher output joined by
// colons — always newlines.
// =============================================================================
std::string SimpleScriptPatcher::PatchSelectCaseArrayAccess(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Match: Select Case ArrayName(index)
    // where ArrayName is a word and index can be a variable or expression.
    // The index group allows ONE level of nested parens so calls like
    // `LightType(chgLamp(ii, 0))`, `Shots(CurPlayer, CurSong(CurPlayer))` and
    // `BitSetCount(avSecretProgress(nCurPlayer), 6)` are captured intact and
    // the outer `)` lands in the right place in the rewrite. Previously the
    // naive `[^)]+` stopped at the first `)` and left the outer `)` stranded
    // after vpx_ssc_tmp, producing unparseable output on every Diner/Black
    // Knight 2000/KISS/American Dad variant that used nested indices.
    // Transform to:
    //     vpx_ssc_tmp = ArrayName(index)
    //     Select Case vpx_ssc_tmp<suffix>
    // Note: vpx_ssc_tmp is declared globally in InjectHelpers (Dim not allowed inside Case blocks)
    static const RE2 p(R"((?i)([ \t]*)(Select\s+Case\s+)(\w+)\s*\(\s*([^()]*(?:\([^()]*\)[^()]*)*)\s*\)(\w?))");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        std::string indent = m[1];
        std::string selectCase = m[2];
        std::string arrayName = m[3];
        std::string index = m[4];
        std::string nextChar = m[5];

        // Skip if it looks like a function call (common VBS functions)
        std::string lowerName = arrayName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName == "ubound" || lowerName == "lbound" || lowerName == "len" ||
            lowerName == "mid" || lowerName == "left" || lowerName == "right" ||
            lowerName == "instr" || lowerName == "cint" || lowerName == "clng" ||
            lowerName == "cstr" || lowerName == "asc" || lowerName == "chr" ||
            lowerName == "int" || lowerName == "fix" || lowerName == "abs" ||
            lowerName == "sgn" || lowerName == "rnd" || lowerName == "round" ||
            lowerName == "hex" || lowerName == "oct" || lowerName == "cbool" ||
            lowerName == "cbyte" || lowerName == "ccur" || lowerName == "cdate" ||
            lowerName == "cdbl" || lowerName == "csng" || lowerName == "trim" ||
            lowerName == "ltrim" || lowerName == "rtrim" || lowerName == "lcase" ||
            lowerName == "ucase" || lowerName == "space" || lowerName == "string" ||
            lowerName == "split" || lowerName == "join" || lowerName == "replace" ||
            lowerName == "instrrev" || lowerName == "strreverse" || lowerName == "array" ||
            lowerName == "filter" || lowerName == "isarray" || lowerName == "isdate" ||
            lowerName == "isempty" || lowerName == "isnull" || lowerName == "isnumeric" ||
            lowerName == "isobject" || lowerName == "typename" || lowerName == "vartype" ||
            lowerName == "ssp_idx") {
            return std::string(m[0]);  // Don't transform function calls
        }

        count++;
        PLOGI.printf("PatchSelectCaseArrayAccess: %s(%s) -> temp variable", arrayName.c_str(), index.c_str());
        // Only insert a separating space when the next char is a word char that would
        // otherwise fuse with vpx_ssc_tmp (e.g. `)MOD` -> `vpx_ssc_tmpMOD`). A space before
        // `.`, `:`, operators, etc. breaks Wine's VBScript tokenizer (e.g. `vpx_ssc_tmp .image`).
        std::string spacer = nextChar.empty() ? "" : " ";
        return indent + "vpx_ssc_tmp = " + arrayName + "(" + index + ")\r\n" +
               indent + selectCase + "vpx_ssc_tmp" + spacer + nextChar;
    });

    if (count > 0) {
        LogPatch("Fixed Select Case with array access (Wine limitation)", count);
    }
    return r;
}

// =============================================================================
// Enable FlexDMD Virtual Segment DMD by default for GLF tables
// Pattern: Table1.Option("Glf Virtual Segment DMD", 0, 1, 1, 0, 0, ...)
//                                                        ^ change to 1
// Also: Change SetDelay to direct call since Android timing may have issues
// =============================================================================
std::string SimpleScriptPatcher::PatchEnableFlexDMDByDefault(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Match the GLF Virtual Segment DMD option and change default from 0 to 1
    // Parameters: name, min, max, step, default, unit, array
    static const RE2 p(R"((?i)(Table1\.Option\s*\(\s*"Glf Virtual Segment DMD"\s*,\s*0\s*,\s*1\s*,\s*1\s*,\s*)0(\s*,\s*0\s*,))");
    std::string before = r;
    r = RE2Replace(r, p, "\\11\\2");
    if (r != before) count++;

    // Change SetDelay call to direct call - Android timing may cause delays to never fire
    // Pattern: SetDelay "start_flex_segments", "Glf_EnableVirtualSegmentDmd", Null, 500
    // Replace with: Glf_EnableVirtualSegmentDmd Null
    static const RE2 p2(R"((?i)SetDelay\s+"start_flex_segments"\s*,\s*"Glf_EnableVirtualSegmentDmd"\s*,\s*Null\s*,\s*\d+)");
    before = r;
    r = RE2Replace(r, p2, "Glf_EnableVirtualSegmentDmd Null");
    if (r != before) count++;

    if (count > 0) {
        LogPatch("Enabled FlexDMD Virtual Segment DMD by default", count);
    }
    return r;
}

// =============================================================================
// Fix FlexDMD Segments array assignment for Wine VBScript
// Wine VBScript cannot pass arrays to COM properties, so we convert:
//   obj.Segments = arrayVar
// to:
//   obj.SetSegmentsFromString(Join(arrayVar, ","))
// =============================================================================
std::string SimpleScriptPatcher::PatchFlexDMDSegments(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Match: glf_flex_alphadmd.Segments = glf_flex_alphadmd_segments
    // Replace with: glf_flex_alphadmd.SetSegmentsFromString(Join(glf_flex_alphadmd_segments, ","))
    static const RE2 p(R"((?i)(\w+)\.Segments\s*=\s*(\w+))");
    std::string before = r;
    r = RE2Replace(r, p, "\\1.SetSegmentsFromString(Join(\\2, \",\"))");
    if (r != before) count++;

    if (count > 0) {
        LogPatch("Converted FlexDMD .Segments to .SetSegmentsFromString for Wine compatibility", count);
    }
    return r;
}

// =============================================================================
// Single-line For Each - Wine VBScript has issues with For Each over arrays
// Convert For Each to regular For loop using UBound
// Pattern: For Each var In collection : statement : Next
// Convert to: For temp_i = 0 To UBound(collection) : var = collection(temp_i) : statement : Next
// =============================================================================
// Static counter for unique temp variable names
static int s_forEachTempVarCounter = 0;

// =============================================================================
// Single-line For Each - Wine VBScript does NOT support this syntax
// =============================================================================
// CRITICAL LESSONS LEARNED:
// 1. Must use `Set var = array(i)` not `var = array(i)` for COM objects
//    Otherwise: VBSE_ACTION_NOT_SUPPORTED error
// 2. Do NOT use `On Error Resume Next` wrapper around the For loop!
//    When UBound() fails under error suppression, VBScript behavior is undefined
//    and can cause infinite loops / ANR (Application Not Responding)
// 3. SAFE APPROACH: Use `If IsArray(collection) Then` to guard the loop
//    This cleanly skips undefined arrays without any risk of hangs
// =============================================================================
std::string SimpleScriptPatcher::PatchSingleLineForEach(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Pattern 1: Dim var : For Each var In collection : body : Next
    // NOTE: RE2 does NOT support backreferences (\3) so we can't enforce that
    // the Dim variable matches the For Each variable in the regex itself.
    // Instead we capture both and the callback verifies they match.
    // Collection supports expressions like obj(x) via \w+(?:\s*\([^)]*\))?
    // Body uses (.*) to capture multiple colon-separated statements.
    // Wine's VBScript parser chokes on single-line `For Each var In coll : body : Next`
    // but handles multi-line For Each natively. Previously this patcher converted to
    // `For i = 0 To UBound(coll)` indexed iteration, which is semantically WRONG for
    // VPX editor Collections — `IsArray(collection)` returns False, so the guarded
    // body is silently skipped. Monster Bash's `For Each xx in DracTargets:...:Next`
    // (iterating an editor Collection) never actually dropped any walls, producing
    // the "invisible wall blocks ball" stuck bug. Fix: expand to multi-line For Each
    // so Collection iteration semantics are preserved.
    static const RE2 p1(R"((?im)^([ \t]*)Dim\s+(\w+)\s*:\s*For\s+Each\s+(\w+)\s+[Ii]n\s+(\w+(?:\s*\([^)]*\))?)\s*:\s*(.*?)\s*:\s*[Nn]ext[ \t]*(?:'[^\r\n]*)?\r?$)");
    r = RE2ReplaceWithCallback(r, p1, [&count](const RE2Match& m) -> std::string {
        std::string indent = m[1];
        std::string dimVar = m[2];
        std::string forVar = m[3];
        std::string collection = m[4];
        std::string body = m[5];
        // Verify Dim variable matches For Each variable (case-insensitive)
        if (!EqualsIgnoreCase(dimVar, forVar)) {
            return m.full_match;  // Not a match, return unchanged
        }
        count++;
        PLOGI.printf("PatchSingleLineForEach: Expanding Dim + For Each to multi-line (body: %s)", body.c_str());
        return indent + "Dim " + forVar + "\r\n" +
               indent + "For Each " + forVar + " In " + collection + "\r\n" +
               indent + "\t" + body + "\r\n" +
               indent + "Next";
    });

    // Pattern 2: For Each var In collection : body : Next (no Dim prefix)
    // Body uses (.*?) with trailing :\s*Next anchor to capture all statements.
    // Collection supports expressions like obj(x).
    static const RE2 p2(R"((?im)^([ \t]*)For\s+Each\s+(\w+)\s+[Ii]n\s+(\w+(?:\s*\([^)]*\))?)\s*:\s*(.*?)\s*:\s*[Nn]ext[ \t]*(?:'[^\r\n]*)?\r?$)");
    r = RE2ReplaceWithCallback(r, p2, [&count](const RE2Match& m) -> std::string {
        std::string indent = m[1];
        std::string varName = m[2];
        std::string collection = m[3];
        std::string body = m[4];
        count++;
        PLOGI.printf("PatchSingleLineForEach: Expanding single-line For Each to multi-line (body: %s)", body.c_str());
        return indent + "For Each " + varName + " In " + collection + "\r\n" +
               indent + "\t" + body + "\r\n" +
               indent + "Next";
    });

    // Pattern 3: Multi-line For Each (already expanded or written multi-line)
    // NOTE: This pattern uses (?!Next\b) negative lookahead which RE2 does NOT support.
    // The pattern silently fails (re_.ok() == false). However, Wine VBScript handles
    // multi-line For Each natively, so this pattern is not needed for correctness.
    // Keeping it as documentation of what was attempted.
    static const RE2 p3(R"((?ims)^([ \t]*)(For\s+Each\s+(\w+)\s+[Ii]n\s+(\w+)\s*\r?\n)((?:(?!Next\b)[^\r\n]*\r?\n)*?)([ \t]*)(Next\b))");
    if (!p3.ok()) {
        PLOGI.printf("PatchSingleLineForEach: Pattern 3 (multi-line) skipped - RE2 does not support negative lookahead");
    } else {
        r = RE2ReplaceWithCallback(r, p3, [&count](const RE2Match& m) -> std::string {
            std::string indent = m[1];
            std::string varName = m[3];
            std::string collection = m[4];
            std::string body = m[5];
            std::string nextIndent = m[6];
            std::string tempIdx = "ssp_i" + std::to_string(s_forEachTempVarCounter++);
            count++;
            PLOGI.printf("PatchSingleLineForEach: Converting multi-line For Each to For loop");
            return indent + "Dim " + tempIdx + " : If IsArray(" + collection + ") Then : For " + tempIdx + " = 0 To UBound(" + collection + ")\r\n" +
                   indent + "\tIf IsObject(" + collection + "(" + tempIdx + ")) Then : Set " + varName + " = " + collection + "(" + tempIdx + ") : Else : " + varName + " = " + collection + "(" + tempIdx + ") : End If\r\n" +
                   body +
                   nextIndent + "Next : End If";
        });
    }

    if (count > 0) {
        LogPatch("Converted For Each to For loop for Wine compatibility", count);
    }
    return r;
}

// =============================================================================
// System.Collections.ArrayList - .NET class not available on Android/Wine
// Replace CreateObject("System.Collections.ArrayList") with VBScript class
// =============================================================================
std::string SimpleScriptPatcher::PatchArrayList(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Check if script uses ArrayList
    static const RE2 checkPattern(R"((?i)CreateObject\s*\(\s*"System\.Collections\.ArrayList"\s*\))");
    if (!RE2::PartialMatch(script, checkPattern)) {
        return r;  // No ArrayList usage, skip
    }

    // Replace CreateObject("System.Collections.ArrayList") with (new VBSArrayList)
    static const RE2 p(R"((?i)CreateObject\s*\(\s*"System\.Collections\.ArrayList"\s*\))");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        count++;
        PLOGI.printf("PatchArrayList: Replacing System.Collections.ArrayList with VBSArrayList");
        return "(new VBSArrayList)";
    });

    // Inject the VBSArrayList class at the start of the script (after any Option Explicit)
    if (count > 0) {
        std::string arrayListClass = R"(
' ============================================================================
' VBSArrayList - Wine/Android replacement for System.Collections.ArrayList
' ============================================================================
Class VBSArrayList
    Private m_items()
    Private m_count

    Private Sub Class_Initialize()
        m_count = 0
        ReDim m_items(-1)
    End Sub

    Public Sub Add(item)
        ReDim Preserve m_items(m_count)
        m_items(m_count) = item
        m_count = m_count + 1
    End Sub

    Public Property Get Count()
        Count = m_count
    End Property

    Public Property Get Item(index)
        If index >= 0 And index < m_count Then
            Item = m_items(index)
        End If
    End Property

    Public Default Property Get ItemDefault(index)
        ItemDefault = Item(index)
    End Property

    Public Sub Remove(item)
        Dim i, j, found
        found = False
        For i = 0 To m_count - 1
            If m_items(i) = item Then
                found = True
                Exit For
            End If
        Next
        If found Then
            For j = i To m_count - 2
                m_items(j) = m_items(j + 1)
            Next
            m_count = m_count - 1
            If m_count > 0 Then
                ReDim Preserve m_items(m_count - 1)
            Else
                ReDim m_items(-1)
            End If
        End If
    End Sub

    Public Sub RemoveAt(index)
        If index >= 0 And index < m_count Then
            Dim j
            For j = index To m_count - 2
                m_items(j) = m_items(j + 1)
            Next
            m_count = m_count - 1
            If m_count > 0 Then
                ReDim Preserve m_items(m_count - 1)
            Else
                ReDim m_items(-1)
            End If
        End If
    End Sub

    Public Sub Clear()
        m_count = 0
        ReDim m_items(-1)
    End Sub

    Public Function Contains(item)
        Dim i
        Contains = False
        For i = 0 To m_count - 1
            If m_items(i) = item Then
                Contains = True
                Exit Function
            End If
        Next
    End Function

    Public Function IndexOf(item)
        Dim i
        IndexOf = -1
        For i = 0 To m_count - 1
            If m_items(i) = item Then
                IndexOf = i
                Exit Function
            End If
        Next
    End Function
End Class

)";
        // Insert after Option Explicit if present, otherwise at start
        static const RE2 optionExplicit(R"((?i)(Option\s+Explicit[^\r\n]*\r?\n))");
        std::string before = r;
        r = RE2Replace(r, optionExplicit, "\\1" + arrayListClass);
        if (r == before) {
            // No Option Explicit, insert at very beginning
            r = arrayListClass + r;
        }

        LogPatch("Replaced System.Collections.ArrayList with VBSArrayList class", count);
    }

    return r;
}

// =============================================================================
// RenderingMode - VPX global that may not be defined on Android
// Some tables check RenderingMode for VR detection (0=Desktop, 2=VR)
// =============================================================================
// LESSON LEARNED:
// - Initial pattern checked for `RenderingMode\s*=` to detect if already defined
// - PROBLEM: This matched comparisons like `If RenderingMode = 2` thinking it was
//   an assignment, causing the patcher to skip replacement
// - SOLUTION: Only check for explicit `Dim RenderingMode` or `Const RenderingMode`
//   declarations, not assignment patterns
// =============================================================================
std::string SimpleScriptPatcher::PatchRenderingMode(const std::string& script) {
    std::string r = script;

    // Check if script uses RenderingMode
    static const RE2 checkPattern(R"((?i)\bRenderingMode\b)");
    if (!RE2::PartialMatch(script, checkPattern)) {
        return r;  // No RenderingMode usage, skip
    }

    // Check if RenderingMode is already defined (Dim or Const declaration ONLY)
    // IMPORTANT: Do NOT check for assignment pattern (RenderingMode = X) as this
    // incorrectly matches comparisons like `If RenderingMode = 2`
    static const RE2 definedPattern(R"((?i)(?:Dim|Const)\s+RenderingMode\b)");
    if (RE2::PartialMatch(script, definedPattern)) {
        return r;  // Already defined, skip
    }

    // Wrap RenderingMode usage in a function that returns 0 if not defined
    // This is safer than injecting a Const which might conflict
    // Replace: If RenderingMode = X  with  If GetRenderingMode() = X
    static const RE2 usagePattern(R"((?i)\bRenderingMode\b)");
    int count = 0;
    r = RE2ReplaceWithCallback(r, usagePattern, [&count](const RE2Match& m) -> std::string {
        count++;
        return "0"; // Just replace RenderingMode with 0 (Desktop mode)
    });

    if (count > 0) {
        LogPatch("Replaced undefined RenderingMode with 0 (Desktop mode)", count);
    }
    return r;
}

// =============================================================================
// TestVRonDT - VR testing variable that may not be defined
// Replace undefined TestVRonDT with False (not testing VR on Desktop/Android)
// =============================================================================
// LESSON LEARNED:
// - Some scripts define `Const TestVRonDT = false` AFTER its first use
// - This is invalid VBScript (constants must be declared before use) but scripts do it
// - If we just check for Const existence and skip, the first use still fails
// - SOLUTION: Remove the Const declaration entirely and replace ALL usages with False
//   This handles the forward-reference bug in the original scripts
// =============================================================================
std::string SimpleScriptPatcher::PatchTestVRonDT(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Check if script uses TestVRonDT
    static const RE2 checkPattern(R"((?i)\bTestVRonDT\b)");
    if (!RE2::PartialMatch(script, checkPattern)) {
        return r;  // No TestVRonDT usage, skip
    }

    // First, REMOVE any Const TestVRonDT declaration
    // This handles scripts that declare Const AFTER first use (invalid but common)
    static const RE2 constPattern(R"((?im)^[ \t]*Const\s+TestVRonDT\s*=\s*[^\r\n]*\r?\n)");
    r = RE2ReplaceWithCallback(r, constPattern, [&count](const RE2Match& m) -> std::string {
        count++;
        PLOGI.printf("PatchTestVRonDT: Removing Const TestVRonDT declaration");
        return ""; // Remove the Const line
    });

    // Replace all TestVRonDT usages with False
    static const RE2 usagePattern(R"((?i)\bTestVRonDT\b)");
    r = RE2ReplaceWithCallback(r, usagePattern, [&count](const RE2Match& m) -> std::string {
        count++;
        return "False"; // Replace with False (not testing VR on Desktop)
    });

    if (count > 0) {
        LogPatch("Replaced TestVRonDT with False", count);
    }
    return r;
}

// =============================================================================
// Orphaned Next after commented For Each - table script bug fix
// Pattern: '  For Each ... \n  statement \n  Next  (For Each commented but body/Next not)
// Fix by commenting out the orphaned lines
// =============================================================================
std::string SimpleScriptPatcher::PatchOrphanedNext(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Pattern: commented For Each, followed by uncommented statement(s), then uncommented Next
    // '    For Each X In Y
    //      statement
    //  Next
    static const RE2 p(R"((?im)(^[ \t]*'[ \t]*For\s+Each\s+\w+\s+[Ii]n\s+\w+[^\r\n]*\r?\n)((?:[ \t]+[^'\r\n][^\r\n]*\r?\n)*?)([ \t]*)(Next\b))");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        std::string commentedForEach = m[1];
        std::string body = m[2];
        std::string indent = m[3];
        std::string next = m[4];

        // Check if body lines are not already commented
        if (body.length() > 0 && body.find_first_not_of(" \t\r\n") != std::string::npos) {
            // Comment out the body lines
            std::string commentedBody;
            std::istringstream iss(body);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.find_first_not_of(" \t\r\n") != std::string::npos) {
                    // Find leading whitespace
                    size_t firstNonSpace = line.find_first_not_of(" \t");
                    if (firstNonSpace != std::string::npos && line[firstNonSpace] != '\'') {
                        commentedBody += line.substr(0, firstNonSpace) + "' " + line.substr(firstNonSpace) + "\r\n";
                    } else {
                        commentedBody += line + "\r\n";
                    }
                } else {
                    commentedBody += line + "\r\n";
                }
            }
            count++;
            PLOGI.printf("PatchOrphanedNext: Commenting out orphaned body and Next after commented For Each");
            return commentedForEach + commentedBody + indent + "' " + next;
        }
        return std::string(m[0]);
    });

    if (count > 0) {
        LogPatch("Fixed orphaned Next statements after commented For Each", count);
    }
    return r;
}

// =============================================================================
// Controller methods that may fail on Android - wrap with error handling
// Includes: ChangedLamps, B2SSetData, and other Controller methods
// =============================================================================
std::string SimpleScriptPatcher::PatchControllerChangedLamps(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Pattern 1: Sub containing Controller.ChangedLamps
    static const RE2 p(R"((?is)(Sub\s+\w*(?:Lamp|Timer)\w*_timer\s*(?:\([^)]*\))?[^\r\n]*\r?\n)([\t ]*)(Dim\s+[^\r\n]*\r?\n)([\t ]*)(\w+\s*=\s*Controller\.ChangedLamps))");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        count++;
        PLOGI.printf("PatchControllerChangedLamps: Adding error handling to sub with Controller.ChangedLamps");
        return std::string(m[1]) +
               std::string(m[2]) + "On Error Resume Next ' Android: Controller methods may fail\r\n" +
               std::string(m[2]) + std::string(m[3]) +
               std::string(m[4]) + std::string(m[5]);
    });

    // Pattern 2: Direct assignment without Dim
    static const RE2 p2(R"((?is)(Sub\s+\w*(?:Lamp|Timer)\w*_timer\s*(?:\([^)]*\))?[^\r\n]*\r?\n)([\t ]*)(\w+\s*=\s*Controller\.ChangedLamps))");
    r = RE2ReplaceWithCallback(r, p2, [&count](const RE2Match& m) -> std::string {
        std::string subLine = m[1];
        if (subLine.find("On Error Resume Next") != std::string::npos) {
            return std::string(m[0]);
        }
        count++;
        PLOGI.printf("PatchControllerChangedLamps: Adding error handling (pattern 2)");
        return std::string(m[1]) +
               std::string(m[2]) + "On Error Resume Next ' Android: Controller methods may fail\r\n" +
               std::string(m[2]) + std::string(m[3]);
    });

    // =========================================================================
    // B2SSetData - Backglass Server method that may fail on Android
    // =========================================================================
    // LESSON LEARNED:
    // B2SSetData appears in THREE different contexts in VBScript:
    //   a) Start of line:     `    Controller.B2SSetData 179,0`
    //   b) After colon:       `sw73.IsDropped = 0:Controller.B2SSetData 179,0`
    //   c) After Then:        `If bFlag=0 Then Controller.B2SSetData bg_id,1`
    //
    // Initial fix only handled (a), causing WoZ crashes during gameplay
    // when patterns (b) and (c) were hit. Had to add patterns for all cases.
    // Each wraps the call with inline error handling.
    // =========================================================================

    // Pattern 3a: B2SSetData at start of line
    static const RE2 p3a(R"((?im)^([ \t]*)(Controller\.B2SSetData\s+[^:\r\n]*))");
    r = RE2ReplaceWithCallback(r, p3a, [&count](const RE2Match& m) -> std::string {
        std::string indent = m[1];
        std::string call = m[2];
        count++;
        PLOGI.printf("PatchControllerChangedLamps: Wrapping B2SSetData call (start of line)");
        return indent + "On Error Resume Next : " + call + " : On Error Goto 0";
    });

    // Pattern 3b: B2SSetData after colon (inline statement separator).
    //   Example: sw73.IsDropped = 0:Controller.B2SSetData 179,0
    //
    // IMPORTANT: we must NOT wrap when the containing line is a single-line
    // If-Then (e.g. `If step=2 Then Controller.B2SSetData 131,1:Controller.B2SSetData 132,0`).
    // Wrapping each call with `On Error Resume Next : ... : On Error Goto 0`
    // produces multiple `On Error Goto 0` statements inside the single-line If
    // body, which Wine's VBScript parser rejects (fails the whole script with
    // "Description unavailable" at line 1). Medieval Madness Bigus MOD 3.0 has
    // 21 such lines and hits this exact path. Check the current line for an
    // `If ... Then` prefix in the callback and skip wrapping when present.
    static const RE2 p3b(R"((?i)(:\s*)(Controller\.B2SSetData\s+[^:\r\n]*))");
    static const RE2 ifThenOnLine(R"((?i)\bIf\b.+\bThen\b)");
    const std::string p3bSnapshot = r;
    r = RE2ReplaceWithCallback(p3bSnapshot, p3b,
        [&count, &p3bSnapshot](const RE2Match& m) -> std::string {
            size_t pos = m.position;
            size_t lineStart = p3bSnapshot.rfind('\n', pos == 0 ? 0 : pos - 1);
            lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
            std::string linePrefix = p3bSnapshot.substr(lineStart, pos - lineStart);
            if (RE2::PartialMatch(linePrefix, ifThenOnLine)) {
                // Inside a single-line If-Then. Leave the call unwrapped —
                // Wine can't handle On Error Goto 0 nested in an If body.
                return std::string(m[0]);
            }
            count++;
            PLOGI.printf("PatchControllerChangedLamps: Wrapping B2SSetData call (after colon)");
            return std::string(m[1]) + "On Error Resume Next : " + std::string(m[2]) + " : On Error Goto 0";
        });

    // Pattern 3c was here: `If x Then Controller.B2SSetData ...`. Removed —
    // this is by definition inside a single-line If-Then, and Wine rejects
    // `On Error Goto 0` there. If B2SSetData fails inside such a conditional,
    // the script-level protection is minimal anyway (the whole If body is
    // about that one action). Better to leave it unwrapped than to break
    // every table that hits it.

    if (count > 0) {
        LogPatch("Added error handling for Controller methods (ChangedLamps, B2SSetData)", count);
    }
    return r;
}

// =============================================================================
// Duplicate vpmInit Me calls corrupt flipper callback state in Wine
// Some table scripts (e.g., Back to the Future) call vpmInit Me twice.
// Wine VBScript re-initializes callbacks on the second call, breaking flippers.
// Fix: Keep only the first vpmInit Me call, comment out duplicates.
// =============================================================================
std::string SimpleScriptPatcher::PatchDuplicateVpmInit(const std::string& script) {
    std::string r = script;

    // Find all vpmInit Me calls (case insensitive)
    static const RE2 vpmInitRegex(R"((?i)\bvpmInit\s+[Mm]e\b)");

    auto matches = RE2FindAll(r, vpmInitRegex);

    if (matches.size() > 1) {
        PLOGI.printf("PatchDuplicateVpmInit: Found %zu vpmInit calls - removing duplicates", matches.size());

        // Remove all but the first occurrence (process in reverse to preserve positions)
        for (size_t i = matches.size() - 1; i > 0; --i) {
            size_t pos = matches[i].position;
            size_t len = matches[i].length;
            // Comment out the duplicate instead of removing to preserve line numbers
            r = r.substr(0, pos) + "' [Wine: Removed duplicate] " + r.substr(pos, len) + r.substr(pos + len);
            PLOGI.printf("PatchDuplicateVpmInit: Commented out duplicate vpmInit at position %zu", pos);
        }

        LogPatch("Removed duplicate vpmInit Me calls (Wine flipper fix)", (int)(matches.size() - 1));
    }

    return r;
}

// =============================================================================
// Dangling Else: single-line If-Then-Else got split across two lines.
// Pattern observed in South Park 1.3 FlasherTimer2_Timer and Black Knight VR
// Room v2.0.2 equivalent — a line ending with `: Else` followed by the else
// body on the NEXT line. Whether the table author wrote it that way or an
// upstream pass produced it, the result is invalid VBScript (Wine and MS
// both reject it). Fix: join the two lines so the If-Then-Else parses as
// a legal single-line construct.
//
// Guards against eating structural keywords on the next line (Next, End ...,
// Else, Case, Loop, Wend) so a `: Else` at end of an unrelated If doesn't
// accidentally consume a loop terminator.
// =============================================================================
std::string SimpleScriptPatcher::PatchDanglingElseBody(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Match: `...: Else\r?\n<indent><body>` where body is the NEXT physical line
    // and doesn't start with a structural keyword or another colon.
    static const RE2 p(R"((?im)(:[ \t]*Else)[ \t]*\r?\n[ \t]+([^:\r\n].*?)[ \t]*$)");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        std::string elseTok = m[1];
        std::string body = m[2];
        // Don't join if the "body" is actually the next structural token —
        // those belong to the enclosing block, not to this Else.
        std::string bodyLower = body;
        std::transform(bodyLower.begin(), bodyLower.end(), bodyLower.begin(), ::tolower);
        static const std::string kKeywords[] = {
            "next", "end ", "end\t", "else", "elseif", "case ", "case\t",
            "loop", "wend", "end select", "end if", "end sub", "end function",
        };
        for (const std::string& kw : kKeywords) {
            if (bodyLower.compare(0, kw.size(), kw) == 0) {
                return std::string(m[0]);  // leave unchanged
            }
        }
        count++;
        PLOGI.printf("PatchDanglingElseBody: Joined dangling Else with body '%s'", body.c_str());
        return elseTok + " " + body;
    });

    if (count > 0) {
        LogPatch("Joined dangling ': Else' with following body line", count);
    }
    return r;
}

// =============================================================================
// Reversed relational operators `=>` / `=<`
// Microsoft's VBScript parser silently accepts `=>` as `>=` and `=<` as `<=`.
// Wine's parser rejects them and fails the whole script compile with the
// generic line-1 "Description unavailable" error.
// Observed in Medieval Madness Bigus MOD 3.0 (6 × `=>`).
// =============================================================================
std::string SimpleScriptPatcher::PatchReversedRelationalOp(const std::string& script) {
    std::string r = script;
    int count = 0;

    // `=>` → `>=`. A bare literal match is acceptable: `=>` has no valid VBS
    // meaning, so any occurrence outside a string/comment is a bug. The only
    // false-positive risk is a string literal like `"=>"` that happens to
    // contain the glyphs; MM 3.0 has none, and the cost of a string mutation
    // is low vs. the script-wide compile failure this causes.
    static const RE2 pGe(R"(=>)");
    std::string before = r;
    r = RE2Replace(r, pGe, ">=");
    size_t pos = 0;
    while ((pos = before.find("=>", pos)) != std::string::npos) { count++; pos += 2; }

    // `=<` → `<=` (same reasoning).
    static const RE2 pLe(R"(=<)");
    before = r;
    r = RE2Replace(r, pLe, "<=");
    pos = 0;
    while ((pos = before.find("=<", pos)) != std::string::npos) { count++; pos += 2; }

    if (count > 0) {
        LogPatch("Fixed reversed relational operators (=> / =<)", count);
    }
    return r;
}

// =============================================================================
// Helper function injection
// =============================================================================
std::string SimpleScriptPatcher::InjectHelpers(const std::string& script) {
    std::string helpers = R"(
' ============================================================================
' Wine VBScript Compatibility Helpers (SimpleScriptPatcher)
' ============================================================================

' Global temp variable for Select Case array access workaround
' (Wine VBScript doesn't allow Dim inside Case blocks)
Dim vpx_ssc_tmp

' Temp variable for (new ClassName)(args) decomposition
Dim ssp_newobj

' End Wine VBScript Compatibility Helpers
' ============================================================================

)";

    // Add DropTarget class if needed (but only if not already defined)
    static const RE2 dropTargetClassPattern(R"((?i)\bClass\s+DropTarget\b)");
    if (s_needsDropTargetClass && !RE2::PartialMatch(script, dropTargetClassPattern)) {
        helpers += R"(
' DropTarget class - Wine VBScript cannot handle Array(x)(y) syntax
' This class converts DTArray indexed access to property access
Class DropTarget
  Private m_primary, m_secondary, m_prim, m_sw, m_animate, m_isDropped

  ' Primary, Secondary, Prim are OBJECTS - must use Property Set, not Property Let
  ' Using Property Let with Set inside causes "Description unavailable" errors in Wine
  Public Property Get Primary(): Set Primary = m_primary: End Property
  Public Property Set Primary(input): Set m_primary = input: End Property

  Public Property Get Secondary(): Set Secondary = m_secondary: End Property
  Public Property Set Secondary(input): Set m_secondary = input: End Property

  Public Property Get Prim(): Set Prim = m_prim: End Property
  Public Property Set Prim(input): Set m_prim = input: End Property

  ' Sw, Animate, IsDropped are VALUES - use Property Let (no Set)
  Public Property Get Sw(): Sw = m_sw: End Property
  Public Property Let Sw(input): m_sw = input: End Property

  Public Property Get Animate(): Animate = m_animate: End Property
  Public Property Let Animate(input): m_animate = input: End Property

  Public Property Get IsDropped(): IsDropped = m_isDropped: End Property
  Public Property Let IsDropped(input): m_isDropped = input: End Property

  ' 5-arg version for VPW tables with Array(primary, secondary, prim, sw, animate)
  Public default Function init(primary, secondary, prim, sw, animate)
    Set m_primary = primary
    Set m_secondary = secondary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_isDropped = False
    Set Init = Me
  End Function

  ' 6-arg version called via Init6 for tables with isDropped field
  ' (e.g. Halloween 1978-1981 (Original 2022) uses Array(primary, secondary, prim, sw, animate, isDropped))
  Public Function Init6(primary, secondary, prim, sw, animate, isDropped)
    Set m_primary = primary
    Set m_secondary = secondary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_isDropped = isDropped
    Set Init6 = Me
  End Function
End Class

)";
        PLOGI.printf("SimpleScriptPatcher: Injected DropTarget class");
    } else if (s_needsDropTargetClass) {
        PLOGI.printf("SimpleScriptPatcher: DropTarget class already exists in script, skipping injection");
    }

    // Add StandupTarget class if needed (but only if not already defined)
    static const RE2 standupTargetClassPattern(R"((?i)\bClass\s+StandupTarget\b)");
    if (s_needsStandupTargetClass && !RE2::PartialMatch(script, standupTargetClassPattern)) {
        helpers += R"(
' StandupTarget class - Wine VBScript cannot handle Array(x)(y) syntax
' This class converts STArray indexed access to property access
' Supports both 4-element Array(primary, prim, sw, animate) and 5-element with id
Class StandupTarget
  Private m_primary, m_prim, m_sw, m_animate, m_id

  ' Primary and Prim are OBJECTS - must use Property Set, not Property Let
  ' Using Property Let with Set inside causes "Description unavailable" errors in Wine
  Public Property Get Primary(): Set Primary = m_primary: End Property
  Public Property Set Primary(input): Set m_primary = input: End Property

  Public Property Get Prim(): Set Prim = m_prim: End Property
  Public Property Set Prim(input): Set m_prim = input: End Property

  ' Sw, Animate, Id are VALUES - use Property Let (no Set)
  Public Property Get Sw(): Sw = m_sw: End Property
  Public Property Let Sw(input): m_sw = input: End Property

  Public Property Get Animate(): Animate = m_animate: End Property
  Public Property Let Animate(input): m_animate = input: End Property

  Public Property Get Id(): Id = m_id: End Property
  Public Property Let Id(input): m_id = input: End Property

  ' 4-arg version for tables with Array(primary, prim, sw, animate)
  Public default Function init(primary, prim, sw, animate)
    Set m_primary = primary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_id = Empty
    Set Init = Me
  End Function

  ' 5-arg version called via Init5 for tables with id field
  Public Function Init5(primary, prim, sw, animate, id)
    Set m_primary = primary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_id = id
    Set Init5 = Me
  End Function
End Class

)";
        PLOGI.printf("SimpleScriptPatcher: Injected StandupTarget class");
    } else if (s_needsStandupTargetClass) {
        PLOGI.printf("SimpleScriptPatcher: StandupTarget class already exists in script, skipping injection");
    }

    helpers += R"(' End Wine VBScript Compatibility Helpers
' ============================================================================

)";

    std::string r = script;

    // Find insertion point - after Option Explicit or at start
    static const RE2 optionExplicit(R"((?i)(Option\s+Explicit[^\r\n]*[\r\n]+))");
    RE2Match match;
    if (RE2FindFirst(r, optionExplicit, match)) {
        r = r.substr(0, match.position + match.length) + helpers + r.substr(match.position + match.length);
    } else {
        r = helpers + r;
    }

    LogPatch("Injected helper functions", 1);
    return r;
}

// =============================================================================
// Main Entry Point
// =============================================================================
std::string SimpleScriptPatcher::PatchScript(const std::string& script, const std::string& tableFilename) {
    s_patchReport = "";
    s_needsDropTargetClass = false;
    s_needsStandupTargetClass = false;
    PLOGI.printf("SimpleScriptPatcher: patching script (%zu chars)", script.length());

    std::string result = StripBOM(script);
    // Keep only an input pointer for later GLF diagnostic checks (no copy).
    const std::string& original = script;
    bool patched = false;

    // Post-audit 2026-04-23: only the 12 rules empirically corroborated by the
    // upstream jsm174/vpx-standalone-scripts community project are enabled.
    // The remaining ~18 rules previously applied here had no upstream evidence
    // and were found to be dead weight — MM 2.0 and MM 3.0 load fine with
    // them all disabled. See:
    //   C:\Android\com.retroeki.pinball\crash_investigations\vpx_standalone_scripts_audit.md
    //   memory/feedback_verify_wine_claims_empirically.md
    // If a new table breaks, verify the failure via the VBScriptCompile /
    // VBScriptRuntime logcat tags BEFORE adding or re-enabling any rule.

    // ENABLED (upstream-corroborated, "high" confidence):
    result = PatchMultiplicationInSubCall(result);  // Bug 54177; FNAF, IndyJones-Hanibal, Defender VPW
    // DISABLED 2026-05-08: WScript.Shell is now stubbed at the host level via
    // ScriptWScriptShell (wired in def.cpp::external_create_object). Scripts
    // get a real IDispatch with no-op RegRead/RegWrite/Run/Sleep/etc., so
    // Subs that mix WshShell calls with other init code (Sonic LoadUltraDMD:
    // WshShell.RegWrite ... then Set UltraDMD = CreateObject(...)) run to
    // completion instead of having their bodies wholesale-stubbed by the
    // patcher's regex (which destroyed the post-RegWrite UltraDMD init).
    // result = PatchWScriptShell(result);             // KISS Bigus 1.1, Thundercats 1.0.9
    result = PatchNewClassCall(result);             // (new Class)(args) - bug class confirmed
    // DISABLED 2026-05-08: Wine 11.8 brings bug 58056 fix (chained array indexing) so
    // original `DTArray = Array(Array(...))` + `DTarray(i)(2).transz` syntax works natively.
    // The patcher converted the original arrays into a class hierarchy (Class DropTarget +
    // Set DT1 = (new DropTarget)(...)) which then required EVERY call site to be rewritten
    // — fragile, missed mixed-case variants like AC-DC LUCI's `DTarray(i)(2)` lowercase 'a'
    // in BallSearch, leaving DropTarget objects being indexed → ARITY_MISMATCH on default
    // init member. Better to leave the original Array(Array(...)) untouched and let Wine
    // handle native chained indexing. If empirical testing shows regressions on tables that
    // relied on the conversion, gate this behind a flag rather than re-enabling globally.
    // result = PatchDTArray(result);                  // DT class conversion; many WPC tables
    // result = PatchSTArray(result);                  // ST class conversion; Monster Bash, TZ, Dr Who
    result = PatchAmbiguousCallParens(result);      // Defender VPW v1.4
    result = PatchPinUpPlayerFileAccess(result);    // Thundercats 1.0.9
    result = PatchForwardConstantReference(result); // Wheel of Fortune Stern, Flash Gordon VPW, Cactus Canyon
    result = PatchSolCallbackBlock(result);         // Playboy (Bally 1978); repo issue #267
    // result = PatchSingleLineForEach(result);     // DISABLED 2026-04-23: audit rated "high" by pattern-matching
                                                    // Cue Ball Wizard / Scared Stiff upstream fixes, but those
                                                    // address missing-colon / broken-Sub bugs, NOT a Wine
                                                    // single-line For Each rejection. Empirical evidence: MM 2.0
                                                    // has 106 lines this rule would rewrite and parses fine
                                                    // without the rule. The rule's own comment says "Wine chokes
                                                    // on single-line For Each"; VBScriptCompile logs show Wine
                                                    // parses these lines with error_loc=-1. Cosmetic-only.
    result = PatchDanglingElseBody(result);         // South Park 1.3, Starship Troopers, AC-DC LUCI

    // Warning-only, enabled: alerts us if Dictionary.Keys/Items direct-indexing
    // pattern shows up (Wine Bug 58051; Batman66 patches this manually).
    PatchDictionaryAccess(result);                  // Bug 58051

    // DISABLED (audit confidence: low / medium / dead / pre-existing regression):
    // result = PatchReversedRelationalOp(result);     // dead — empirically disproven 2026-04-23
    // result = PatchAlwaysOnTop(result);              // low — no upstream evidence
    // result = PatchUBound(result);                   // dead — rule itself is a no-op per comment
    // result = PatchBooleanNot(result);               // low
    // result = PatchLineContinuation(result);         // low — real Wine bug, no sampled table needs it
    // result = PatchDoubleDot(result);                // low — defensive, no upstream evidence
    // result = PatchGlfBooleanArray(result);          // low — GLF-specific, sparse in upstream repo
    result = PatchControllerPause(result);          // Knight Rider `Controller.Pause = True` runtime E_FAIL — re-enabled 2026-04-24
    // result = PatchParenthesizedNot(result);         // medium — bug real, no sample citation
    // result = PatchSelectCaseArrayAccess(result);    // low
    // result = PatchControllerChangedLamps(result);   // medium — upstream resizes arrays instead
    result = PatchDuplicateVpmInit(result);         // Back to the Future (Data East 1990) flipper fix — re-enabled per user report
    // result = PatchArrayList(result);                // low — real Android gap but no upstream precedent
    // result = PatchRenderingMode(result);            // medium
    // result = PatchTestVRonDT(result);               // medium
    // result = PatchOrphanedNext(result);             // low — probably self-introduced problem
    // result = PatchEnableFlexDMDByDefault(result);   // low — feature toggle, not a bug workaround
    // result = PatchFlexDMDSegments(result);          // low — bug class plausible, no upstream sample
    // result = PatchGetRefCall(result);               // pre-existing regression marker
    // result = Patch2DArrayAccess(result);            // pre-existing regression marker (breaks lvalue dict(k)("f") = v)
    // PatchSplitIndexing(result);                     // Bug 58056 (warning) — no upstream citation

    // Check if any patches were applied. LogPatch appends to s_patchReport only when
    // count > 0, so a non-empty report means something changed. Avoids an O(N) byte
    // compare against the full original script.
    if (!s_patchReport.empty()) {
        patched = true;
        result = InjectHelpers(result);
    }

    // Per-table layer: runs after all core rules, on the core-patched output. A no-op
    // (byte-identical) for any file not in the registry, so non-registered tables are
    // unaffected. See src/ui/tablepatches.{h,cpp}.
    {
        std::vector<std::pair<std::string, int>> tableApplied;
        result = tablepatches::ApplyTableSpecificPatches(result, tableFilename, &tableApplied);
        for (const auto& a : tableApplied)
            LogPatch(a.first, a.second);
        if (!tableApplied.empty())
            PLOGI.printf("SimpleScriptPatcher: per-table patches for '%s' (%zu)", tableFilename.c_str(), tableApplied.size());
        patched = !s_patchReport.empty(); // include per-table edits in the finalize/log decision
    }

    // CRITICAL: Normalize line endings to CRLF AND sanitize non-ASCII in a single
    // pass. Wine VBScript requires CRLF line endings and its lexer can choke on
    // UTF-8, so we do both in one traversal to avoid two full-script rebuilds.
    {
        std::string finalized;
        finalized.reserve(result.size() + result.size() / 10);
        int nonAsciiCount = 0;
        for (size_t i = 0; i < result.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(result[i]);
            if (c > 127) {
                finalized += ' ';
                ++nonAsciiCount;
            } else if (c == '\r') {
                finalized += "\r\n";
                if (i + 1 < result.size() && result[i + 1] == '\n') {
                    ++i;
                }
            } else if (c == '\n') {
                finalized += "\r\n";
            } else {
                finalized += static_cast<char>(c);
            }
        }
        if (nonAsciiCount > 0) {
            PLOGI.printf("SimpleScriptPatcher: Sanitized %d non-ASCII bytes", nonAsciiCount);
        }
        result = std::move(finalized);
    }

    // Final logging and script dump
    if (patched) {
        PLOGI.printf("SimpleScriptPatcher: patches applied (output %zu chars)", result.length());
        PLOGI.printf("%s", s_patchReport.c_str());

        // Debug: Check if GLF patch survived to the end (only if script uses GLF)
        if (original.find("glf_funcRefMap") != std::string::npos) {
            if (result.find("glf_last_isGetRef") != std::string::npos) {
                PLOGI.printf("GLF patch: FINAL CHECK - glf_last_isGetRef PRESENT in result");
            } else {
                PLOGE.printf("GLF patch: FINAL CHECK - glf_last_isGetRef MISSING from result!");
            }
        }

        // Dump patched script for debugging/error reporting
        DumpScript(result, "patched_script.vbs");
    } else {
        PLOGI.printf("SimpleScriptPatcher: No patches needed - script unchanged");
        PLOGI.printf("SimpleScriptPatcher: Output script length: %zu characters", result.length());
    }

    return result;
}

#endif // __STANDALONE__

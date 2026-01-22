/**
 * @file simplescriptpatcher.cpp
 * @brief Simplified Wine VBScript compatibility patches
 *
 * A minimal script patcher that addresses ONLY confirmed Wine VBScript bugs
 * without the complexity of full class emulation.
 *
 * Philosophy:
 * - Less transformation = less breakage
 * - Only patch what Wine actually fails on
 * - Reuse existing helper wrappers (VPX_SafeUBound, etc.)
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "simplescriptpatcher.h"
#include "scriptpatcher_internal.h"
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

    // Match: SubName (expr)*number at statement position
    // Transform to: SubName number*(expr)
    // Example: AddScore (Score+100)*2 -> AddScore 2*(Score+100)
    static const RE2 p(R"((?im)(^[ \t]*|:[ \t]*)(\w+)[ \t]+\(([^)]+)\)\s*\*\s*(\d+))");

    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        count++;
        return m[1] + m[2] + " " + m[4] + "*(" + m[3] + ")";
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
// Comment out the CreateObject line to avoid runtime errors on Android
// =============================================================================
std::string SimpleScriptPatcher::PatchWScriptShell(const std::string& script) {
    std::string r = script;
    int count = 0;

    // Match: Set variable = CreateObject("WScript.Shell")
    static const RE2 p(R"((?i)(Set\s+\w+\s*=\s*CreateObject\s*\(\s*"WScript\.Shell"\s*\)))");
    r = RE2ReplaceWithCallback(r, p, [&count](const RE2Match& m) -> std::string {
        count++;
        // Comment out the line - WScript.Shell not available on Android
        return "' DISABLED ON ANDROID: " + std::string(m[1]);
    });

    if (count > 0) {
        LogPatch("Disabled Windows-only WScript.Shell creation", count);
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
    // No-op - now handled in Wine VBScript compiler
    return script;
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
// Multi-dimensional Array Access: Array(x)(y) fails in Wine
// We NO LONGER do generic transformation - only specific DTArray/STArray patterns
// Generic arr(x)(y) is too dangerous as it breaks function calls
// =============================================================================
std::string SimpleScriptPatcher::Patch2DArrayAccess(const std::string& script) {
    // DISABLED - generic 2D array transformation breaks function calls
    // Only DTArray/STArray are handled specifically in their own functions
    return script;
}

// =============================================================================
// DTArray patterns - Drop Target array compatibility
// Implements the human patching approach:
// 1. Inject DropTarget class
// 2. Convert DT1 = Array(...) to Set DT1 = (new DropTarget)(...)
// 3. Convert DTArray(i)(n) to DTArray(i).property
// =============================================================================
std::string SimpleScriptPatcher::PatchDTArray(const std::string& script) {
    // Check if script uses DTArray with chained access
    if (script.find("DTArray") == std::string::npos) {
        return script;  // No DTArray usage
    }

    // Check for chained access pattern DTArray(i)(n)
    static const RE2 checkPattern(R"(DTArray\s*\([^)]+\)\s*\()");
    if (!RE2Search(script, checkPattern)) {
        return script;  // No chained access pattern
    }

    PLOGI.printf("SimpleScriptPatcher: Detected DTArray(i)(n) pattern - applying class injection");

    std::string r = script;
    int totalCount = 0;

    // Step 1: Convert DT variable Array() initialization to class instantiation
    // Pattern: DT1 = Array(primary, secondary, prim, sw, animate)
    // To: Set DT1 = (new DropTarget)(primary, secondary, prim, sw, animate)
    // Wine compiler fix in compile.c handles (new ClassName)(args) by calling Init explicitly
    // Match DT followed by digits and optional letters (DT1, DT54a) - NOT DTArray!
    // DT\d+\w* matches DT1, DT54, DT54a but not DTArray (which has no digit after DT)
    static const RE2 arrayInit(R"((?im)(^[ \t]*)(DT\d+\w*)\s*=\s*Array\s*\(([^)]+)\))");
    r = RE2ReplaceWithCallback(r, arrayInit, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return m[1] + "Set " + m[2] + " = (new DropTarget)(" + m[3] + ")";
    });

    // Step 2: Convert DTArray(i)(n) to DTArray(i).property
    // Index mapping: 0=primary, 1=secondary, 2=prim, 3=sw, 4=animate, 5=isDropped

    // DTArray(i)(0) -> DTArray(i).primary
    static const RE2 idx0(R"(DTArray\s*\(\s*([^)]+)\s*\)\s*\(\s*0\s*\))");
    r = RE2ReplaceWithCallback(r, idx0, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "DTArray(" + m[1] + ").primary";
    });

    // DTArray(i)(1) -> DTArray(i).secondary
    static const RE2 idx1(R"(DTArray\s*\(\s*([^)]+)\s*\)\s*\(\s*1\s*\))");
    r = RE2ReplaceWithCallback(r, idx1, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "DTArray(" + m[1] + ").secondary";
    });

    // DTArray(i)(2) -> DTArray(i).prim
    static const RE2 idx2(R"(DTArray\s*\(\s*([^)]+)\s*\)\s*\(\s*2\s*\))");
    r = RE2ReplaceWithCallback(r, idx2, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "DTArray(" + m[1] + ").prim";
    });

    // DTArray(i)(3) -> DTArray(i).sw
    static const RE2 idx3(R"(DTArray\s*\(\s*([^)]+)\s*\)\s*\(\s*3\s*\))");
    r = RE2ReplaceWithCallback(r, idx3, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "DTArray(" + m[1] + ").sw";
    });

    // DTArray(i)(4) -> DTArray(i).animate
    static const RE2 idx4(R"(DTArray\s*\(\s*([^)]+)\s*\)\s*\(\s*4\s*\))");
    r = RE2ReplaceWithCallback(r, idx4, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "DTArray(" + m[1] + ").animate";
    });

    // DTArray(i)(5) -> DTArray(i).isDropped
    static const RE2 idx5(R"(DTArray\s*\(\s*([^)]+)\s*\)\s*\(\s*5\s*\))");
    r = RE2ReplaceWithCallback(r, idx5, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "DTArray(" + m[1] + ").isDropped";
    });

    if (totalCount > 0) {
        LogPatch("DTArray: Converted to DropTarget class pattern", totalCount);
        s_needsDropTargetClass = true;
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
    // Check if script uses STArray with chained access
    if (script.find("STArray") == std::string::npos) {
        return script;  // No STArray usage
    }

    // Check for chained access pattern STArray(i)(n)
    static const RE2 checkPattern(R"(STArray\s*\([^)]+\)\s*\()");
    if (!RE2Search(script, checkPattern)) {
        return script;  // No chained access pattern
    }

    PLOGI.printf("SimpleScriptPatcher: Detected STArray(i)(n) pattern - applying class injection");

    std::string r = script;
    int totalCount = 0;

    // Step 1: Convert ST variable Array() initialization to class instantiation
    // Pattern: ST12 = Array(primary, prim, sw, animate, id)
    // To: Set ST12 = (new StandupTarget)(primary, prim, sw, animate, id)
    // Match ST followed by digits and optional letters (ST12, ST18a, ST18b) - NOT STArray!
    // ST\d+\w* matches ST18, ST18a, ST18b but not STArray (which has no digit after ST)
    static const RE2 arrayInit(R"((?im)(^[ \t]*)(ST\d+\w*)\s*=\s*Array\s*\(([^)]+)\))");
    r = RE2ReplaceWithCallback(r, arrayInit, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return m[1] + "Set " + m[2] + " = (new StandupTarget)(" + m[3] + ")";
    });

    // Step 2: Convert STArray(i)(n) to STArray(i).property
    // Index mapping: 0=primary, 1=prim, 2=sw, 3=animate

    // STArray(i)(0) -> STArray(i).primary
    static const RE2 idx0(R"(STArray\s*\(\s*([^)]+)\s*\)\s*\(\s*0\s*\))");
    r = RE2ReplaceWithCallback(r, idx0, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "STArray(" + m[1] + ").primary";
    });

    // STArray(i)(1) -> STArray(i).prim
    static const RE2 idx1(R"(STArray\s*\(\s*([^)]+)\s*\)\s*\(\s*1\s*\))");
    r = RE2ReplaceWithCallback(r, idx1, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "STArray(" + m[1] + ").prim";
    });

    // STArray(i)(2) -> STArray(i).sw
    static const RE2 idx2(R"(STArray\s*\(\s*([^)]+)\s*\)\s*\(\s*2\s*\))");
    r = RE2ReplaceWithCallback(r, idx2, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "STArray(" + m[1] + ").sw";
    });

    // STArray(i)(3) -> STArray(i).animate
    static const RE2 idx3(R"(STArray\s*\(\s*([^)]+)\s*\)\s*\(\s*3\s*\))");
    r = RE2ReplaceWithCallback(r, idx3, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "STArray(" + m[1] + ").animate";
    });

    // STArray(i)(4) -> STArray(i).id
    static const RE2 idx4(R"(STArray\s*\(\s*([^)]+)\s*\)\s*\(\s*4\s*\))");
    r = RE2ReplaceWithCallback(r, idx4, [&totalCount](const RE2Match& m) -> std::string {
        totalCount++;
        return "STArray(" + m[1] + ").id";
    });

    if (totalCount > 0) {
        LogPatch("STArray: Converted to StandupTarget class pattern", totalCount);
        s_needsStandupTargetClass = true;
    }

    return r;
}

// =============================================================================
// Controller.Pause patch - Android/Wine doesn't have Controller before init
// Pattern: Controller.Pause = True/False -> comment out
// =============================================================================
std::string SimpleScriptPatcher::PatchControllerPause(const std::string& script) {
    std::string r = script;
    int count = 0;

    // First handle colon-separated statements (e.g., Sub Foo:Controller.Pause = True:End Sub)
    // These can't be safely commented out, so remove them entirely
    // Pattern matches :Controller.Pause = Value followed by :
    static const RE2 p1(R"((?i):[ \t]*Controller\.Pause\s*=\s*(True|False)[ \t]*:)");
    std::string before = r;
    r = RE2Replace(r, p1, ":");
    if (r != before) count++;

    // Then handle statements on their own lines (comment them out)
    static const RE2 p2(R"((?i)(\s*)(Controller\.Pause\s*=\s*(True|False)))");
    before = r;
    r = RE2Replace(r, p2, "\\1' \\2 ' Disabled for Android");
    if (r != before) count++;

    if (count > 0) {
        LogPatch("Commented out Controller.Pause statements", count);
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
    static const RE2 p(R"(\(Not\s+(\w+)\)\s*\(([^)]+)\))");
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
// Helper function injection
// =============================================================================
std::string SimpleScriptPatcher::InjectHelpers(const std::string& script) {
    std::string helpers = R"(
' ============================================================================
' Wine VBScript Compatibility Helpers (SimpleScriptPatcher)
' ============================================================================

)";

    // Add DropTarget class if needed
    if (s_needsDropTargetClass) {
        helpers += R"(
' DropTarget class - Wine VBScript cannot handle Array(x)(y) syntax
' This class converts DTArray indexed access to property access
Class DropTarget
  Private m_primary, m_secondary, m_prim, m_sw, m_animate, m_isDropped

  Public Property Get Primary(): Set Primary = m_primary: End Property
  Public Property Let Primary(input): Set m_primary = input: End Property

  Public Property Get Secondary(): Set Secondary = m_secondary: End Property
  Public Property Let Secondary(input): Set m_secondary = input: End Property

  Public Property Get Prim(): Set Prim = m_prim: End Property
  Public Property Let Prim(input): Set m_prim = input: End Property

  Public Property Get Sw(): Sw = m_sw: End Property
  Public Property Let Sw(input): m_sw = input: End Property

  Public Property Get Animate(): Animate = m_animate: End Property
  Public Property Let Animate(input): m_animate = input: End Property

  Public Property Get IsDropped(): IsDropped = m_isDropped: End Property
  Public Property Let IsDropped(input): m_isDropped = input: End Property

  Public default Function init(primary, secondary, prim, sw, animate)
    Set m_primary = primary
    Set m_secondary = secondary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_isDropped = False
    Set Init = Me
  End Function
End Class

)";
        PLOGI.printf("SimpleScriptPatcher: Injected DropTarget class");
    }

    // Add StandupTarget class if needed
    if (s_needsStandupTargetClass) {
        helpers += R"(
' StandupTarget class - Wine VBScript cannot handle Array(x)(y) syntax
' This class converts STArray indexed access to property access
' Array format: Array(primary, prim, sw, animate, identifier)
Class StandupTarget
  Private m_primary, m_prim, m_sw, m_animate, m_id

  Public Property Get Primary(): Set Primary = m_primary: End Property
  Public Property Let Primary(input): Set m_primary = input: End Property

  Public Property Get Prim(): Set Prim = m_prim: End Property
  Public Property Let Prim(input): Set m_prim = input: End Property

  Public Property Get Sw(): Sw = m_sw: End Property
  Public Property Let Sw(input): m_sw = input: End Property

  Public Property Get Animate(): Animate = m_animate: End Property
  Public Property Let Animate(input): m_animate = input: End Property

  Public Property Get Id(): Id = m_id: End Property
  Public Property Let Id(input): m_id = input: End Property

  Public default Function init(primary, prim, sw, animate, id)
    Set m_primary = primary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_id = id
    Set Init = Me
  End Function
End Class

)";
        PLOGI.printf("SimpleScriptPatcher: Injected StandupTarget class");
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
std::string SimpleScriptPatcher::PatchScript(const std::string& script) {
    s_patchReport = "";
    s_needsDropTargetClass = false;
    s_needsStandupTargetClass = false;
    PLOGI.printf("SimpleScriptPatcher: ========== STARTING PATCH PROCESS ==========");
    PLOGI.printf("SimpleScriptPatcher: Input script length: %zu characters", script.length());

    std::string result = StripBOM(script);

    // Track if any patches were applied
    std::string original = result;
    bool patched = false;

    // Apply patches in order of likelihood/impact
    result = PatchAlwaysOnTop(result);              // Windows-only PowerShell Sub - stub on Android
    result = PatchWScriptShell(result);             // Windows-only WScript.Shell - disable on Android
    result = PatchMultiplicationInSubCall(result);  // Bug 54177
    result = PatchUBound(result);                    // Bug 54291
    // REMOVED: PatchSingleLineIf - Wine parser now handles these patterns natively
    result = PatchBooleanNot(result);               // Bug 55093
    result = PatchLineContinuation(result);         // Bug 56480
    result = PatchDoubleDot(result);                // Common typo fix
    result = PatchGlfBooleanArray(result);          // GLF Boolean Array Bug
    // REMOVED: PatchInlineStatements - Wine parser now handles these patterns natively
    result = Patch2DArrayAccess(result);            // Bug 53877
    result = PatchDTArray(result);                  // DTArray patterns
    result = PatchSTArray(result);                  // STArray patterns
    // REMOVED: PatchNewClassCall - now handled in Wine VBScript compiler (compile.c)
    result = PatchControllerPause(result);          // Controller.Pause not available on Android
    result = PatchPinUpPlayerFileAccess(result);    // PinUp Player file access not available on Android
    result = PatchParenthesizedNot(result);         // Wine arity bug with (Not Func)(arg)
    // DISABLED: FlexDMD Virtual Segment DMD hides the light-based segments, but FlexDMD can't render overlays on Android
    // result = PatchEnableFlexDMDByDefault(result);   // Enable FlexDMD segment display for GLF tables
    // result = PatchFlexDMDSegments(result);          // Convert .Segments array to string method for Wine

    // Warnings only (no transformation yet)
    PatchDictionaryAccess(result);                  // Bug 58051
    PatchSplitIndexing(result);                     // Bug 58056

    // Check if any patches were applied
    if (result != original) {
        patched = true;
        result = InjectHelpers(result);
    }

    // CRITICAL: Normalize line endings to CRLF AFTER all patching
    // Wine VBScript can fail on mixed line endings
    result = NormalizeLineEndings(result);

    // Sanitize non-ASCII characters (Wine lexer issues with UTF-8)
    result = SanitizeNonAscii(result);

    // Final logging and script dump
    if (patched) {
        PLOGI.printf("SimpleScriptPatcher: ========== PATCHES APPLIED ==========");
        PLOGI.printf("%s", s_patchReport.c_str());
        PLOGI.printf("SimpleScriptPatcher: Output script length: %zu characters", result.length());
        PLOGI.printf("SimpleScriptPatcher: =====================================");

        // Debug: Check if GLF patch survived to the end
        if (result.find("glf_last_isGetRef") != std::string::npos) {
            PLOGI.printf("GLF patch: FINAL CHECK - glf_last_isGetRef PRESENT in result");
        } else {
            PLOGE.printf("GLF patch: FINAL CHECK - glf_last_isGetRef MISSING from result!");
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

#pragma once

#include <string>

#ifdef __STANDALONE__

/**
 * SimpleScriptPatcher - Minimal Wine VBScript compatibility patches
 *
 * This is a simplified alternative to ScriptPatcher that ONLY addresses
 * confirmed Wine VBScript bugs without complex class emulation.
 *
 * Active Wine Bugs Addressed (from vpinball/standalone README):
 * - Bug 53844: Let property with VT_DISPATCH arguments
 * - Bug 54177: Sub call fails when arg has (expr)*number
 * - Bug 54221: GetRef function issues
 * - Bug 54291: UBound on Empty with On Error Resume Next
 * - Bug 55006: Single-line if-else without else body
 * - Bug 55037: Colon on new line after Then
 * - Bug 55093: Boolean conditions without parentheses (partial)
 * - Bug 56480: Underscore line continuation issues
 * - Bug 58051: Direct Dictionary Keys/Items access
 * - Bug 58056: Directly indexing Split() return value
 * - Bug 58248: Me(Idx) syntax fails
 *
 * Also addresses:
 * - Multi-dimensional array access Array(x)(y) - Bug 53877
 * - DTArray/STArray compatibility patterns
 */
class SimpleScriptPatcher
{
public:
    /**
     * Patch a VBScript for Wine/Android compatibility.
     * Returns the patched script.
     */
    static std::string PatchScript(const std::string& script);

    /**
     * Get a report of what was patched (for logging)
     */
    static std::string GetLastPatchReport();

private:
    // BOM handling
    static std::string StripBOM(const std::string& script);

    // Line ending normalization (Wine expects CRLF)
    static std::string NormalizeLineEndings(const std::string& script);

    // Non-ASCII sanitization (Wine lexer issues)
    static std::string SanitizeNonAscii(const std::string& script);

    // Dump script to file for debugging
    static void DumpScript(const std::string& script, const std::string& filename);

    // Bug 54177: Sub call with (expr)*number
    static std::string PatchMultiplicationInSubCall(const std::string& script);

    // Bug 54291: UBound on Empty
    static std::string PatchUBound(const std::string& script);

    // Bug 55006/55037: Single-line If issues
    static std::string PatchSingleLineIf(const std::string& script);

    // Bug 56480: Line continuation before dot
    static std::string PatchLineContinuation(const std::string& script);

    // Bug 58051: Dictionary Keys/Items direct access
    static std::string PatchDictionaryAccess(const std::string& script);

    // Bug 58056: Split() direct indexing
    static std::string PatchSplitIndexing(const std::string& script);

    // Bug 55093: Boolean Not without parentheses
    static std::string PatchBooleanNot(const std::string& script);

    // Double dot typo fix (common in VBS scripts)
    static std::string PatchDoubleDot(const std::string& script);

    // GLF Boolean Array Bug - Wine corrupts VT_BOOL in SAFEARRAY
    static std::string PatchGlfBooleanArray(const std::string& script);

    // Inline statements (Then X End If on same line)
    static std::string PatchInlineStatements(const std::string& script);

    // Multi-dimensional array access Array(x)(y)
    static std::string Patch2DArrayAccess(const std::string& script);

    // DTArray/STArray patterns
    static std::string PatchDTArray(const std::string& script);
    static std::string PatchSTArray(const std::string& script);

    // Controller.Pause - not supported on Android (no Controller object before init)
    static std::string PatchControllerPause(const std::string& script);

    // Parenthesized Not function calls - Wine arity mismatch bug
    // Pattern: (Not IsNull)(x) -> Not IsNull(x)
    static std::string PatchParenthesizedNot(const std::string& script);

    // Enable FlexDMD Virtual Segment DMD by default for GLF tables
    static std::string PatchEnableFlexDMDByDefault(const std::string& script);

    // Convert FlexDMD .Segments array assignment to string method for Wine compatibility
    static std::string PatchFlexDMDSegments(const std::string& script);

    // Inject helper functions
    static std::string InjectHelpers(const std::string& script);

    // Track what was patched
    static std::string s_patchReport;
    static bool s_needsDropTargetClass;
    static bool s_needsStandupTargetClass;
    static void LogPatch(const std::string& description, int count = 1);
};

#endif // __STANDALONE__

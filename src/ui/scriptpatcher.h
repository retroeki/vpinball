#pragma once

#include <string>
#include <regex>

#ifdef __STANDALONE__

/**
 * VBScript Patcher for Wine/Android compatibility
 *
 * Wine's VBScript engine cannot handle multi-dimension array assignments like:
 *   DTArray(i)(4) = DTAnimate(...)
 *   STArray(i)(3) = STCheckHit(...)
 *
 * This patcher automatically rewrites scripts to use class-based patterns instead,
 * allowing tables that use these patterns to work on Android/Linux/Wine.
 */
class ScriptPatcher
{
public:
    /**
     * Patch a VBScript to fix Wine VBScript engine incompatibilities.
     *
     * @param script The original VBScript source
     * @return The patched VBScript source
     */
    static std::string PatchScript(const std::string& script);

private:
    /**
     * Check if script uses DTArray pattern
     */
    static bool UsesDTArray(const std::string& script);

    /**
     * Check if script uses STArray pattern
     */
    static bool UsesSTArray(const std::string& script);

    /**
     * Inject the DropTarget class definition at the start of the script
     */
    static std::string InjectDropTargetClass(const std::string& script);

    /**
     * Inject the StandupTarget class definition at the start of the script
     */
    static std::string InjectStandupTargetClass(const std::string& script);

    /**
     * Convert DTxx = Array(...) to Set DTxx = (new DropTarget)(...)
     */
    static std::string PatchDTArrayDefinitions(const std::string& script);

    /**
     * Convert STxx = Array(...) to Set STxx = (new StandupTarget)(...)
     */
    static std::string PatchSTArrayDefinitions(const std::string& script);

    /**
     * Replace DTArray(i)(N) with DTArray(i).propertyName
     * Also handles DTArray(ind)(N), DTArray(x)(N), etc.
     */
    static std::string PatchDTArrayAccess(const std::string& script);

    /**
     * Replace STArray(i)(N) with STArray(i).propertyName
     */
    static std::string PatchSTArrayAccess(const std::string& script);

    /**
     * Check if script uses Controller.Pause
     */
    static bool UsesControllerPause(const std::string& script);

    /**
     * Comment out Controller.Pause lines (not supported on Wine/standalone)
     */
    static std::string PatchControllerPause(const std::string& script);

    /**
     * Check if script has PuPlayer.playstop pDMD in playclear function
     */
    static bool UsesPuPlayerPlaystopInPlayclear(const std::string& script);

    /**
     * Comment out PuPlayer.playstop pDMD in playclear (stops background video on Android)
     */
    static std::string PatchPuPlayerPlaystopInPlayclear(const std::string& script);

    /**
     * Strip BOM (Byte Order Mark) from start of script
     * Fixes scripts that fail at line 1 due to UTF-8/UTF-16 BOM
     */
    static std::string StripBOM(const std::string& script);

    /**
     * Fix AddScore parentheses for Wine VBScript compatibility
     * Game of Thrones: AddScore (expr)+expr2 -> AddScore ((expr)+expr2)
     */
    static std::string PatchAddScoreParentheses(const std::string& script);

    /**
     * Fix SetAlignedPosition parentheses for Wine VBScript compatibility
     * Game of Thrones: .SetAlignedPosition ((i-1)*25)+14,... -> .SetAlignedPosition (((i-1)*25)+14),...
     */
    static std::string PatchSetAlignedPositionParentheses(const std::string& script);

    /**
     * Check if script has SlingshotCorrection class (VPW tables)
     * Wine VBScript engine cannot parse certain class constructs
     */
    static bool UsesSlingshotCorrection(const std::string& script);

    /**
     * Comment out SlingshotCorrection class and related code
     * LOTR Valinor Edition and other VPW tables use this
     */
    static std::string PatchSlingshotCorrection(const std::string& script);


    /**
     * Fix line continuation before dot (Wine Bug 56480)
     * Transforms: obj _
.Method() -> obj. _
Method()
     */
    static std::string PatchLineContinuationBeforeDot(const std::string& script);

    /**
     * Fix single-line If...Then...Else without body after Else (Wine Bug 55006)
     * Transforms: If x Then DoSomething() Else -> If x Then DoSomething() Else:
     */
    static std::string PatchSingleLineIfElse(const std::string& script);

    /**
     * Fix Execute statements with eval that may reference non-existent objects
     * Wraps in IsObject check for Wine compatibility
     */
    static std::string PatchExecuteEval(const std::string& script);

    /**
     * Fix string concatenation where first operand is numeric expression
     * Transforms: (expr) & " text" -> "" & (expr) & " text"
     */
    static std::string PatchStringConcatenation(const std::string& script);

    // Class definition strings
    static const char* DROP_TARGET_CLASS;
    static const char* STANDUP_TARGET_CLASS;
};

#endif // __STANDALONE__

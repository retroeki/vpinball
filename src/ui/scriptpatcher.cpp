#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include <regex>
#include <sstream>

// DropTarget class definition - matches official vpx-standalone-scripts format
const char* ScriptPatcher::DROP_TARGET_CLASS = R"(
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

  Public default Function init(primary, secondary, prim, sw, animate, isDropped)
    Set m_primary = primary
    Set m_secondary = secondary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_isDropped = isDropped
    Set Init = Me
  End Function
End Class

)";

// StandupTarget class definition - includes Target property (5 properties total)
const char* ScriptPatcher::STANDUP_TARGET_CLASS = R"(
Class StandupTarget
  Private m_primary, m_prim, m_sw, m_animate, m_target

  Public Property Get Primary(): Set Primary = m_primary: End Property
  Public Property Let Primary(input): Set m_primary = input: End Property

  Public Property Get Prim(): Set Prim = m_prim: End Property
  Public Property Let Prim(input): Set m_prim = input: End Property

  Public Property Get Sw(): Sw = m_sw: End Property
  Public Property Let Sw(input): m_sw = input: End Property

  Public Property Get Animate(): Animate = m_animate: End Property
  Public Property Let Animate(input): m_animate = input: End Property

  Public Property Get Target(): Target = m_target: End Property
  Public Property Let Target(input): m_target = input: End Property

  Public default Function init(primary, prim, sw, animate, target)
    Set m_primary = primary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_target = target
    Set Init = Me
  End Function
End Class

)";

bool ScriptPatcher::UsesDTArray(const std::string& script)
{
    // Check for DTArray access pattern: DTArray(something)(number)
    std::regex pattern(R"(DTArray\s*\(\s*\w+\s*\)\s*\(\s*\d+\s*\))", std::regex::icase);
    return std::regex_search(script, pattern);
}

bool ScriptPatcher::UsesSTArray(const std::string& script)
{
    // Check for STArray access pattern: STArray(something)(number)
    std::regex pattern(R"(STArray\s*\(\s*\w+\s*\)\s*\(\s*\d+\s*\))", std::regex::icase);
    return std::regex_search(script, pattern);
}

std::string ScriptPatcher::InjectDropTargetClass(const std::string& script)
{
    // Check if class already exists
    std::regex existingClass(R"(Class\s+DropTarget)", std::regex::icase);
    if (std::regex_search(script, existingClass))
        return script;

    // Find first DT definition: DT<digits> = Array(...)
    // Insert class RIGHT BEFORE this line (like official patches do)
    std::regex firstDTDef(R"((\r?\n)([ \t]*)(DT\d+\s*=\s*Array\s*\())", std::regex::icase);
    std::smatch match;

    if (std::regex_search(script, match, firstDTDef))
    {
        size_t insertPos = match.position();
        std::string indent = match[2].str();
        return script.substr(0, insertPos) + "\n" + std::string(DROP_TARGET_CLASS) + script.substr(insertPos);
    }

    // Fallback: inject after Option Explicit
    std::regex optionExplicit(R"(^\s*Option\s+Explicit\s*$)", std::regex::icase | std::regex::multiline);
    if (std::regex_search(script, match, optionExplicit))
    {
        size_t insertPos = match.position() + match.length();
        return script.substr(0, insertPos) + "\n" + std::string(DROP_TARGET_CLASS) + script.substr(insertPos);
    }

    return std::string(DROP_TARGET_CLASS) + script;
}

std::string ScriptPatcher::InjectStandupTargetClass(const std::string& script)
{
    // Check if class already exists
    std::regex existingClass(R"(Class\s+StandupTarget)", std::regex::icase);
    if (std::regex_search(script, existingClass))
        return script;

    // Find first ST definition: ST<digits> = Array(...)
    // Insert class RIGHT BEFORE this line
    std::regex firstSTDef(R"((\r?\n)([ \t]*)(ST\d+\s*=\s*Array\s*\())", std::regex::icase);
    std::smatch match;

    if (std::regex_search(script, match, firstSTDef))
    {
        size_t insertPos = match.position();
        return script.substr(0, insertPos) + "\n" + std::string(STANDUP_TARGET_CLASS) + script.substr(insertPos);
    }

    // Fallback: inject after Option Explicit
    std::regex optionExplicit(R"(^\s*Option\s+Explicit\s*$)", std::regex::icase | std::regex::multiline);
    if (std::regex_search(script, match, optionExplicit))
    {
        size_t insertPos = match.position() + match.length();
        return script.substr(0, insertPos) + "\n" + std::string(STANDUP_TARGET_CLASS) + script.substr(insertPos);
    }

    return std::string(STANDUP_TARGET_CLASS) + script;
}

std::string ScriptPatcher::PatchDTArrayDefinitions(const std::string& script)
{
    std::string result = script;

    // Match: DT<digits> = Array(...)
    // Replace with: Set DT<digits> = (new DropTarget)(..., false)
    // The (new DropTarget)(...) syntax calls the default function
    // We need to add 'false' as 6th param for isDropped if not present

    // First, handle definitions with 5 params (add false for isDropped)
    std::regex dtDef5Params(
        R"(\b(DT\d+)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))",
        std::regex::icase
    );
    result = std::regex_replace(result, dtDef5Params, "Set $1 = (new DropTarget)($2, $3, $4, $5, $6, false)");

    // Then handle definitions with 6 params (already has isDropped)
    std::regex dtDef6Params(
        R"(\b(DT\d+)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))",
        std::regex::icase
    );
    result = std::regex_replace(result, dtDef6Params, "Set $1 = (new DropTarget)($2, $3, $4, $5, $6, $7)");

    return result;
}

std::string ScriptPatcher::PatchSTArrayDefinitions(const std::string& script)
{
    std::string result = script;

    // Match: ST<digits> = Array(...)
    // Replace with: Set ST<digits> = (new StandupTarget)(...)
    // StandupTarget has 5 params: primary, prim, sw, animate, target
    std::regex stDefPattern(
        R"(\b(ST\d+)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))",
        std::regex::icase
    );
    result = std::regex_replace(result, stDefPattern, "Set $1 = (new StandupTarget)($2, $3, $4, $5, $6)");

    return result;
}

std::string ScriptPatcher::PatchDTArrayAccess(const std::string& script)
{
    std::string result = script;

    // Property mappings for DTArray (matching official patches):
    // (0) -> .primary
    // (1) -> .secondary
    // (2) -> .prim
    // (3) -> .sw
    // (4) -> .animate
    // (5) -> .isDropped

    std::regex dt0(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*0\s*\))", std::regex::icase);
    result = std::regex_replace(result, dt0, "DTArray($1).primary");

    std::regex dt1(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*1\s*\))", std::regex::icase);
    result = std::regex_replace(result, dt1, "DTArray($1).secondary");

    std::regex dt2(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\))", std::regex::icase);
    result = std::regex_replace(result, dt2, "DTArray($1).prim");

    std::regex dt3(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\))", std::regex::icase);
    result = std::regex_replace(result, dt3, "DTArray($1).sw");

    std::regex dt4(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\))", std::regex::icase);
    result = std::regex_replace(result, dt4, "DTArray($1).animate");

    std::regex dt5(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*5\s*\))", std::regex::icase);
    result = std::regex_replace(result, dt5, "DTArray($1).isDropped");

    return result;
}

std::string ScriptPatcher::PatchSTArrayAccess(const std::string& script)
{
    std::string result = script;

    // Property mappings for STArray (matching official patches):
    // (0) -> .primary
    // (1) -> .prim
    // (2) -> .sw
    // (3) -> .animate
    // (4) -> .target

    std::regex st0(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*0\s*\))", std::regex::icase);
    result = std::regex_replace(result, st0, "STArray($1).primary");

    std::regex st1(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*1\s*\))", std::regex::icase);
    result = std::regex_replace(result, st1, "STArray($1).prim");

    std::regex st2(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\))", std::regex::icase);
    result = std::regex_replace(result, st2, "STArray($1).sw");

    std::regex st3(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\))", std::regex::icase);
    result = std::regex_replace(result, st3, "STArray($1).animate");

    std::regex st4(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\))", std::regex::icase);
    result = std::regex_replace(result, st4, "STArray($1).target");

    return result;
}

std::string ScriptPatcher::PatchControllerPause(const std::string& script)
{
    std::string result = script;

    // Controller.Pause is not supported on Wine/standalone
    // Comment out these lines: Controller.Pause = True/False
    std::regex controllerPause(R"((\s*)(Controller\.Pause\s*=\s*(True|False)))", std::regex::icase);
    result = std::regex_replace(result, controllerPause, "$1' $2 ' Commented out for Wine/Android compatibility");

    return result;
}

bool ScriptPatcher::UsesControllerPause(const std::string& script)
{
    std::regex pattern(R"(Controller\.Pause\s*=)", std::regex::icase);
    return std::regex_search(script, pattern);
}

bool ScriptPatcher::UsesPuPlayerPlaystopInPlayclear(const std::string& script)
{
    // Check for pattern: if chan = pBackglass Then ... PuPlayer.playstop pDMD
    // This stops DMD when clearing backglass, which breaks Android PUP display
    std::regex pattern(R"(if\s+chan\s*=\s*pBackglass\s+Then[\s\S]*?PuPlayer\.playstop\s+pDMD)", std::regex::icase);
    return std::regex_search(script, pattern);
}

std::string ScriptPatcher::PatchPuPlayerPlaystopInPlayclear(const std::string& script)
{
    std::string result = script;

    // Comment out PuPlayer.playstop pDMD when it's inside if chan = pBackglass block
    // Pattern: if chan = pBackglass Then <newline+whitespace> PuPlayer.playstop pDMD
    std::regex pattern(R"((if\s+chan\s*=\s*pBackglass\s+Then\s*[\r\n]+)([ \t]*)(PuPlayer\.playstop\s+pDMD))", std::regex::icase);
    result = std::regex_replace(result, pattern, "$1$2' $3 ' Commented out for Android - stops background video");

    return result;
}

std::string ScriptPatcher::StripBOM(const std::string& script)
{
    // UTF-8 BOM is EF BB BF (3 bytes)
    if (script.length() >= 3 &&
        (unsigned char)script[0] == 0xEF &&
        (unsigned char)script[1] == 0xBB &&
        (unsigned char)script[2] == 0xBF)
    {
        PLOGI.printf("ScriptPatcher: Stripping UTF-8 BOM from script");
        return script.substr(3);
    }
    // UTF-16 LE BOM is FF FE
    if (script.length() >= 2 &&
        (unsigned char)script[0] == 0xFF &&
        (unsigned char)script[1] == 0xFE)
    {
        PLOGI.printf("ScriptPatcher: Stripping UTF-16 LE BOM from script");
        return script.substr(2);
    }
    // UTF-16 BE BOM is FE FF
    if (script.length() >= 2 &&
        (unsigned char)script[0] == 0xFE &&
        (unsigned char)script[1] == 0xFF)
    {
        PLOGI.printf("ScriptPatcher: Stripping UTF-16 BE BOM from script");
        return script.substr(2);
    }
    return script;
}

std::string ScriptPatcher::PatchAddScoreParentheses(const std::string& script)
{
    std::string result = script;

    // Fix: AddScore (expr1)+expr2 -> AddScore ((expr1)+expr2)
    // Game of Thrones has: AddScore (BonusCnt * BonusMultiplier(CurrentPlayer))+BonusHeldPoints(CurrentPlayer)
    // This pattern needs outer parentheses for correct parsing
    std::regex addScorePattern(
        R"(AddScore\s+\(([^)]+\([^)]*\)[^)]*)\)\s*\+\s*(\w+\([^)]*\)))",
        std::regex::icase
    );
    result = std::regex_replace(result, addScorePattern, "AddScore (($1)+$2)");

    return result;
}

std::string ScriptPatcher::PatchSetAlignedPositionParentheses(const std::string& script)
{
    std::string result = script;

    // Fix: .SetAlignedPosition ((i-1)*25)+14,... -> .SetAlignedPosition (((i-1)*25)+14),...
    // The first argument needs to be fully parenthesized
    std::regex setAlignedPattern(
        R"(\.SetAlignedPosition\s+\(\(([^)]+)\)\*(\d+)\)\+(\d+)\s*,)",
        std::regex::icase
    );
    result = std::regex_replace(result, setAlignedPattern, ".SetAlignedPosition ((($1)*$2)+$3),");

    return result;
}

bool ScriptPatcher::UsesSlingshotCorrection(const std::string& script)
{
    // Check for VPW physics classes that Wine VBScript engine can't parse
    // These include SlingshotCorrection, FlipperPolarity, and other nFozzy/VPW classes
    std::regex pattern(R"(Class\s+(SlingshotCorrection|FlipperPolarity|FlipperPhysics|BumperPhysics))", std::regex::icase);
    return std::regex_search(script, pattern);
}

std::string ScriptPatcher::PatchSlingshotCorrection(const std::string& script)
{
    std::string result = script;

    // Comment out the entire SlingshotCorrection class from "Class SlingshotCorrection" to "End Class"
    // This class is a physics enhancement for slingshots in VPW tables that Wine can't parse
    // The table will still work without it - slingshots will just use default physics

    // Pattern to match the entire class block (multiline)
    // We use a line-by-line approach since the class can span many lines
    std::istringstream stream(result);
    std::ostringstream output;
    std::string line;
    bool inVPWClass = false;
    int classNestLevel = 0;

    while (std::getline(stream, line))
    {
        // Check if we're starting the SlingshotCorrection class
        std::regex classStart(R"(^\s*Class\s+(SlingshotCorrection|FlipperPolarity|FlipperPhysics|BumperPhysics))", std::regex::icase);
        if (!inVPWClass && std::regex_search(line, classStart))
        {
            inVPWClass = true;
            classNestLevel = 1;
            output << "' " << line << " ' Commented out for Wine/Android compatibility\n";
            continue;
        }

        if (inVPWClass)
        {
            // Check for nested Class statements
            std::regex nestedClass(R"(^\s*Class\s+\w+)", std::regex::icase);
            if (std::regex_search(line, nestedClass))
            {
                classNestLevel++;
            }

            // Check for End Class
            std::regex endClass(R"(^\s*End\s+Class)", std::regex::icase);
            if (std::regex_search(line, endClass))
            {
                classNestLevel--;
                if (classNestLevel == 0)
                {
                    inVPWClass = false;
                    output << "' " << line << " ' End SlingshotCorrection\n";
                    continue;
                }
            }

            // Comment out all lines inside the class
            output << "' " << line << "\n";
        }
        else
        {
            output << line << "\n";
        }
    }

    // Also comment out any references to SlingshotCorrection instances
    result = output.str();

    // Comment out: Set xxx = New <VPWClass>
    std::regex newVPWClass(R"((\s*)(Set\s+\w+\s*=\s*New\s+(SlingshotCorrection|FlipperPolarity|FlipperPhysics|BumperPhysics)))", std::regex::icase);
    result = std::regex_replace(result, newVPWClass, "$1' $2 ' Commented out - class disabled");

    // Comment out: Dim xxx As <VPWClass> or Dim xxx As New <VPWClass>
    std::regex dimVPWClass(R"((\s*)(Dim\s+\w+\s+As\s+(New\s+)?(SlingshotCorrection|FlipperPolarity|FlipperPhysics|BumperPhysics)))", std::regex::icase);
    result = std::regex_replace(result, dimVPWClass, "$1' $2 ' Commented out - class disabled");

    return result;
}

std::string ScriptPatcher::PatchScript(const std::string& script)
{
    // First strip any BOM - this is critical for scripts that fail at line 1
    std::string result = StripBOM(script);
    bool patched = result.length() != script.length(); // Track if BOM was stripped

    PLOGI.printf("ScriptPatcher: Checking script (length=%zu)", result.length());

    // Check and patch DTArray patterns
    if (UsesDTArray(result))
    {
        PLOGI.printf("ScriptPatcher: Detected DTArray pattern, applying Wine compatibility patches");
        result = InjectDropTargetClass(result);
        result = PatchDTArrayDefinitions(result);
        result = PatchDTArrayAccess(result);
        patched = true;
    }

    // Check and patch STArray patterns
    if (UsesSTArray(result))
    {
        PLOGI.printf("ScriptPatcher: Detected STArray pattern, applying Wine compatibility patches");
        result = InjectStandupTargetClass(result);
        result = PatchSTArrayDefinitions(result);
        result = PatchSTArrayAccess(result);
        patched = true;
    }

    // Check and patch Controller.Pause (not supported on Wine/standalone)
    if (UsesControllerPause(result))
    {
        PLOGI.printf("ScriptPatcher: Detected Controller.Pause, commenting out for Wine compatibility");
        result = PatchControllerPause(result);
        patched = true;
    }

    // Check and patch PuPlayer.playstop pDMD in playclear (stops background video on Android)
    if (UsesPuPlayerPlaystopInPlayclear(result))
    {
        PLOGI.printf("ScriptPatcher: Detected PuPlayer.playstop pDMD in playclear, commenting out for Android");
        result = PatchPuPlayerPlaystopInPlayclear(result);
        patched = true;
    }

    // Always apply operator precedence patches (Game of Thrones and similar tables)
    {
        std::string before = result;
        result = PatchAddScoreParentheses(result);
        if (result != before)
        {
            PLOGI.printf("ScriptPatcher: Fixed AddScore parentheses for Wine compatibility");
            patched = true;
        }
    }

    {
        std::string before = result;
        result = PatchSetAlignedPositionParentheses(result);
        if (result != before)
        {
            PLOGI.printf("ScriptPatcher: Fixed SetAlignedPosition parentheses for Wine compatibility");
            patched = true;
        }
    }


    // Check and patch SlingshotCorrection class (VPW tables like LOTR Valinor)
    if (UsesSlingshotCorrection(result))
    {
        PLOGI.printf("ScriptPatcher: Detected VPW physics classes, commenting out for Wine compatibility");
        result = PatchSlingshotCorrection(result);
        patched = true;
    }

    if (patched)
    {
        PLOGI.printf("ScriptPatcher: Script patching complete (new length=%zu)", result.length());
    }

    return result;
}

#endif // __STANDALONE__

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <regex>

#ifdef __STANDALONE__

// ============================================================================
// VBScript Class Emulation - Data Structures
// ============================================================================
//
// PROBLEM:
// Wine's VBScript engine doesn't support the "Class" keyword. Tables using
// VPW physics classes (SlingshotCorrection, FlipperPolarity, etc.) fail.
//
// SOLUTION:
// Transform VBScript classes into Dictionary-based objects that Wine can handle.
//
// HOW IT WORKS:
//
//   ORIGINAL:                          TRANSFORMED:
//   ---------                          ------------
//   Class Foo                          Function Foo_Create()
//       Public Value                       Dim this_
//       Public Sub Init(x)                 Set this_ = CreateObject("Scripting.Dictionary")
//           Value = x                      this_("Value") = Empty
//       End Sub                            Set Foo_Create = this_
//   End Class                          End Function
//
//   Set obj = New Foo                  Sub Foo_Init(this_, x)
//   obj.Init 5                             this_("Value") = x
//   x = obj.Value                      End Sub
//
//                                      Set obj = Foo_Create()
//                                      Foo_Init obj, 5
//                                      x = obj("Value")
//
// ============================================================================

// A class property (member variable)
struct VBClassProperty {
    std::string name;
    bool isPublic = false;
    bool isArray = false;  // true if declared with () - e.g., "Private ballvel()"
    int arraySize = -1;    // -1 = dynamic, 0+ = fixed size (e.g., "Private arr(300)" -> 300)
};

// A class method (Sub or Function)
struct VBClassMethod {
    std::string name;
    bool isPublic;
    bool isFunction;                       // true = Function, false = Sub
    bool isDefault;                        // "Public Default Function"
    std::vector<std::string> params;
    std::string body;                      // Everything between declaration and End Sub/Function
};

// Property Get/Let/Set accessor
struct VBClassAccessor {
    std::string name;
    std::string type;                      // "Get", "Let", or "Set"
    std::vector<std::string> params;
    std::string body;
};

// Complete parsed class
struct VBClassDefinition {
    std::string name;
    std::vector<VBClassProperty> properties;
    std::vector<VBClassMethod> methods;
    std::vector<VBClassAccessor> accessors;
    std::string initializeBody;            // Class_Initialize content
    std::string terminateBody;             // Class_Terminate content
    size_t startPos;                       // Position in script where class starts
    size_t endPos;                         // Position where class ends
};

/**
 * VBScript Patcher for Wine/Android compatibility
 *
 * Handles two main issues:
 * 1. Multi-dimension array assignments (DTArray, STArray patterns)
 * 2. VBScript Class keyword (not supported by Wine)
 */
class ScriptPatcher
{
public:
    /**
     * Patch a VBScript to fix Wine VBScript engine incompatibilities.
     */
    static std::string PatchScript(const std::string& script);

private:
    // ========================================================================
    // Class Emulation - Phase 1: Parsing
    // ========================================================================

    /**
     * Check if script contains any VBScript Class definitions
     */
    static bool HasClassDefinitions(const std::string& script);

    /**
     * Parse all Class definitions from the script.
     * Returns a vector of parsed classes with their properties, methods, etc.
     */
    static std::vector<VBClassDefinition> ParseClassDefinitions(const std::string& script);

    /**
     * Parse parameters from a method/function declaration.
     * Input: "ByVal x, y, ByRef z" -> ["x", "y", "z"]
     */
    static std::vector<std::string> ParseParameters(const std::string& paramStr);

    // ========================================================================
    // Class Emulation - Phase 2: Code Generation
    // ========================================================================

    /**
     * Generate Wine-compatible emulation code for a parsed class.
     * Creates: ClassName_Create(), ClassName_MethodName(), etc.
     */
    static std::string EmitClassEmulation(const VBClassDefinition& classDef);

    /**
     * Transform method body: replace Me.X with this_("X"), etc.
     * @param methodParams - parameter names to exclude from transformation
     */
    static std::string TransformMethodBody(const std::string& body,
                                           const VBClassDefinition& classDef,
                                           const std::vector<std::string>& methodParams = {});

    // ========================================================================
    // Class Emulation - Phase 3: Usage Transformation
    // ========================================================================

    /**
     * Transform the entire script to use emulated classes.
     * Replaces class definitions with emulation code and transforms usage sites.
     */
    static std::string EmulateClasses(const std::string& script);

    /**
     * Transform "Set x = New ClassName" -> "Set x = ClassName_Create()"
     * Also handles "(new ClassName)(args)" -> "ClassName_defaultMethod(ClassName_Create(), args)"
     */
    static std::string TransformNewStatements(const std::string& script,
                                              const std::vector<VBClassDefinition>& classes);

    /**
     * Transform method calls: obj.Method(args) -> ClassName_Method(obj, args)
     * Requires tracking which variables hold which class types.
     */
    static std::string TransformMethodCalls(const std::string& script,
                                            const std::vector<VBClassDefinition>& classes);

    /**
     * Transform property access: obj.Property -> obj("Property")
     */
    static std::string TransformPropertyAccess(const std::string& script,
                                               const std::vector<VBClassDefinition>& classes);

    /**
     * Transform accessor access: obj.accessor(idx) -> ClassName_Get/Let_accessor(obj, idx, ...)
     */
    static std::string TransformAccessorAccess(const std::string& script,
                                               const std::vector<VBClassDefinition>& classes);

    // ========================================================================
    // Existing Patches (DTArray, STArray, etc.)
    // ========================================================================

    static bool UsesDTArray(const std::string& script);
    static bool UsesSTArray(const std::string& script);
    static std::string InjectDropTargetClass(const std::string& script);
    static std::string InjectStandupTargetClass(const std::string& script);
    static std::string PatchDTArrayDefinitions(const std::string& script);
    static std::string PatchSTArrayDefinitions(const std::string& script);
    static std::string PatchDTArrayAccess(const std::string& script);
    static std::string PatchSTArrayAccess(const std::string& script);

    static bool UsesControllerPause(const std::string& script);
    static std::string PatchControllerPause(const std::string& script);

    static bool UsesPuPlayerPlaystopInPlayclear(const std::string& script);
    static std::string PatchPuPlayerPlaystopInPlayclear(const std::string& script);

    static std::string StripBOM(const std::string& script);
    static std::string PatchAddScoreParentheses(const std::string& script);
    static std::string PatchSetAlignedPositionParentheses(const std::string& script);

    static bool UsesSlingshotCorrection(const std::string& script);
    static std::string PatchSlingshotCorrection(const std::string& script);

    static std::string PatchLineContinuationBeforeDot(const std::string& script);
    static std::string PatchSingleLineIfElse(const std::string& script);
    static std::string PatchNestedSingleLineIf(const std::string& script);
    static std::string PatchSingleLineIfEndIf(const std::string& script);
    static std::string PatchExecuteEval(const std::string& script);
    static std::string PatchStringConcatenation(const std::string& script);

    // Wine VBScript Array Compatibility
    static bool UsesProblematicArrays(const std::string& script);
    static std::string InjectWineArrayHelpers(const std::string& script);
    static std::string PatchUBoundInConditions(const std::string& script);
    static std::string PatchUBoundInForLoops(const std::string& script);
    static std::string PatchAllUBound(const std::string& script);
    static std::string PatchSafeUBoundArrayAccess(const std::string& script);
    static std::string PatchLinearEnvelopeGuard(const std::string& script);
    static std::string PatchBallArrayAccess(const std::string& script);
    static std::string PatchBallLoopGuard(const std::string& script);
    static std::string PatchReDimWithUBound(const std::string& script);
    static std::string Patch2DArrayAccess(const std::string& script);
    static std::string PatchArrayElementAssignment(const std::string& script);
    static std::string PatchNestedArrayAssignment(const std::string& script);
    static std::string PatchDictArrayAccess(const std::string& script);
    static std::string PatchArrayObjectPropertyAccess(const std::string& script);
    static std::string PatchArrayObjectPropertyRead(const std::string& script);
    static std::string InjectVPXSetArrObjProp(const std::string& script);
    static std::string RemoveUnusedClasses(const std::string& script);
    static std::string RemoveDuplicateVpmInit(const std::string& script);

    // Native class protection - classes that interact with external code via Me
    // These must be kept 100% native with no transformations
    struct NativeClassInfo {
        std::string name;
        std::string fullText;  // Complete "Class ... End Class" definition
        std::string placeholder;
    };
    static std::vector<NativeClassInfo> ExtractNativeClasses(std::string& script);
    static std::string RestoreNativeClasses(const std::string& script,
                                             const std::vector<NativeClassInfo>& nativeClasses);

    // Class definition strings for DTArray/STArray
    static const char* DROP_TARGET_CLASS;
    static const char* STANDUP_TARGET_CLASS;
};

#endif // __STANDALONE__

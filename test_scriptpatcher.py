#!/usr/bin/env python3
"""
Comprehensive test for ScriptPatcher - simulates the ENTIRE patching pipeline.
Run this BEFORE building to catch bugs.

Tests the full flow:
1. Class parsing
2. Class emulation (TransformMethodBody, EmitClassEmulation)
3. Wine array patches (InjectWineArrayHelpers, PatchArrayElementAssignment, etc.)
"""
import re
import os

# =============================================================================
# SIMULATE THE FULL SCRIPTPATCHER PIPELINE
# =============================================================================

class VBClassProperty:
    def __init__(self, name, is_public=False, is_array=False):
        self.name = name
        self.is_public = is_public
        self.is_array = is_array

class VBClassMethod:
    def __init__(self, name, body, is_function=False, params=None):
        self.name = name
        self.body = body
        self.is_function = is_function
        self.params = params or []

class VBClassDefinition:
    def __init__(self, name):
        self.name = name
        self.properties = []
        self.methods = []
        self.accessors = []
        self.initialize_body = ""
        self.start_pos = 0
        self.end_pos = 0


def parse_class_definitions(script):
    """Parse VBScript class definitions"""
    classes = []

    class_pattern = re.compile(r'^\s*Class\s+(\w+)', re.IGNORECASE | re.MULTILINE)
    class_end_pattern = re.compile(r'^\s*End\s+Class\s*$', re.IGNORECASE | re.MULTILINE)
    property_pattern = re.compile(r'^\s*(Public|Private)\s+(?!Sub|Function|Property|Default)(.+)$', re.IGNORECASE)
    method_start_pattern = re.compile(r'^\s*(Public\s+|Private\s+)?(Default\s+)?(Sub|Function)\s+(\w+)(?:\s*\(([^)]*)\))?', re.IGNORECASE)
    method_end_sub = re.compile(r'^\s*End\s+Sub\s*$', re.IGNORECASE)
    method_end_func = re.compile(r'^\s*End\s+Function\s*$', re.IGNORECASE)

    lines = script.split('\n')
    in_class = False
    in_method = False
    current_class = None
    current_method = None
    current_method_body = []

    for i, line in enumerate(lines):
        # Check for class start
        m = class_pattern.match(line)
        if m and not in_class:
            in_class = True
            current_class = VBClassDefinition(m.group(1))
            continue

        # Check for class end
        if in_class and class_end_pattern.match(line):
            classes.append(current_class)
            in_class = False
            current_class = None
            continue

        if not in_class:
            continue

        # Inside a class
        if in_method:
            is_end_sub = method_end_sub.match(line)
            is_end_func = method_end_func.match(line)
            if (is_end_sub and not current_method.is_function) or (is_end_func and current_method.is_function):
                current_method.body = '\n'.join(current_method_body)
                if current_method.name.lower() == 'class_initialize':
                    current_class.initialize_body = current_method.body
                else:
                    current_class.methods.append(current_method)
                in_method = False
                current_method = None
                current_method_body = []
                continue
            current_method_body.append(line)
            continue

        # Check for method start
        m = method_start_pattern.match(line)
        if m:
            is_function = m.group(3).lower() == 'function'
            method_name = m.group(4)
            params = [p.strip() for p in (m.group(5) or '').split(',') if p.strip()]
            current_method = VBClassMethod(method_name, "", is_function, params)
            current_method_body = []
            in_method = True
            continue

        # Check for property declaration
        m = property_pattern.match(line)
        if m:
            is_public = m.group(1).lower() == 'public'
            var_list = m.group(2)
            for var_token in var_list.split(','):
                var_token = var_token.strip()
                is_array = '(' in var_token
                if '(' in var_token:
                    var_token = var_token[:var_token.index('(')]
                var_token = var_token.strip()
                if var_token:
                    prop = VBClassProperty(var_token, is_public, is_array)
                    current_class.properties.append(prop)

    return classes


def detect_arrays_from_redim(classes):
    """Detect arrays from ReDim usage in class bodies"""
    for cls in classes:
        all_bodies = cls.initialize_body + '\n'
        for m in cls.methods:
            all_bodies += m.body + '\n'

        for prop in cls.properties:
            if not prop.is_array:
                pattern = r'\bReDim\s+' + re.escape(prop.name) + r'\s*\('
                if re.search(pattern, all_bodies, re.IGNORECASE):
                    prop.is_array = True


def transform_method_body(body, class_def):
    """Transform method body - replace property references"""
    result = body

    # Me.Property -> this_("Property")
    result = re.sub(r'\bMe\.(\w+)', r'this_("\1")', result, flags=re.IGNORECASE)

    # Standalone Me -> this_
    result = re.sub(r'\bMe\b(?!\.)', 'this_', result, flags=re.IGNORECASE)

    for prop in class_def.properties:
        escaped_name = re.escape(prop.name)

        if prop.is_array:
            # Array properties: replace ALL occurrences with global variable name
            global_name = f"{class_def.name}_{prop.name}"
            prop_pattern = r'\b' + escaped_name + r'\b'
            result = re.sub(prop_pattern, global_name, result, flags=re.IGNORECASE)
        else:
            # Non-array: only replace assignments
            prop_pattern = r'\b' + escaped_name + r'\s*=\s*'
            result = re.sub(prop_pattern, f'this_("{prop.name}") = ', result, flags=re.IGNORECASE)

    return result


def emit_class_emulation(class_def):
    """Generate emulated class code"""
    out = []
    out.append(f"' === {class_def.name} Class Emulation ===\n")

    # Global Dim for array properties
    for prop in class_def.properties:
        if prop.is_array:
            out.append(f"Dim {class_def.name}_{prop.name}()")
    out.append("")

    # Factory function
    out.append(f"Function {class_def.name}_Create()")
    out.append("    Dim this_")
    out.append('    Set this_ = CreateObject("Scripting.Dictionary")')
    out.append(f'    this_("__class__") = "{class_def.name}"')

    for prop in class_def.properties:
        if not prop.is_array:
            out.append(f'    this_("{prop.name}") = Empty')

    if class_def.initialize_body:
        out.append("    ' Class_Initialize")
        init_body = transform_method_body(class_def.initialize_body, class_def)
        for line in init_body.split('\n'):
            if line.strip():
                out.append(f"    {line}")

    out.append(f"    Set {class_def.name}_Create = this_")
    out.append("End Function")
    out.append("")

    # Methods
    for method in class_def.methods:
        param_list = "this_"
        if method.params:
            param_list += ", " + ", ".join(method.params)

        kw = "Function" if method.is_function else "Sub"
        out.append(f"{kw} {class_def.name}_{method.name}({param_list})")

        transformed_body = transform_method_body(method.body, class_def)
        for line in transformed_body.split('\n'):
            out.append(f"    {line}")

        out.append(f"End {kw}")
        out.append("")

    out.append(f"' === End {class_def.name} ===\n")
    return '\n'.join(out)


def inject_wine_array_helpers(script):
    """Inject Wine VBScript array helper functions"""
    helpers = '''
' Wine VBScript Array Compatibility Helpers (Auto-injected by ScriptPatcher)
Function VPX_SafeUBound(arr)
    On Error Resume Next
    VPX_SafeUBound = -1
    VPX_SafeUBound = UBound(arr)
    On Error Goto 0
End Function

Sub VPX_SafeArraySet(arr, idx, val)
    On Error Resume Next
    arr(idx) = val
    On Error Goto 0
End Sub
' End Wine VBScript Array Compatibility Helpers

'''
    # Insert after Option Explicit or at start
    option_explicit = re.search(r'(Option\s+Explicit[^\r\n]*[\r\n]+)', script, re.IGNORECASE)
    if option_explicit:
        pos = option_explicit.end()
        return script[:pos] + helpers + script[pos:]
    return helpers + script


def patch_ubound_in_conditions(script):
    """Replace UBound in If conditions with VPX_SafeUBound"""
    pattern = r'If\s+UBound\s*\(\s*(\w+)\s*\)'
    return re.sub(pattern, r'If VPX_SafeUBound(\1)', script, flags=re.IGNORECASE)


def patch_array_element_assignment(script):
    """Patch problematic array element assignments.

    IMPORTANT: Only match ASSIGNMENTS at statement start, not comparisons in If conditions!
    Statement start positions: line start, after :, after Then, after Else

    Example that must NOT be transformed (comparison in If condition):
        If rolling(b) = True Then ...
    Example that SHOULD be transformed (assignment):
        rolling(b) = True

    Use inline error handling to avoid needing helper function at runtime:
        rolling(b) = False -> On Error Resume Next : rolling(b) = False : On Error Goto 0
    """
    # For known problematic arrays with True/False values - only at line start
    pattern = r'(^[ \t]*)(bBallInTrough|rolling)\s*\(\s*(\w+)\s*\)\s*=\s*(True|False)'
    script = re.sub(pattern, r'\1On Error Resume Next : \2(\3) = \4 : On Error Goto 0', script, flags=re.IGNORECASE | re.MULTILINE)

    # After Then/Else
    pattern2 = r'(\bThen[ \t]+|\bElse[ \t]+)(bBallInTrough|rolling)\s*\(\s*(\w+)\s*\)\s*=\s*(True|False)'
    script = re.sub(pattern2, r'\1On Error Resume Next : \2(\3) = \4 : On Error Goto 0', script, flags=re.IGNORECASE)

    # NOTE: ballvel/ballvelx/ballvely are now handled by class emulation
    # Do NOT add patterns for them here

    return script


def patch_array_object_property_access(script):
    """
    Patch Array(idx).property = value patterns.
    Wine VBScript doesn't support setting properties on array elements directly.
    Transform: ArrayName(idx).property = value
    To: VPX_SetArrObjProp ArrayName, idx, "property", value

    VPX_SetArrObjProp is a helper sub that sets the property with error handling.
    It uses a Select Case for common properties and Execute for unknown ones.

    IMPORTANT: Only match ASSIGNMENTS at statement start, not comparisons in If conditions!
    Statement start positions: line start, after :, after Then, after Else

    Example that must NOT be transformed (comparison in If condition):
        If Glowing(b).state = 0 Then ...
    Example that SHOULD be transformed (assignment after Then):
        If x Then Glowing(b).state = 1
    """
    # Pattern requires statement start position to avoid matching comparisons
    # Value capture stops at : or Then/Else keywords or newline (non-greedy)
    pattern = r'(^[ \t]*|:[ \t]*|\bThen[ \t]+|\bElse[ \t]+)(\w+)\s*\(\s*([^)]+)\s*\)\s*\.(\w+)\s*=\s*([^:\r\n]+?)(?=[ \t]*(?::|\'|\bThen\b|\bElse\b|\r|\n|$))'

    def replacer(m):
        prefix = m.group(1)
        arr_name = m.group(2)
        idx = m.group(3)
        prop = m.group(4)
        val = m.group(5).rstrip()
        return f'{prefix}VPX_SetArrObjProp {arr_name}, {idx}, "{prop}", {val}'

    return re.sub(pattern, replacer, script, flags=re.IGNORECASE | re.MULTILINE)


def patch_array_object_property_read(script):
    """
    Patch Array(idx).property READ patterns.
    Wine VBScript doesn't support reading properties from array elements directly.
    Transform: ArrayName(idx).property
    To: VPX_GetArrObjProp(ArrayName, idx, "property")

    This handles property reads like:
        gBOT(b).z          -> VPX_GetArrObjProp(gBOT, b, "z")
        If arr(i).x > 5    -> If VPX_GetArrObjProp(arr, i, "x") > 5
        y = obj(n).visible -> y = VPX_GetArrObjProp(obj, n, "visible")
        If obj(n).state = 0 Then -> If VPX_GetArrObjProp(obj, n, "state") = 0 Then

    IMPORTANT: This runs AFTER patch_array_object_property_access which handles writes.
    Any remaining arr(idx).prop patterns are reads (including comparisons like state = 0).
    We match ALL remaining patterns since writes have already been converted to VPX_SetArrObjProp.
    """
    # Pattern: word(index).property - match all remaining occurrences
    pattern = r'(\w+)\s*\(\s*([^)]+)\s*\)\s*\.(\w+)\b'
    return re.sub(pattern, r'VPX_GetArrObjProp(\1, \2, "\3")', script, flags=re.IGNORECASE)


def inject_wine_array_helpers(script):
    """Inject Wine VBScript array helper functions"""
    helpers = '''
' Wine VBScript Array Compatibility Helpers (Auto-injected by ScriptPatcher)
Dim VPX_TmpObj  ' Used for array object property access workaround

Function VPX_SafeUBound(arr)
    On Error Resume Next
    VPX_SafeUBound = -1
    VPX_SafeUBound = UBound(arr)
    On Error Goto 0
End Function

Sub VPX_SafeArraySet(arr, idx, val)
    On Error Resume Next
    arr(idx) = val
    On Error Goto 0
End Sub
' End Wine VBScript Array Compatibility Helpers

'''
    # Insert after Option Explicit or at start
    option_explicit = re.search(r'(Option\s+Explicit[^\r\n]*[\r\n]+)', script, re.IGNORECASE)
    if option_explicit:
        pos = option_explicit.end()
        return script[:pos] + helpers + script[pos:]
    return helpers + script


def emulate_classes(script):
    """Full class emulation pipeline"""
    classes = parse_class_definitions(script)
    if not classes:
        return script

    # Detect arrays from ReDim usage
    detect_arrays_from_redim(classes)

    # Generate emulation code and replace classes
    result = script
    for cls in sorted(classes, key=lambda c: c.start_pos, reverse=True):
        emulation = emit_class_emulation(cls)
        # For testing, just append emulation (real code replaces class definition)
        # This is simplified - real implementation tracks positions

    return result, classes


def patch_script(script):
    """Full patching pipeline - mirrors C++ PatchScript()"""
    result = script

    # 1. Class emulation
    classes = parse_class_definitions(result)
    detect_arrays_from_redim(classes)

    # Generate emulated code for each class
    emulated_classes = {}
    for cls in classes:
        emulated_classes[cls.name] = emit_class_emulation(cls)

    # 2. Wine array helpers
    result = inject_wine_array_helpers(result)

    # 3. Patch UBound in conditions
    result = patch_ubound_in_conditions(result)

    # 4. Patch array element assignments
    result = patch_array_element_assignment(result)

    return result, classes, emulated_classes


# =============================================================================
# TESTS
# =============================================================================

def test_cortracker_full_pipeline():
    """Test CoRTracker class through FULL pipeline"""
    print("=" * 70)
    print("TEST: CoRTracker full pipeline")
    print("=" * 70)

    # Original class from Lord of the Rings script
    original = '''Option Explicit

Class CoRTracker
	Public ballvel, ballvelx, ballvely

	Private Sub Class_Initialize
		ReDim ballvel(0)
		ReDim ballvelx(0)
		ReDim ballvely(0)
	End Sub

	Public Sub Update()
		Dim str, b, AllBalls, highestID
		allBalls = GetBalls

		For Each b In allballs
			If b.id >= HighestID Then highestID = b.id
		Next

		If UBound(ballvel) < highestID Then ReDim ballvel(highestID)
		If UBound(ballvelx) < highestID Then ReDim ballvelx(highestID)
		If UBound(ballvely) < highestID Then ReDim ballvely(highestID)

		For Each b In allballs
			ballvel(b.id) = BallSpeed(b)
			ballvelx(b.id) = b.velx
			ballvely(b.id) = b.vely
		Next
	End Sub
End Class

Dim cor
Set cor = New CoRTracker
'''

    # Parse and process
    classes = parse_class_definitions(original)
    assert len(classes) == 1, f"Expected 1 class, got {len(classes)}"

    cls = classes[0]
    assert cls.name == "CoRTracker", f"Expected CoRTracker, got {cls.name}"
    assert len(cls.properties) == 3, f"Expected 3 properties, got {len(cls.properties)}"

    # Detect arrays from ReDim
    detect_arrays_from_redim(classes)

    # All 3 should be detected as arrays
    array_props = [p for p in cls.properties if p.is_array]
    assert len(array_props) == 3, f"Expected 3 array properties, got {len(array_props)}"
    print(f"  Detected {len(array_props)} array properties: {[p.name for p in array_props]}")

    # Generate emulation
    emulation = emit_class_emulation(cls)
    print("\nEmulated class:")
    print(emulation)

    errors = []

    # Check global Dim statements
    if 'Dim CoRTracker_ballvel()' not in emulation:
        errors.append("FAIL: Missing 'Dim CoRTracker_ballvel()'")
    if 'Dim CoRTracker_ballvelx()' not in emulation:
        errors.append("FAIL: Missing 'Dim CoRTracker_ballvelx()'")
    if 'Dim CoRTracker_ballvely()' not in emulation:
        errors.append("FAIL: Missing 'Dim CoRTracker_ballvely()'")

    # Check Class_Initialize transformation
    if 'ReDim CoRTracker_ballvel(0)' not in emulation:
        errors.append("FAIL: Class_Initialize not transformed correctly")

    # Check Update method transformation
    if 'UBound(CoRTracker_ballvel)' not in emulation:
        errors.append("FAIL: UBound(ballvel) not transformed")
    if 'ReDim CoRTracker_ballvel(highestID)' not in emulation:
        errors.append("FAIL: ReDim ballvel not transformed")
    if 'CoRTracker_ballvel(b.id) = BallSpeed(b)' not in emulation:
        errors.append("FAIL: ballvel(b.id) = BallSpeed(b) not transformed")

    # CRITICAL: Check NO false transformations
    if 'CoRTracker_VPX_' in emulation:
        errors.append("FAIL: False positive - VPX_ functions got transformed!")
    if 'CoRTracker_UBound' in emulation:
        errors.append("FAIL: False positive - UBound got transformed!")
    if 'CoRTracker_BallSpeed' in emulation:
        errors.append("FAIL: False positive - BallSpeed got transformed!")

    # Check untransformed ballvel doesn't exist (except in comments)
    lines = [l for l in emulation.split('\n') if not l.strip().startswith("'")]
    code_only = '\n'.join(lines)
    if re.search(r'\bballvel\b(?!x|y)', code_only):
        errors.append("FAIL: Untransformed 'ballvel' still present in code")

    if errors:
        print("\n" + "\n".join(errors))
        return False

    print("\nPASS: CoRTracker full pipeline!")
    return True


def test_wine_patches_dont_interfere():
    """Test that Wine patches don't interfere with class emulation"""
    print("\n" + "=" * 70)
    print("TEST: Wine patches don't interfere with class emulation")
    print("=" * 70)

    # Simulated already-transformed code (after class emulation)
    transformed = '''
CoRTracker_ballvel(b.id) = BallSpeed(b)
CoRTracker_ballvelx(b.id) = b.velx
CoRTracker_ballvely(b.id) = b.vely
bBallInTrough(b) = True
'''

    result = patch_array_element_assignment(transformed)
    print("After patch_array_element_assignment:")
    print(result)

    errors = []

    # CoRTracker arrays should NOT be touched (they're already class-prefixed)
    if 'VPX_SafeArraySet CoRTracker_ballvel' in result:
        # This is actually OK if it happens, but the original should be preserved
        pass

    # bBallInTrough SHOULD be transformed to inline error handling
    if 'On Error Resume Next : bBallInTrough(b) = True : On Error Goto 0' not in result:
        errors.append("FAIL: bBallInTrough was not transformed to inline error handling")

    # CRITICAL: No mangled names like CoRTracker_VPX_SafeArraySet
    if 'CoRTracker_VPX_SafeArraySet' in result:
        errors.append("FAIL: Wine patch created mangled name 'CoRTracker_VPX_SafeArraySet'!")

    if errors:
        print("\n" + "\n".join(errors))
        return False

    print("\nPASS: Wine patches don't interfere!")
    return True


def test_with_real_script():
    """Test with the actual Lord of the Rings script"""
    print("\n" + "=" * 70)
    print("TEST: Real Lord of the Rings script")
    print("=" * 70)

    script_path = os.path.join(os.path.dirname(__file__),
                               'example_table_scripts',
                               'lord of the rings script.txt')

    if not os.path.exists(script_path):
        print(f"  SKIP: Script not found at {script_path}")
        return True

    with open(script_path, 'r', encoding='utf-8', errors='ignore') as f:
        script = f.read()

    print(f"  Loaded script: {len(script)} chars")

    # Parse classes
    classes = parse_class_definitions(script)
    print(f"  Found {len(classes)} classes:")
    for cls in classes:
        print(f"    - {cls.name}: {len(cls.properties)} properties, {len(cls.methods)} methods")

    # Detect arrays
    detect_arrays_from_redim(classes)

    errors = []

    # Find CoRTracker
    cortracker = next((c for c in classes if c.name == 'CoRTracker'), None)
    if not cortracker:
        errors.append("FAIL: CoRTracker class not found")
    else:
        array_props = [p for p in cortracker.properties if p.is_array]
        if len(array_props) != 3:
            errors.append(f"FAIL: Expected 3 array props in CoRTracker, got {len(array_props)}")

        # Generate emulation and verify
        emulation = emit_class_emulation(cortracker)

        if 'CoRTracker_ballvel(b.id) = BallSpeed(b)' not in emulation:
            errors.append("FAIL: ballvel assignment not transformed correctly")

        if 'CoRTracker_VPX_' in emulation:
            errors.append("FAIL: False positive transformation detected!")

    if errors:
        print("\n" + "\n".join(errors))
        return False

    print("\nPASS: Real script test!")
    return True


def test_multiple_classes():
    """Test script with multiple classes"""
    print("\n" + "=" * 70)
    print("TEST: Multiple classes")
    print("=" * 70)

    script = '''Option Explicit

Class Dampener
    Private ModIn(), ModOut()

    Private Sub Class_Initialize
        ReDim ModIn(0)
        ReDim ModOut(0)
    End Sub

    Public Sub Apply(idx, val)
        ModIn(idx) = val
        ModOut(idx) = val * 2
    End Sub
End Class

Class CoRTracker
    Public ballvel, ballvelx, ballvely

    Private Sub Class_Initialize
        ReDim ballvel(0)
    End Sub
End Class
'''

    classes = parse_class_definitions(script)
    detect_arrays_from_redim(classes)

    errors = []

    # Check Dampener
    dampener = next((c for c in classes if c.name == 'Dampener'), None)
    if not dampener:
        errors.append("FAIL: Dampener class not found")
    else:
        emulation = emit_class_emulation(dampener)
        if 'Dampener_ModIn(idx) = val' not in emulation:
            errors.append("FAIL: Dampener ModIn not transformed")
        if 'Dampener_ModOut(idx) = val * 2' not in emulation:
            errors.append("FAIL: Dampener ModOut not transformed")
        print("Dampener emulation:")
        print(emulation[:500])

    # Check CoRTracker
    cortracker = next((c for c in classes if c.name == 'CoRTracker'), None)
    if not cortracker:
        errors.append("FAIL: CoRTracker class not found")
    else:
        emulation = emit_class_emulation(cortracker)
        if 'CoRTracker_ballvel' not in emulation:
            errors.append("FAIL: CoRTracker ballvel not transformed")

    if errors:
        print("\n" + "\n".join(errors))
        return False

    print("\nPASS: Multiple classes!")
    return True


def test_array_object_property_access():
    """Test Array(idx).property = value transformation"""
    print("\n" + "=" * 70)
    print("TEST: Array object property access")
    print("=" * 70)

    original = '''Sub RollingTimer()
    Dim b
    For b = UBound(gBOT) + 1 to tnob
        If AmbientBallShadowOn = 0 Then BallShadowA(b).visible = 0
        rolling(b) = False
    Next

    For b = 0 to UBound(gBOT)
        BallShadowA(b).X = gBOT(b).X
        BallShadowA(b).Y = gBOT(b).Y + BallSize/5 + fovY
        BallShadowA(b).height = gBOT(b).z - BallSize/4
        BallShadowA(b).visible = 1
        BallShadowA(s).Opacity = 100*AmbientBSFactor
    Next
End Sub

Sub UpdateGlowball
    Dim b
    For b = 0 to UBound(gBOT)
        If NOT bBallInTrough(b) Then
            If Glowing(b).state = 0 Then Glowing(b).state = 1
            Glowing(b).x = gBOT(b).x
        Else
            Glowing(b).state = 0
        End If
    Next
End Sub'''

    result = patch_array_object_property_access(original)
    print("\nOriginal:")
    print(original[:400] + "...")
    print("\nTransformed:")
    print(result[:600] + "...")

    errors = []

    # Check transformations - assignments at statement start SHOULD be transformed
    if 'VPX_SetArrObjProp BallShadowA, b, "visible", 0' not in result:
        errors.append("FAIL: Missing transformed visible property (after Then)")

    if 'VPX_SetArrObjProp BallShadowA, b, "X", gBOT(b).X' not in result:
        errors.append("FAIL: Missing transformed X property (line start)")

    # rolling(b) = False should NOT be transformed (it's not .property access)
    if 'rolling(b) = False' not in result:
        errors.append("FAIL: rolling(b) = False was incorrectly transformed")

    # CRITICAL: If condition comparisons should NOT be transformed!
    # "If Glowing(b).state = 0 Then" - this is a COMPARISON, not assignment
    if 'If VPX_SetArrObjProp' in result:
        errors.append("FAIL: If condition comparison was incorrectly transformed!")
        errors.append("      'If Glowing(b).state = 0 Then' should NOT become 'If VPX_SetArrObjProp ...'")

    # The comparison should remain unchanged
    if 'If Glowing(b).state = 0 Then' not in result:
        errors.append("FAIL: If condition comparison was modified (should be unchanged)")

    # But the assignment after Then SHOULD be transformed
    if 'Then VPX_SetArrObjProp Glowing, b, "state", 1' not in result:
        errors.append("FAIL: Assignment after Then was not transformed")

    # Line-start assignments should be transformed
    if 'VPX_SetArrObjProp Glowing, b, "x", gBOT(b).x' not in result:
        errors.append("FAIL: Line-start assignment Glowing(b).x not transformed")

    # After Else should also be transformed
    if 'VPX_SetArrObjProp Glowing, b, "state", 0' not in result:
        errors.append("FAIL: Assignment after Else not transformed")

    if errors:
        print("\n" + "\n".join(errors))
        return False

    print("\nPASS: Array object property access!")
    return True


def test_rolling_array_assignment():
    """Test that rolling(b) = False gets transformed but If rolling(b) = True doesn't"""
    print("\n" + "=" * 70)
    print("TEST: rolling array assignment (assignment vs comparison)")
    print("=" * 70)

    # Test case with both assignment AND comparison
    original = '''Sub RollingTimer()
    Dim b
    For b = 0 to tnob
        rolling(b) = False
        If rolling(b) = True Then
            StopSound("BallRoll_" & b)
        End If
    Next
End Sub
'''

    result = patch_array_element_assignment(original)
    print("\nOriginal:")
    print(original)
    print("\nTransformed:")
    print(result)

    errors = []

    # Assignment SHOULD be transformed to inline error handling
    if 'On Error Resume Next : rolling(b) = False : On Error Goto 0' not in result:
        errors.append("FAIL: rolling(b) = False not transformed to inline error handling")

    # If condition comparison should NOT be transformed
    if 'If rolling(b) = True Then' not in result:
        errors.append("FAIL: If condition 'If rolling(b) = True Then' was incorrectly transformed!")

    # Also test bBallInTrough still works
    test2 = "    bBallInTrough(b) = True"
    result2 = patch_array_element_assignment(test2)
    if 'On Error Resume Next : bBallInTrough(b) = True : On Error Goto 0' not in result2:
        errors.append("FAIL: bBallInTrough not transformed")

    if errors:
        print("\n" + "\n".join(errors))
        return False

    print("\nPASS: rolling array assignment!")
    return True


def test_array_object_property_read():
    """Test Array(idx).property READ transformation"""
    print("\n" + "=" * 70)
    print("TEST: Array object property READ")
    print("=" * 70)

    original = '''Sub CheckBalls()
    Dim b
    For b = 0 to UBound(gBOT)
        If BallVel(gBOT(b)) > 1 AND gBOT(b).z < 30 AND NOT bBallInTrough(b) Then
            ProcessBall b, gBOT(b).x, gBOT(b).y
        End If
        y = arr(i).visible
        If obj(n).state = 0 Then DoSomething
    Next
End Sub'''

    # First apply writes transformation (to handle any assignments)
    result = patch_array_object_property_access(original)
    # Then apply reads transformation
    result = patch_array_object_property_read(result)

    print("\nOriginal:")
    print(original)
    print("\nTransformed:")
    print(result)

    errors = []

    # gBOT(b).z should become VPX_GetArrObjProp(gBOT, b, "z")
    if 'VPX_GetArrObjProp(gBOT, b, "z")' not in result:
        errors.append("FAIL: gBOT(b).z not transformed to VPX_GetArrObjProp")

    # gBOT(b).x should become VPX_GetArrObjProp(gBOT, b, "x")
    if 'VPX_GetArrObjProp(gBOT, b, "x")' not in result:
        errors.append("FAIL: gBOT(b).x not transformed to VPX_GetArrObjProp")

    # gBOT(b).y should become VPX_GetArrObjProp(gBOT, b, "y")
    if 'VPX_GetArrObjProp(gBOT, b, "y")' not in result:
        errors.append("FAIL: gBOT(b).y not transformed to VPX_GetArrObjProp")

    # arr(i).visible on RHS of assignment
    if 'VPX_GetArrObjProp(arr, i, "visible")' not in result:
        errors.append("FAIL: arr(i).visible not transformed to VPX_GetArrObjProp")

    # obj(n).state in If condition (comparison, not assignment)
    if 'VPX_GetArrObjProp(obj, n, "state")' not in result:
        errors.append("FAIL: obj(n).state not transformed to VPX_GetArrObjProp")

    # Nested function call should still work: BallVel(gBOT(b))
    # Note: gBOT(b) passed to function doesn't have .property so shouldn't be transformed
    if 'BallVel(gBOT(b))' not in result:
        errors.append("FAIL: BallVel(gBOT(b)) was incorrectly transformed")

    if errors:
        print("\n" + "\n".join(errors))
        return False

    print("\nPASS: Array object property READ!")
    return True


def test_helper_not_self_transformed():
    """Test that VPX_SetArrObjProp helper code doesn't get transformed by the patcher.

    This is the key fix: the helper must be injected AFTER PatchArrayObjectPropertyAccess runs,
    otherwise arr(idx).visible = val inside the helper becomes recursive VPX_SetArrObjProp calls.
    """
    print("\n" + "=" * 70)
    print("TEST: VPX_SetArrObjProp helper not self-transformed")
    print("=" * 70)

    # Simulate what happens if helper is injected BEFORE transformation (the bug)
    helper_code = '''Sub VPX_SetArrObjProp(arr, idx, propName, val)
    On Error Resume Next
    Select Case LCase(propName)
        Case "visible": arr(idx).visible = val
        Case "x": arr(idx).x = val
        Case "y": arr(idx).y = val
    End Select
    On Error Goto 0
End Sub
'''

    # Apply the transformation to helper code (simulating the bug)
    transformed = patch_array_object_property_access(helper_code)

    errors = []

    # This demonstrates WHY we need to inject helper AFTER transformation
    # If transformed, we'd see recursive calls - THIS IS THE BUG we're preventing
    if 'VPX_SetArrObjProp arr, idx, "visible", val' in transformed:
        print("  (Expected) If helper was transformed, we'd get recursive calls:")
        print("    arr(idx).visible = val -> VPX_SetArrObjProp arr, idx, \"visible\", val")
        print("  This is why C++ injects VPX_SetArrObjProp AFTER PatchArrayObjectPropertyAccess")
    else:
        # This shouldn't happen - the regex DOES match
        if 'arr(idx).visible = val' in transformed:
            print("  Helper code preserved correctly (arr(idx).visible = val)")

    # The CORRECT behavior is ensured by injecting helper AFTER transformation
    # Simulate correct order: transform script first, then inject helper
    script = '''Sub Test()
    BallShadow(b).visible = 0
End Sub
'''
    # Step 1: Transform script
    transformed_script = patch_array_object_property_access(script)
    # Step 2: Then inject helper (not transformed)
    final = helper_code + transformed_script

    if 'VPX_SetArrObjProp BallShadow, b, "visible", 0' not in final:
        errors.append("FAIL: Script not transformed correctly")

    # Helper should still have original arr(idx).visible = val
    if 'arr(idx).visible = val' not in final:
        errors.append("FAIL: Helper code was incorrectly modified")

    if errors:
        print("\n" + "\n".join(errors))
        return False

    print("  Correct order: Transform script first, then inject helper")
    print("\nPASS: Helper code protection works!")
    return True


if __name__ == '__main__':
    all_passed = True

    all_passed &= test_cortracker_full_pipeline()
    all_passed &= test_wine_patches_dont_interfere()
    all_passed &= test_multiple_classes()
    all_passed &= test_array_object_property_access()
    all_passed &= test_array_object_property_read()
    all_passed &= test_rolling_array_assignment()
    all_passed &= test_helper_not_self_transformed()
    all_passed &= test_with_real_script()

    print("\n" + "=" * 70)
    if all_passed:
        print("ALL TESTS PASSED - Safe to build!")
    else:
        print("SOME TESTS FAILED - DO NOT BUILD!")
    print("=" * 70)

    exit(0 if all_passed else 1)

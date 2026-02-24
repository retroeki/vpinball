#!/usr/bin/env python3
"""
Test script for VPinball ScriptPatcher external VBS file handling.

This script tests various transformations that the ScriptPatcher applies
to external VBS scripts (like de.vbs, core.vbs) when they contain VBScript
class definitions that need to be emulated for Wine VBScript compatibility.

Run from vpinball-master directory:
    python tests/scriptpatcher/test_external_vbs.py
"""

import os
import re
import sys
from pathlib import Path

# Test case definitions
# Each test has: name, input_vbs, expected_patterns (regexes that should match),
# unexpected_patterns (regexes that should NOT match)

TEST_CASES = [
    {
        "name": "Function return value should NOT be transformed as method call",
        "input": '''
Class cvpmDictionary
    Private mDict

    Private Sub Class_Initialize
        Set mDict = CreateObject("Scripting.Dictionary")
    End Sub

    Public Function Items()
        Items = mDict.Items
    End Function

    Public Function Keys()
        Keys = mDict.Keys
    End Function
End Class
''',
        "expected": [
            r"cvpmDictionary_Items\s*=\s*this_\(\"mDict\"\)\.Items",  # Return assignment preserved
            r"cvpmDictionary_Keys\s*=\s*this_\(\"mDict\"\)\.Keys",    # Return assignment preserved
        ],
        "unexpected": [
            r"cvpmDictionary_Items\s+this_,\s*=",  # Should NOT have "this_," before =
            r"cvpmDictionary_Keys\s+this_,\s*=",   # Should NOT have "this_," before =
        ]
    },
    {
        "name": "Native Dictionary properties should NOT be transformed",
        "input": '''
Class cvpmDictionary
    Private mDict

    Private Sub Class_Initialize
        Set mDict = CreateObject("Scripting.Dictionary")
    End Sub

    Public Function Count()
        Count = mDict.Count
    End Function

    Public Function Exists(aKey)
        Exists = mDict.Exists(aKey)
    End Function
End Class
''',
        "expected": [
            r"this_\(\"mDict\"\)\.Count",   # Native .Count preserved
            r"this_\(\"mDict\"\)\.Exists",  # Native .Exists preserved
        ],
        "unexpected": [
            r"cvpmDictionary_Get_Count\(this_\(\"mDict\"\)\)",  # Should NOT transform native Dict property
        ]
    },
    {
        "name": "External class With blocks should be transformed",
        "input": '''
' External script that uses cvpmDropTarget from core.vbs
Dim DTBank
Set DTBank = New cvpmDropTarget
With DTBank
    .InitDrop Array(sw41, sw42), Array(41, 42)
    .InitSnd "dropsound", "raisesound"
End With
''',
        "expected": [
            r"Set DTBank = cvpmDropTarget_Create\(\)",
            r"cvpmDropTarget_InitDrop\s+DTBank,\s*Array\(sw41, sw42\),\s*Array\(41, 42\)",
            r"cvpmDropTarget_InitSnd\s+DTBank,\s*\"dropsound\",\s*\"raisesound\"",
        ],
        "unexpected": [
            r"With DTBank",  # With block should be removed
            r"\.InitDrop",   # Dot notation should be transformed
        ]
    },
    {
        "name": "External class method calls without With should be transformed",
        "input": '''
' External script with method calls
Dim DTBank
Set DTBank = New cvpmDropTarget
DTBank.Hit 1
DTBank.SolHit 2, True
''',
        "expected": [
            r"cvpmDropTarget_Hit\s+DTBank,\s*1",
            r"cvpmDropTarget_SolHit\s+DTBank,\s*2,\s*True",
        ],
        "unexpected": [
            r"DTBank\.Hit",      # Dot notation should be transformed
            r"DTBank\.SolHit",   # Dot notation should be transformed
        ]
    },
    {
        "name": "Internal class method calls should be transformed",
        "input": '''
Class cvpmDropTarget
    Private mSw

    Public Sub Hit(aNo)
        CheckAllDn True
    End Sub

    Private Sub CheckAllDn(aStatus)
        ' Do something
    End Sub
End Class
''',
        "expected": [
            r"cvpmDropTarget_CheckAllDn\s+this_,\s*True",  # Internal call transformed
        ],
        "unexpected": [
            r"\bCheckAllDn\s+True\b(?!.*this_)",  # Should not have bare CheckAllDn True
        ]
    },
    {
        "name": "Property Get without params should be transformed inside class",
        "input": '''
Class FlipperPolarity
    Private mStartPoint
    Private mEndPoint

    Public Property Get StartPoint
        StartPoint = mStartPoint
    End Property

    Public Property Get EndPoint
        EndPoint = mEndPoint
    End Property

    Public Sub Calculate()
        Dim diff
        diff = EndPoint - StartPoint
    End Sub
End Class
''',
        "expected": [
            r"FlipperPolarity_Get_StartPoint\(this_\)",
            r"FlipperPolarity_Get_EndPoint\(this_\)",
        ],
        "unexpected": []
    },
    {
        "name": "Array property access on external objects should be transformed",
        "input": '''
' Table script using external class arrays
Dim i, LightArray(10)
For i = 0 To UBound(LightArray)
    LightArray(i).State = 1
    LightArray(i).intensityscale = 0.5
Next
''',
        "expected": [
            # Array element property access patterns
            r"LightArray\(i\)\.State",  # VPX native properties preserved
        ],
        "unexpected": []
    },
]


def simulate_transform(input_vbs):
    """
    Simulate key scriptpatcher transformations for testing.
    This is a simplified Python implementation to verify expected patterns.
    The actual C++ implementation is in scriptpatcher.cpp.
    """
    result = input_vbs

    # 1. Transform Class definitions to functions
    class_pattern = r'Class\s+(\w+)(.*?)End\s+Class'
    classes = re.findall(class_pattern, result, re.DOTALL | re.IGNORECASE)

    for class_name, class_body in classes:
        # This is a very simplified transform - actual C++ does much more
        # We're just checking patterns here, not doing full transformation
        pass

    return result


def run_tests():
    """Run all test cases and report results."""
    passed = 0
    failed = 0

    print("=" * 60)
    print("ScriptPatcher External VBS Tests")
    print("=" * 60)
    print()

    for test in TEST_CASES:
        print(f"Test: {test['name']}")
        print("-" * 40)

        # For now, we just document expected patterns
        # Actual testing would require calling the C++ patcher

        print(f"  Input length: {len(test['input'])} chars")
        print(f"  Expected patterns: {len(test['expected'])}")
        print(f"  Unexpected patterns: {len(test['unexpected'])}")

        # Document expected transformations
        print("  Expected transformations:")
        for pattern in test['expected']:
            print(f"    + {pattern[:60]}...")

        if test['unexpected']:
            print("  Should NOT contain:")
            for pattern in test['unexpected']:
                print(f"    - {pattern[:60]}...")

        print()
        passed += 1

    print("=" * 60)
    print(f"Tests documented: {passed}")
    print("=" * 60)
    print()
    print("NOTE: These tests document expected behavior.")
    print("Actual validation requires running the C++ ScriptPatcher")
    print("and comparing output against expected patterns.")

    return passed, failed


def create_test_vbs_files():
    """Create test VBS files for manual testing."""
    test_dir = Path(__file__).parent / "vbs_samples"
    test_dir.mkdir(exist_ok=True)

    for i, test in enumerate(TEST_CASES):
        filename = test_dir / f"test_{i+1}_{test['name'][:30].replace(' ', '_')}.vbs"
        with open(filename, 'w') as f:
            f.write(f"' Test: {test['name']}\n")
            f.write(f"' Expected patterns after transformation:\n")
            for pattern in test['expected']:
                f.write(f"'   + {pattern}\n")
            if test['unexpected']:
                f.write(f"' Should NOT contain:\n")
                for pattern in test['unexpected']:
                    f.write(f"'   - {pattern}\n")
            f.write("'\n")
            f.write(test['input'])
        print(f"Created: {filename}")


def validate_patched_output(patched_file, test_case):
    """
    Validate a patched VBS file against expected patterns.

    Args:
        patched_file: Path to patched script output
        test_case: Test case dictionary with expected/unexpected patterns

    Returns:
        Tuple of (success, errors)
    """
    with open(patched_file, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()

    errors = []

    # Check expected patterns
    for pattern in test_case.get('expected', []):
        if not re.search(pattern, content):
            errors.append(f"Expected pattern not found: {pattern}")

    # Check unexpected patterns
    for pattern in test_case.get('unexpected', []):
        if re.search(pattern, content):
            errors.append(f"Unexpected pattern found: {pattern}")

    return len(errors) == 0, errors


if __name__ == "__main__":
    if len(sys.argv) > 1:
        if sys.argv[1] == "--create-samples":
            create_test_vbs_files()
        elif sys.argv[1] == "--validate" and len(sys.argv) > 2:
            # Validate a patched file against all test cases
            patched_file = sys.argv[2]
            print(f"Validating: {patched_file}")

            # Read patched content
            with open(patched_file, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()

            total_passed = 0
            total_failed = 0

            for test in TEST_CASES:
                success, errors = validate_patched_output(patched_file, test)
                if success:
                    print(f"  PASS: {test['name']}")
                    total_passed += 1
                else:
                    print(f"  FAIL: {test['name']}")
                    for err in errors:
                        print(f"    - {err}")
                    total_failed += 1

            print()
            print(f"Results: {total_passed} passed, {total_failed} failed")
        else:
            print("Usage:")
            print("  python test_external_vbs.py              # Run tests")
            print("  python test_external_vbs.py --create-samples  # Create sample VBS files")
            print("  python test_external_vbs.py --validate <patched.vbs>  # Validate output")
    else:
        run_tests()

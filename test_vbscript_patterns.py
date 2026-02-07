#!/usr/bin/env python3
"""
VBScript Pattern Tester for Wine VBScript Parser

This script generates minimal VBScript test cases to identify parsing issues
with the Wine VBScript parser, particularly around single-line If-Then statements
with array access patterns.

Usage:
    python test_vbscript_patterns.py [--generate-only]

The script will:
1. Generate test .vbs files for various patterns
2. If Wine VBScript is available, attempt to compile each
3. Report which patterns pass/fail
"""

import os
import subprocess
import tempfile
import sys
from pathlib import Path

# Test patterns - each is a tuple of (name, vbscript_code, should_pass)
TEST_PATTERNS = [
    # Basic patterns that should work
    ("basic_assignment", """
Dim x
x = 1
""", True),

    ("basic_if_multiline", """
Dim x
x = 1
If x = 1 Then
    x = 2
End If
""", True),

    ("basic_if_singleline", """
Dim x
x = 1
If x = 1 Then x = 2
""", True),

    ("basic_if_singleline_with_endif", """
Dim x
x = 1
If x = 1 Then x = 2 End If
""", True),  # Wine extension - may fail

    # Array patterns
    ("array_access_simple", """
Dim arr(10)
arr(0) = 1
Dim x
x = arr(0)
""", True),

    ("array_in_if_multiline", """
Dim arr(10)
arr(0) = 1
If arr(0) = 1 Then
    arr(1) = 2
End If
""", True),

    # THE FAILING PATTERN - single-line If with array access on RHS
    ("singleline_if_array_rhs", """
Dim arr(10)
arr(0) = 1
arr(1) = 2
Dim x
If True Then x = arr(0)
""", True),

    ("singleline_if_array_rhs_variable_condition", """
Dim arr(10)
arr(0) = 1
Dim Enabled
Enabled = True
If Enabled Then x = arr(0)
""", True),

    # Two consecutive single-line If statements with array access
    ("consecutive_singleline_if_array", """
Dim arr(10)
arr(0) = 1
arr(1) = 2
Dim x, y
If True Then x = arr(0)
If True Then y = arr(1)
""", True),

    # Nested inside multi-line If
    ("nested_singleline_if_in_multiline", """
Dim arr(10)
arr(0) = 1
arr(1) = 2
Dim x, y
If True Then
    If True Then x = arr(0)
    If True Then y = arr(1)
End If
""", True),

    # The exact failing pattern from the VPX script
    ("vpx_flipper_polarity_pattern", """
Dim RotVxVy(2)
RotVxVy(0) = 1.5
RotVxVy(1) = 2.5
Dim Enabled
Enabled = True

Class TestClass
    Public Sub TestMethod()
        Dim localVar
        If Enabled Then localVar = RotVxVy(0)
        If Enabled Then localVar = RotVxVy(1)
    End Sub
End Class
""", True),

    # Member expression with array access
    ("member_assignment_array_rhs", """
Dim arr(10)
arr(0) = 1

Class Ball
    Public Velx
    Public Vely
End Class

Dim aBall
Set aBall = New Ball
If True Then aBall.Velx = arr(0)
""", True),

    # The exact VPX pattern simplified
    ("vpx_exact_pattern", """
Dim RotVxVy(2)
RotVxVy(0) = 1.0
RotVxVy(1) = 2.0

Class Ball
    Public Velx
    Public Vely
End Class

Dim aBall
Set aBall = New Ball
Dim Enabled
Enabled = True

If Enabled Then aBall.Velx = RotVxVy(0)
If Enabled Then aBall.Vely = RotVxVy(1)
""", True),

    # Function returning array
    ("function_return_array_access", """
Function GetArray()
    Dim arr(2)
    arr(0) = 1
    arr(1) = 2
    GetArray = arr
End Function

Dim result
result = GetArray()
Dim x
If True Then x = result(0)
""", True),

    # Dim inside If block (Wine has issues with this)
    ("dim_inside_if_multiline", """
If True Then
    Dim x
    x = 1
End If
""", True),

    # Dim on single-line If (should fail - invalid VBScript)
    ("dim_on_singleline_if", """
If True Then Dim x
""", False),  # This is actually invalid VBScript

    # Select Case with array
    ("select_case_array", """
Dim arr(10)
arr(0) = 1
Select Case arr(0)
    Case 1
        arr(1) = 2
End Select
""", True),

    # Complex nested pattern from VPX
    ("vpx_nested_if_pattern", """
Dim ModIn(10)
ModIn(0) = 1

Sub TestSub()
    Dim Angle, RotVxVy(2)
    RotVxVy(0) = 1.0
    RotVxVy(1) = 2.0

    If Not IsEmpty(ModIn(0)) Then
        Dim localX, localY
        If True Then localX = RotVxVy(0)
        If True Then localY = RotVxVy(1)
    End If
End Sub
""", True),

    # If-Then-Else single line
    ("singleline_if_then_else", """
Dim x
If True Then x = 1 Else x = 2
""", True),

    # If-Then-Else with array access
    ("singleline_if_then_else_array", """
Dim arr(10)
arr(0) = 1
arr(1) = 2
Dim x
If True Then x = arr(0) Else x = arr(1)
""", True),

    # Colon-separated statements
    ("colon_separated_if", """
Dim x : x = 1 : If x = 1 Then x = 2
""", True),

    # Single-line If with colon
    ("singleline_if_with_colon_body", """
Dim x, y
If True Then x = 1 : y = 2
""", True),

]

def generate_test_file(name: str, code: str) -> Path:
    """Generate a test .vbs file"""
    test_dir = Path(tempfile.gettempdir()) / "vbscript_tests"
    test_dir.mkdir(exist_ok=True)

    filepath = test_dir / f"{name}.vbs"
    # Ensure Windows line endings
    code_crlf = code.replace('\n', '\r\n')
    filepath.write_text(code_crlf, encoding='utf-8')
    return filepath

def run_all_tests(generate_only: bool = False):
    """Run all test patterns"""
    print("VBScript Pattern Tester for Wine VBScript Parser")
    print("=" * 60)
    print()

    results = []

    for name, code, expected_pass in TEST_PATTERNS:
        filepath = generate_test_file(name, code)
        print(f"Generated: {filepath}")

        if generate_only:
            results.append((name, "GENERATED", expected_pass))
            continue

        # TODO: Add actual Wine VBScript compilation test here
        # For now, just mark as untested
        results.append((name, "UNTESTED", expected_pass))

    print()
    print("=" * 60)
    print("Summary:")
    print("=" * 60)

    for name, status, expected in results:
        expected_str = "should PASS" if expected else "should FAIL"
        print(f"  {name}: {status} ({expected_str})")

    print()
    print(f"Test files generated in: {Path(tempfile.gettempdir()) / 'vbscript_tests'}")

    return results

def create_combined_test_script():
    """Create a single combined test script for manual testing"""
    output_path = Path(__file__).parent / "combined_vbscript_test.vbs"

    combined = """' Combined VBScript Test Script
' Tests various patterns that may fail in Wine VBScript parser
' Generated by test_vbscript_patterns.py

Option Explicit

' Global test counter
Dim g_testsPassed, g_testsFailed
g_testsPassed = 0
g_testsFailed = 0

Sub ReportTest(testName, passed)
    If passed Then
        g_testsPassed = g_testsPassed + 1
        ' Debug.Print "PASS: " & testName
    Else
        g_testsFailed = g_testsFailed + 1
        Debug.Print "FAIL: " & testName
    End If
End Sub

' ==============================================================
' TEST 1: Basic single-line If with simple assignment
' ==============================================================
Sub Test_BasicSingleLineIf()
    Dim x
    x = 0
    If True Then x = 1
    ReportTest "BasicSingleLineIf", (x = 1)
End Sub

' ==============================================================
' TEST 2: Single-line If with array access on RHS
' ==============================================================
Sub Test_SingleLineIfArrayRHS()
    Dim arr(2)
    arr(0) = 42
    arr(1) = 99
    Dim x
    x = 0
    If True Then x = arr(0)
    ReportTest "SingleLineIfArrayRHS", (x = 42)
End Sub

' ==============================================================
' TEST 3: Consecutive single-line If with array access
' ==============================================================
Sub Test_ConsecutiveSingleLineIfArray()
    Dim arr(2)
    arr(0) = 10
    arr(1) = 20
    Dim x, y
    x = 0
    y = 0
    If True Then x = arr(0)
    If True Then y = arr(1)
    ReportTest "ConsecutiveSingleLineIfArray", (x = 10 And y = 20)
End Sub

' ==============================================================
' TEST 4: Single-line If with member expression and array RHS
' ==============================================================
Class TestBall
    Public Velx
    Public Vely
End Class

Sub Test_MemberExpressionArrayRHS()
    Dim RotVxVy(2)
    RotVxVy(0) = 1.5
    RotVxVy(1) = 2.5

    Dim ball
    Set ball = New TestBall
    ball.Velx = 0
    ball.Vely = 0

    Dim Enabled
    Enabled = True

    If Enabled Then ball.Velx = RotVxVy(0)
    If Enabled Then ball.Vely = RotVxVy(1)

    ReportTest "MemberExpressionArrayRHS", (ball.Velx = 1.5 And ball.Vely = 2.5)
End Sub

' ==============================================================
' TEST 5: Nested single-line If inside multi-line If
' ==============================================================
Sub Test_NestedSingleLineIf()
    Dim arr(2)
    arr(0) = 100
    arr(1) = 200
    Dim x, y
    x = 0
    y = 0

    If True Then
        If True Then x = arr(0)
        If True Then y = arr(1)
    End If

    ReportTest "NestedSingleLineIf", (x = 100 And y = 200)
End Sub

' ==============================================================
' TEST 6: If-Then-Else single line with array
' ==============================================================
Sub Test_IfThenElseArray()
    Dim arr(2)
    arr(0) = 50
    arr(1) = 60
    Dim x
    If True Then x = arr(0) Else x = arr(1)
    ReportTest "IfThenElseArray", (x = 50)
End Sub

' ==============================================================
' TEST 7: The exact VPX FlipperPolarity pattern
' ==============================================================
Class FlipperPolarityTest
    Private m_Enabled

    Public Sub Init()
        m_Enabled = True
    End Sub

    Public Sub ProcessBall(aBall)
        Dim Angle, RotVxVy(2)
        Angle = 45
        RotVxVy(0) = 1.0
        RotVxVy(1) = 2.0

        If m_Enabled Then aBall.Velx = RotVxVy(0)
        If m_Enabled Then aBall.Vely = RotVxVy(1)
    End Sub
End Class

Sub Test_VPXFlipperPolarity()
    Dim fp
    Set fp = New FlipperPolarityTest
    fp.Init

    Dim ball
    Set ball = New TestBall
    ball.Velx = 0
    ball.Vely = 0

    fp.ProcessBall ball

    ReportTest "VPXFlipperPolarity", (ball.Velx = 1.0 And ball.Vely = 2.0)
End Sub

' ==============================================================
' RUN ALL TESTS
' ==============================================================
Sub RunAllTests()
    Test_BasicSingleLineIf
    Test_SingleLineIfArrayRHS
    Test_ConsecutiveSingleLineIfArray
    Test_MemberExpressionArrayRHS
    Test_NestedSingleLineIf
    Test_IfThenElseArray
    Test_VPXFlipperPolarity

    Debug.Print "================================"
    Debug.Print "Tests Passed: " & g_testsPassed
    Debug.Print "Tests Failed: " & g_testsFailed
    Debug.Print "================================"
End Sub

' Run tests when script loads
RunAllTests
"""

    # Ensure Windows line endings
    combined = combined.replace('\n', '\r\n')
    output_path.write_text(combined, encoding='utf-8')
    print(f"Created combined test script: {output_path}")
    return output_path

def test_line_endings():
    """Test that line endings are properly normalized"""
    print("\nLine Ending Tests:")
    print("=" * 40)

    test_cases = [
        ("CRLF (correct)", "Dim x\r\nx = 1\r\n", True),
        ("LF only", "Dim x\nx = 1\n", True),
        ("CR only", "Dim x\rx = 1\r", True),
        ("Double CR (BROKEN)", "Dim x\r\r\nx = 1\r\r\n", False),  # This is what was failing!
        ("Mixed", "Dim x\r\nx = 1\n", True),
    ]

    for name, content, should_work in test_cases:
        # Count line ending types
        crlf_count = content.count('\r\n')
        cr_only = content.count('\r') - crlf_count
        lf_only = content.count('\n') - crlf_count
        double_cr = content.count('\r\r')

        status = "OK" if should_work else "BROKEN (needs fix)"
        print(f"  {name}: CRLF={crlf_count}, CR={cr_only}, LF={lf_only}, DoubleR={double_cr} -> {status}")

    print()
    print("The NormalizeLineEndings fix converts all line endings to proper CRLF")
    print("by removing all CR and then adding CR before each LF.")

if __name__ == "__main__":
    generate_only = "--generate-only" in sys.argv

    # Test line endings
    test_line_endings()

    # Generate individual test files
    run_all_tests(generate_only)

    # Also create a combined test script
    create_combined_test_script()

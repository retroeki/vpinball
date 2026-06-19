#!/usr/bin/env python3
"""
Regression test for the single-line `If ... Then Controller.Pause = X` patch.

Bug (4 Queens (Bally 1970) 1.2, 2026-06-16): SimpleScriptPatcher's p2 rule
commented out `Controller.Pause = 1` even when it was the consequent of a
single-line If:

    If B2SOn Then Controller.Pause = 1
        -> If B2SOn Then ' Controller.Pause = 1 ' Disabled for Android

That leaves a bare `If ... Then` with no body, which VBScript parses as a
block-If wanting an `End If`. The whole script fails to compile and the table
crashes on load.

Fix (simplescriptpatcher.cpp PatchControllerPause, rule `pPauseIf`, runs BEFORE
p2): rewrite ONLY the consequent to `Exit Sub`, mirroring the existing
Controller.Stop handler `p3b`:

    If B2SOn Then Exit Sub ' Controller.Pause disabled for Android

Two regressions this test guards against:
  1. The original crash (single-line If body lost).
  2. A multi-line block `If x Then \n Controller.Pause=y \n End If` must NOT be
     matched/mangled. The rule uses `[ \t]+` (not `\s+`) after `Then` so the
     match can never span a newline. (A greedy `\s+` here wrongly matched and
     collapsed multi-line blocks in BTILC / CyberRace / Devil's Dare / Die Hard
     / Futurama / Terrifier during fix development.)

Run from vpinball-master:
    python tests/scriptpatcher/test_controller_pause_if.py

Uses genuine RE2 (the on-device engine) via the `google-re2` package when
installed; otherwise falls back to Python `re` (semantically identical for
these patterns, which use no lookahead/backreferences).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_invariants as ci

# Prefer genuine RE2 (matches the device's RE2::GlobalReplace). google-re2's
# .sub() needs \g<N> backrefs; Python re accepts both, so we use \g<N>.
try:
    import re2 as _rx
    ENGINE = "google-re2 (genuine RE2)"
except Exception:
    import re as _rx
    ENGINE = "python re (fallback)"

# --- rules under test, copied verbatim from simplescriptpatcher.cpp ---
# Keep these in sync with PatchControllerPause if the C++ changes.
pSubOnlyPause = _rx.compile(
    r"(?i)(Sub\s+\w+(?:\s*\([^)]*\))?)\s*:\s*Controller\.Pause\s*=\s*(?:True|False|1|0)\s*:\s*(End\s+Sub)")
p1 = _rx.compile(r"(?i):[ \t]*Controller\.Pause\s*=\s*(True|False|1|0)[ \t]*:")
# THE FIX (pPauseIf):
pPauseIf = _rx.compile(r"(?i)(If\b[^\r\n]*?\bThen[ \t]+)Controller\.Pause[ \t]*=[ \t]*(?:True|False|1|0)")
p2 = _rx.compile(r"(?i)(\s*)(Controller\.Pause\s*=\s*(True|False|1|0))")


def patch(script, with_fix):
    s = pSubOnlyPause.sub(r"\g<1>" + "\r\n\t' Controller.Pause disabled for Android\r\n" + r"\g<2>", script)
    s = p1.sub(":", s)
    if with_fix:
        s = pPauseIf.sub(r"\g<1>Exit Sub ' Controller.Pause disabled for Android", s)
    s = p2.sub(r"\g<1>' \g<2> ' Disabled for Android", s)
    return s


def if_violations(text):
    return [v for rule in ci.ALL_RULES for v in rule(text)
            if v[1] == "if_then_else_structure"]


g_fail = 0


def check(cond, msg):
    global g_fail
    if cond:
        print(f"PASS  {msg}")
    else:
        print(f"FAIL  {msg}")
        g_fail += 1


# 1. The canonical 4 Queens crash: WITHOUT the fix it produces a dangling If;
#    WITH the fix it compiles clean.
FOUR_QUEENS = ("Sub Table1_Paused\r\n"
               "\tIf B2SOn Then Controller.Pause = 1\r\n"
               "End Sub\r\n"
               "Sub Table1_unPaused\r\n"
               "\tIf B2SOn Then Controller.Pause = 0\r\n"
               "End Sub\r\n")
check(len(if_violations(patch(FOUR_QUEENS, with_fix=False))) == 2,
      "4 Queens single-line If-Pause: WITHOUT fix produces dangling block-If (reproduces crash)")
check(len(if_violations(patch(FOUR_QUEENS, with_fix=True))) == 0,
      "4 Queens single-line If-Pause: WITH fix compiles clean (no dangling If)")
check("If B2SOn Then Exit Sub ' Controller.Pause disabled for Android" in patch(FOUR_QUEENS, with_fix=True),
      "4 Queens: consequent rewritten to Exit Sub, If prefix preserved")

# 2. Multi-line block-If must be left untouched by the fix (and stay valid):
#    BTILC / CyberRace / Devil's Dare / Die Hard / Futurama / Terrifier class.
MULTILINE_BLOCK = ("If B2SOn Then\r\n"
                   "\tController.Pause = False\r\n"
                   "\tPlaySound \"x\"\r\n"
                   "End If\r\n")
fixed_block = patch(MULTILINE_BLOCK, with_fix=True)
nofix_block = patch(MULTILINE_BLOCK, with_fix=False)
check(fixed_block == nofix_block,
      "multi-line block If: fix produces byte-identical output to no-fix (no false match)")
check("If B2SOn Then\r\n" in fixed_block and "Exit Sub" not in fixed_block,
      "multi-line block If: header preserved, NOT collapsed to single-line Exit Sub")
check(len(if_violations(fixed_block)) == 0,
      "multi-line block If: still structurally valid after patching")

# 3. `If x = True Then Controller.Pause = 0` (comparison in condition) is handled.
COMPARISON = "\tIf B2SOn = True Then Controller.Pause = 0\r\n"
# Wrap in a sub so a lone dangling If is detectable.
COMPARISON_SUB = "Sub Foo\r\n" + COMPARISON + "End Sub\r\n"
check(len(if_violations(patch(COMPARISON_SUB, with_fix=True))) == 0,
      "single-line If with comparison condition: fixed")

# 4. Own-line Controller.Pause (not inside an If) is still handled by p2, not pPauseIf.
OWN_LINE = "Controller.Pause = False\r\n"
out = patch(OWN_LINE, with_fix=True)
check("' Controller.Pause = False ' Disabled for Android" in out and "Exit Sub" not in out,
      "own-line Controller.Pause: commented by p2, pPauseIf does not touch it")

print()
print(f"engine: {ENGINE}")
print(f"{'ALL PASS' if g_fail == 0 else 'FAILURES'} ({g_fail} failure(s))")
sys.exit(1 if g_fail else 0)

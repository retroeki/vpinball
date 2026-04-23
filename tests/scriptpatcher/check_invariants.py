#!/usr/bin/env python3
"""
SimpleScriptPatcher invariant checker (Layer 1 of TESTING_PLAN.md).

Scans patched .vbs output for bug classes that have historically broken real
tables. Each rule catches a whole FAMILY of patcher errors rather than one
specific bug. Exits 0 if all inputs pass, non-zero if any violations are
found.

Usage
-----
    python check_invariants.py <dir-or-file> [<dir-or-file> ...]
    python check_invariants.py <path> --summary           # table summary only
    python check_invariants.py <path> --max-per-file 5    # cap per-file output

Rules
-----
1. no_fused_injected_identifier
       Patcher-injected identifiers (vpx_ssc_tmp, ssp_newobj, ssp_idx, ssp_ref)
       must never be immediately followed by a word char. This catches
       accidental token fusion (e.g. "vpx_ssc_tmpMOD 10", the Castlevania
       bug shipped and reverted on 2026-04-22).

2. balanced_select_case_rewrite
       In patched lines of the form
           vpx_ssc_tmp = EXPR : Select Case vpx_ssc_tmp ...
       the EXPR between `=` and `:` must have balanced parens. This catches
       the entire Group A family (Diner, Black Knight, KISS, American Dad),
       where the patcher's `[^)]+` regex stopped at the first `)` in nested
       calls like `LightType(chgLamp(ii, 0))`, stranding the outer `)`.

3. no_dangling_else
       Lines that end with `: Else` (with no matching body after the Else)
       are malformed. Catches the Group B family where a single-line
       If-Then-Else got split across two lines (South Park, Black Knight VR
       Room).

4. sub_function_balance
       Count of `Sub X` must equal `End Sub`; same for `Function`/`End Function`
       and `Select Case`/`End Select`. If/End If is not counted because
       single-line Ifs don't need End If, making the count unreliable.

5. single_global_injection
       Each injected global (`Dim vpx_ssc_tmp`, `Dim ssp_newobj`) should appear
       at most once. More = InjectHelpers ran twice or collided with an
       existing table global.

Limitations
-----------
- This is a lint, not a parser. It WILL miss bugs that look locally well-
  formed, and it MAY flag false positives on exotic scripts. Use it as a
  regression gate, not a correctness proof.
- String/comment stripping is best-effort VBS. Tables that put the literal
  text `vpx_ssc_tmp` inside a string or comment could trigger a false
  positive on rule 1; none do today.
"""

import argparse
import os
import re
import sys
from typing import Iterable, List, Tuple

Violation = Tuple[int, str, str]  # (line_number, rule_id, message)

INJECTED_IDENTIFIERS = (
    "vpx_ssc_tmp",
    "ssp_newobj",
    "ssp_idx",
    "ssp_ref",
)

# `vpx_ssc_tmp = EXPR <sep> Select Case vpx_ssc_tmp` — we want the EXPR.
# <sep> is either `:` (old single-line rewrite) or `\r?\n` followed by indent
# (post-916ab32 multi-line rewrite). EXPR can't contain `:` or newlines because
# those are the separators. Case-insensitive.
_SELECT_CASE_REWRITE = re.compile(
    r"vpx_ssc_tmp\s*=\s*(?P<expr>[^:\r\n]+?)\s*(?::|\r?\n)\s*Select\s+Case\s+vpx_ssc_tmp",
    re.IGNORECASE,
)

# Sub / Function / Select Case counting.
# - `(?<!\w)` / `(?!\w)` enforce word boundaries so `MySub` doesn't match `Sub`.
# - `[ \t]+` (not `\s+`) keeps the head on one line, so `Exit Sub\nFoo` doesn't
#   get mis-counted as `Sub Foo`.
# - VBS allows paren-less declarations like `Sub Foo`, so we don't force `(`.
# - BUT we must not match `exit sub end if` as `Sub end`, so we require the
#   identifier after `Sub` to be followed by `(`, `:`, or end-of-line — the
#   only places a real declaration can go.
_PAIR_COUNTS = (
    ("Sub",         re.compile(r"(?im)(?<!\w)Sub[ \t]+\w+(?=[ \t]*(?:\(|:|$))"),
                    re.compile(r"(?i)(?<!\w)End[ \t]+Sub(?!\w)"  )),
    ("Function",    re.compile(r"(?im)(?<!\w)Function[ \t]+\w+(?=[ \t]*(?:\(|:|$))"),
                    re.compile(r"(?i)(?<!\w)End[ \t]+Function(?!\w)")),
    ("Select Case", re.compile(r"(?i)(?<!\w)Select[ \t]+Case(?!\w)"),
                    re.compile(r"(?i)(?<!\w)End[ \t]+Select(?!\w)" )),
)


_REM_AT_STMT_START = re.compile(r"(?i)(?:^|:)\s*\bREM\b")


def strip_vbs_line(line: str) -> str:
    """Remove string literals and trailing comments from a VBS source line.

    VBS comment rules: `'` starts a comment to end-of-line; `REM` is also a
    comment when it appears at the start of a statement (start of line or
    after `:`). Neither applies inside a string literal. String rules:
    double-quoted, `""` is an escaped quote.
    """
    out = []
    i, n = 0, len(line)
    in_string = False
    while i < n:
        c = line[i]
        if in_string:
            if c == '"':
                # Doubled quote inside string = escaped quote, not a terminator.
                if i + 1 < n and line[i + 1] == '"':
                    i += 2
                    continue
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            out.append(" ")  # placeholder keeps column counts roughly right
            i += 1
            continue
        if c == "'":
            break  # rest of line is a comment
        # REM comment: triggered only when REM starts a statement (line start or after `:`),
        # and is a whole word.
        if c in ("R", "r"):
            buffered = "".join(out)
            if _REM_AT_STMT_START.search(buffered + "REM") and \
               line[i:i + 3].lower() == "rem" and \
               (i + 3 >= n or not (line[i + 3].isalnum() or line[i + 3] == "_")):
                break
        out.append(c)
        i += 1
    return "".join(out)


def rule_no_fused_injected_identifier(text: str) -> List[Violation]:
    violations: List[Violation] = []
    combined = "|".join(re.escape(ident) for ident in INJECTED_IDENTIFIERS)
    pattern = re.compile(rf"(?P<ident>{combined})[A-Za-z0-9_]")
    for lineno, raw in enumerate(text.splitlines(), 1):
        stripped = strip_vbs_line(raw)
        for m in pattern.finditer(stripped):
            violations.append((
                lineno,
                "no_fused_injected_identifier",
                f"injected '{m.group('ident')}' fuses with following word char: {raw.strip()[:140]}",
            ))
    return violations


def rule_balanced_select_case_rewrite(text: str) -> List[Violation]:
    violations: List[Violation] = []
    raw_lines = text.splitlines()
    # The rewrite can span two lines (post-916ab32), so scan the cleaned whole
    # file rather than per-line. strip_vbs_line preserves newline positions via
    # the join, so we can still recover the 1-based line number from the
    # match offset.
    cleaned = "\n".join(strip_vbs_line(l) for l in raw_lines)
    for m in _SELECT_CASE_REWRITE.finditer(cleaned):
        expr = m.group("expr")
        opens = expr.count("(")
        closes = expr.count(")")
        if opens == closes:
            continue
        lineno = cleaned.count("\n", 0, m.start()) + 1
        raw = raw_lines[lineno - 1] if 0 <= lineno - 1 < len(raw_lines) else ""
        violations.append((
            lineno,
            "balanced_select_case_rewrite",
            f"Select Case rewrite has unbalanced parens in EXPR "
            f"({opens} open, {closes} close): {raw.strip()[:140]}",
        ))
    return violations


_DANGLING_ELSE = re.compile(r"(?i):\s*Else\s*$")


def rule_no_dangling_else(text: str) -> List[Violation]:
    violations: List[Violation] = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        stripped = strip_vbs_line(raw).rstrip()
        if _DANGLING_ELSE.search(stripped):
            violations.append((
                lineno,
                "no_dangling_else",
                f"line ends with ': Else' (single-line If appears truncated): {raw.strip()[:140]}",
            ))
    return violations


# Regexes for the If/Then/Else structure rule. We scan `:`-split statements,
# so each regex anchors at the START of a statement, not necessarily a line.
# VBS common idioms to handle correctly:
#   - `case 0: if p1 = 0 then ... end if` — multi-line If whose header sits
#     after `:` on the same line as a `Case` label (Remdwaas pattern).
#   - `If x Then y : Else z : End If` — single-line If where the entire
#     remainder of the line is the If's body/alternatives. Once we recognise
#     the single-line If in the first segment, we stop processing subsequent
#     segments on that line.
_IF_ENDIF  = re.compile(r"(?i)^\s*End\s+If\b")
_IF_ELSE   = re.compile(r"(?i)^\s*Else(?!If)(?!\w)")
_IF_ELSEIF = re.compile(r"(?i)^\s*ElseIf\b.+\bThen\b")
# Captures tail after `Then` so we can decide single-line vs multi-line.
_IF_THEN   = re.compile(r"(?i)^\s*If\b.+?\bThen\b(?P<tail>.*)$")


def _fold_line_continuations(lines: List[str]) -> List[str]:
    """Fold VBS ` _` line-continuation: when a line ends with ` _` (or `\\t_`
    or just `_`), glue the next non-blank line onto it. The joined text lives
    at the index of the first fragment; subsequent fragment indices become
    empty strings so line-number alignment with the original source is
    preserved (violations report the first line of the joined header)."""
    result = list(lines)
    # Track exactly which indices we've consumed as continuation targets.
    # Whitespace-only non-consumed lines (e.g. a line whose only content was a
    # string literal that strip_vbs_line reduced to a space) must NOT be
    # skipped — doing so accidentally eats later structural tokens like
    # `End If`.
    consumed = [False] * len(result)
    i = 0
    while i < len(result):
        if consumed[i]:
            i += 1
            continue
        line = result[i].rstrip()
        if line.endswith(" _") or line.endswith("\t_") or line == "_":
            head = line[:-1].rstrip() if line != "_" else ""
            j = i + 1
            while j < len(result) and consumed[j]:
                j += 1
            if j < len(result):
                result[i] = head + " " + result[j].lstrip()
                result[j] = ""
                consumed[j] = True
                continue  # re-check result[i] for further continuation
        i += 1
    return result


def rule_if_then_else_structure(text: str) -> List[Violation]:
    """Walk the file tracking open multi-line If blocks. Flag any `Else`,
    `ElseIf`, or `End If` at statement start with no matching open
    `If ... Then`, and any multi-line `If ... Then` that never gets closed."""
    violations: List[Violation] = []
    raw_lines = text.splitlines()
    cleaned_lines = _fold_line_continuations([strip_vbs_line(l) for l in raw_lines])
    stack: List[Tuple[int, str]] = []  # (lineno, raw snippet) for open blocks

    for lineno, line in enumerate(cleaned_lines, 1):
        raw_snippet = raw_lines[lineno - 1].strip()[:140] if lineno - 1 < len(raw_lines) else ""
        segments = line.split(":")
        # A single-line If consumes the rest of the line as its body. Once we see
        # one, stop processing subsequent `:`-segments on the same line.
        skip_remaining = False
        for seg_idx, seg in enumerate(segments):
            if skip_remaining:
                break
            seg_stripped = seg.strip()
            if not seg_stripped:
                continue
            if _IF_ENDIF.match(seg_stripped):
                if not stack:
                    violations.append((
                        lineno, "if_then_else_structure",
                        f"End If with no matching If ... Then: {raw_snippet}",
                    ))
                else:
                    stack.pop()
                continue
            if _IF_ELSE.match(seg_stripped):
                if not stack:
                    violations.append((
                        lineno, "if_then_else_structure",
                        f"Else with no enclosing multi-line If: {raw_snippet}",
                    ))
                # Common idioms that cram more into the same line as Else:
                # - `else <stmt> end if`  (closes the If)
                # - `Else If <cond> Then` (two-word form starts a nested If
                #   inside the else branch, needing its own End If).
                after_else = seg_stripped[4:]  # skip the literal "else"/"Else"
                nested_if = re.search(r"(?i)\bIf\b.+?\bThen\b(?P<tail>.*)$", after_else)
                if nested_if:
                    nested_tail = nested_if.group("tail").strip()
                    has_later = any(
                        segments[k].strip()
                        for k in range(seg_idx + 1, len(segments))
                    )
                    if nested_tail or has_later:
                        # Single-line nested If; rest of line is its body.
                        skip_remaining = True
                    else:
                        stack.append((lineno, raw_snippet))
                if re.search(r"(?i)\bEnd\s+If\b", seg_stripped):
                    if stack:
                        stack.pop()
                    else:
                        violations.append((
                            lineno, "if_then_else_structure",
                            f"End If (inline after Else) with no matching If: {raw_snippet}",
                        ))
                continue
            if _IF_ELSEIF.match(seg_stripped):
                if not stack:
                    violations.append((
                        lineno, "if_then_else_structure",
                        f"ElseIf with no enclosing multi-line If: {raw_snippet}",
                    ))
                continue
            m = _IF_THEN.match(seg_stripped)
            if m:
                tail_here = m.group("tail").strip()
                # A multi-line If requires Then to be the last non-comment
                # content on the entire line. If there's more content on the
                # same line — either after Then in this segment, or in later
                # `:`-segments — it's a single-line If, and the rest of the
                # line is its body.
                has_later = any(
                    segments[k].strip() for k in range(seg_idx + 1, len(segments))
                )
                if tail_here or has_later:
                    skip_remaining = True
                else:
                    stack.append((lineno, raw_snippet))

    for lineno, snippet in stack:
        violations.append((
            lineno, "if_then_else_structure",
            f"multi-line If ... Then never has matching End If: {snippet}",
        ))
    return violations


def rule_sub_function_balance(text: str) -> List[Violation]:
    violations: List[Violation] = []
    # Strip comments/strings across the whole file before counting.
    cleaned = "\n".join(strip_vbs_line(l) for l in text.splitlines())
    for name, open_re, close_re in _PAIR_COUNTS:
        opens = len(open_re.findall(cleaned))
        closes = len(close_re.findall(cleaned))
        if opens != closes:
            violations.append((
                0,
                "sub_function_balance",
                f"{name}/End {name} imbalance: {opens} open vs {closes} close",
            ))
    return violations


# Matches a single physical line that both starts an `If ... Then` on the SAME
# line AND contains `On Error Goto 0` later on that line. Wine's VBScript
# parser rejects `On Error Goto 0` inside a single-line If body, and catching
# this pattern in the patched output is the reliable way to surface the
# Medieval Madness 3.0 family of crashes (multiple B2SSetData calls wrapped
# with OERN/OEG pairs inside an `If step=2 Then ...` body).
_ON_ERROR_GOTO0_IN_SINGLE_LINE_IF = re.compile(
    r"(?im)^[^\r\n]*\bIf\b.+\bThen\b.+\bOn\s+Error\s+Goto\s+0\b[^\r\n]*$"
)


# Matches `Sub X:End Sub` or `Function X:End Function` on one physical line
# with NOTHING between the `:` after the name and the `End Sub/Function`.
# An empty-body single-line Sub/Function slips past Microsoft VBS but Wine's
# parser rejects the whole script (generic line-1 "Description unavailable").
# Seen in Medieval Madness Bigus MOD 3.0 after PatchControllerPause stripped
# `Controller.Pause = True` from `Sub Table1_Paused:Controller.Pause=True:End Sub`
# leaving `Sub Table1_Paused:End Sub`.
_EMPTY_SINGLE_LINE_SUB_OR_FUNC = re.compile(
    r"(?im)^\s*(?:Public\s+|Private\s+|Static\s+)?"
    r"(Sub|Function)\s+\w+(?:\s*\([^)]*\))?\s*:\s*End\s+\1\s*$"
)


def rule_no_empty_single_line_sub(text: str) -> List[Violation]:
    """Catches `Sub X:End Sub` / `Function X:End Function` with no body.
    Wine VBScript rejects the whole script when this construct appears.
    The patcher must either leave at least one statement in the body or
    expand to a multi-line declaration before stripping content."""
    violations: List[Violation] = []
    raw_lines = text.splitlines()
    cleaned = "\n".join(strip_vbs_line(l) for l in raw_lines)
    for m in _EMPTY_SINGLE_LINE_SUB_OR_FUNC.finditer(cleaned):
        lineno = cleaned.count("\n", 0, m.start()) + 1
        raw = raw_lines[lineno - 1] if 0 <= lineno - 1 < len(raw_lines) else ""
        violations.append((
            lineno,
            "no_empty_single_line_sub",
            f"empty-body single-line {m.group(1)}: {raw.strip()[:140]}",
        ))
    return violations


def rule_no_on_error_goto0_in_single_line_if(text: str) -> List[Violation]:
    """Catches `If x Then ... On Error Goto 0 ...` on one physical line.
    Wine's VBScript parser fails the whole script (with a generic line-1
    "Description unavailable") when this construct appears. The patcher
    must either leave single-line If bodies alone or split them to
    multi-line before wrapping statements."""
    violations: List[Violation] = []
    raw_lines = text.splitlines()
    # Match per-line on cleaned source so string literals and comments can't
    # false-positive. _ON_ERROR_GOTO0_IN_SINGLE_LINE_IF uses multiline mode so
    # `^`/`$` match line boundaries within the joined string.
    cleaned = "\n".join(strip_vbs_line(l) for l in raw_lines)
    for m in _ON_ERROR_GOTO0_IN_SINGLE_LINE_IF.finditer(cleaned):
        lineno = cleaned.count("\n", 0, m.start()) + 1
        raw = raw_lines[lineno - 1] if 0 <= lineno - 1 < len(raw_lines) else ""
        violations.append((
            lineno,
            "no_on_error_goto0_in_single_line_if",
            f"single-line If contains On Error Goto 0 (Wine rejects this): "
            f"{raw.strip()[:160]}",
        ))
    return violations


def rule_single_global_injection(text: str) -> List[Violation]:
    violations: List[Violation] = []
    for ident in ("vpx_ssc_tmp", "ssp_newobj"):
        # `(?im)^\s*Dim\s+<ident>\b` — top-level Dim at start of a line.
        pattern = re.compile(rf"(?im)^\s*Dim\s+{re.escape(ident)}\b")
        count = len(pattern.findall(text))
        if count > 1:
            violations.append((
                0,
                "single_global_injection",
                f"'Dim {ident}' appears {count} times (should be 0 or 1)",
            ))
    return violations


ALL_RULES = (
    rule_no_fused_injected_identifier,
    rule_balanced_select_case_rewrite,
    rule_no_dangling_else,
    rule_if_then_else_structure,
    rule_no_on_error_goto0_in_single_line_if,
    rule_no_empty_single_line_sub,
    rule_sub_function_balance,
    rule_single_global_injection,
)


def check_file(path: str) -> List[Violation]:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as exc:
        return [(0, "read_error", str(exc))]
    violations: List[Violation] = []
    for rule in ALL_RULES:
        violations.extend(rule(text))
    return violations


def iter_vbs(path: str) -> Iterable[str]:
    if os.path.isfile(path):
        if path.lower().endswith(".vbs"):
            yield path
        return
    for root, _, files in os.walk(path):
        for fn in files:
            if fn.lower().endswith(".vbs"):
                yield os.path.join(root, fn)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("paths", nargs="+", help="files or directories to check")
    ap.add_argument("--summary", action="store_true",
                    help="print one-line per file summary only")
    ap.add_argument("--max-per-file", type=int, default=0,
                    help="cap per-file violations shown (0 = unlimited)")
    args = ap.parse_args()

    total_files = 0
    failed_files = 0
    total_violations = 0
    by_rule: dict = {}

    for p in args.paths:
        for vbs in sorted(iter_vbs(p)):
            total_files += 1
            violations = check_file(vbs)
            if not violations:
                continue
            failed_files += 1
            total_violations += len(violations)
            rel = os.path.relpath(vbs)
            if args.summary:
                rules = {}
                for _, rule, _ in violations:
                    rules[rule] = rules.get(rule, 0) + 1
                rule_summary = ", ".join(f"{k}={v}" for k, v in sorted(rules.items()))
                print(f"FAIL {rel}  [{rule_summary}]")
            else:
                print(f"\n{rel}:")
                shown = 0
                for lineno, rule, msg in violations:
                    if args.max_per_file and shown >= args.max_per_file:
                        print(f"  ... ({len(violations) - shown} more)")
                        break
                    prefix = f"  L{lineno}" if lineno else "  --"
                    print(f"{prefix}  [{rule}] {msg}")
                    shown += 1
            for _, rule, _ in violations:
                by_rule[rule] = by_rule.get(rule, 0) + 1

    print()
    print("=" * 60)
    print(f"Checked {total_files} file(s). {failed_files} failed. "
          f"{total_violations} violation(s) total.")
    if by_rule:
        print("By rule:")
        for rule, count in sorted(by_rule.items(), key=lambda kv: -kv[1]):
            print(f"  {count:>5}  {rule}")
    print("=" * 60)
    return 1 if failed_files else 0


if __name__ == "__main__":
    sys.exit(main())

# SimpleScriptPatcher regression tests

## Layer 1 — invariant checker (`check_invariants.py`)

Lint that scans patched `.vbs` output for bug classes the patcher has
historically produced. Each rule catches a *family* of bugs rather than one
specific instance, so it keeps working as the patcher evolves.

### Usage

```
python check_invariants.py <dir-or-file> [<dir-or-file> ...]
python check_invariants.py <path> --summary
python check_invariants.py <path> --max-per-file 5
```

Exits 0 if all files pass, non-zero otherwise.

### Rules

| ID | Catches |
|---|---|
| `no_fused_injected_identifier` | `vpx_ssc_tmp` / `ssp_newobj` / `ssp_idx` / `ssp_ref` followed by a word char (e.g. `vpx_ssc_tmpMOD`, the Castlevania bug). |
| `balanced_select_case_rewrite` | `vpx_ssc_tmp = EXPR : Select Case vpx_ssc_tmp` where EXPR has unbalanced parens (Diner/BK/KISS/American Dad family — patcher's `[^)]+` stops at first `)` in nested calls). |
| `no_dangling_else` | Lines ending with `: Else` (single-line If-Then-Else got split across two lines; South Park / BK VR Room family). |
| `if_then_else_structure` | Stack-based walker that opens on `If ... Then` as a multi-line header, closes on `End If`, and requires `Else` / `ElseIf` / `End If` at statement start to have a matching open block. Catches orphan `End If`, orphan `Else`/`ElseIf`, and unclosed multi-line If blocks — the whole class of "patcher mangled an If-block structurally" bugs. Handles `_` line-continuation headers, `case N: if ... then` post-colon openers, `Else If ... Then` (two-word nested-If form), and `else stmt end if` on one line. Known limitation: treats every `:`-separated segment that starts with an If keyword as a candidate, which is fine for current patches but may need hardening if future patchers produce creative layouts. |
| `no_on_error_goto0_in_single_line_if` | Any single physical line matching `If ... Then ... On Error Goto 0 ...`. Wine's VBScript parser rejects the whole script (with a generic line-1 "Description unavailable") when `On Error Goto 0` appears inside a single-line If body. Hit historically by `PatchControllerChangedLamps` when it wrapped every colon-separated `Controller.B2SSetData` call with `On Error Resume Next : … : On Error Goto 0` — fine at top level, but inside `If step=2 Then call1:call2:call3` that stuffed multiple `On Error Goto 0` into the If body. Medieval Madness Bigus MOD (2.0 and 3.0) hit this; retroactively explains last night's MM 2.0 `Description unavailable` failures. The patcher now skips wrapping inside single-line Ifs — this rule ensures no future patch reintroduces the pattern. |
| `no_empty_single_line_sub` | Any `Sub X:End Sub` or `Function X:End Function` on one physical line with nothing between the colon and the `End` keyword. Wine rejects empty-body single-line Sub/Function declarations with the same generic line-1 error as above. Introduced when `PatchControllerPause` stripped `Controller.Pause = True` from `Sub Table1_Paused:Controller.Pause=True:End Sub`, leaving `Sub Table1_Paused:End Sub`. The patcher now expands these to multi-line Subs with a comment body before stripping the Pause call, which Wine accepts cleanly. |
| `sub_function_balance` | `Sub`/`End Sub`, `Function`/`End Function`, `Select Case`/`End Select` counts don't match. |
| `single_global_injection` | `Dim vpx_ssc_tmp` or `Dim ssp_newobj` appears more than once (InjectHelpers ran twice or collided with a table global). |

### Expected output against today's corpus

Pointing the checker at `crash_investigations/crash_reports_2026-04-22/`
(52 patched scripts captured from on-device crashes) produces:

```
By rule:
  9  balanced_select_case_rewrite    # Group A: nested-paren Select Case
  2  no_dangling_else                 # Group B: split single-line If-Then-Else
  1  no_fused_injected_identifier     # Castlevania pre-fix
```

Every one of these maps to a known patcher bug. Files that don't match these
rules either have non-patcher crashes (FileSystemObject, FlexDMD, etc.) or
ran cleanly — Layer 2 golden snapshots catch those.

### When to run it

- **Before committing a patcher change:** run against `crash_investigations/`.
  Regression if the failure counts go up; improvement if they go down.
- **After a user reports a new crash:** drop the new `.vbs` into the corpus,
  run the checker — if a rule fires, the fix pattern is already identified.
- **CI (future):** once the corpus is committed to this tree, wire
  `python tests/scriptpatcher/check_invariants.py tests/scriptpatcher/corpus`
  into CTest.

### Limitations

- Line-oriented lint, not a parser. Can miss bugs that look locally
  well-formed across multiple lines.
- String and `'`/`REM` comment stripping is best-effort; exotic scripts
  that embed the literal text `vpx_ssc_tmp` inside a string or comment
  could trip rule 1. None in today's corpus do.
- Paren-balance for `balanced_select_case_rewrite` is per-EXPR inside the
  rewrite pattern, not whole-file. That's intentional — whole-file balance
  is not a useful signal on tables that contain generated code or data tables.

## Layer 2 — golden snapshot tests

Planned, not yet built. See `TESTING_PLAN.md` at the same path for design.
Corpus seed candidates (from today's triage): Castlevania SOTN 1.1 (fused-
identifier regression), Diner 10.2 (nested-paren), South Park (dangling
Else), a known-passing table, Medieval Madness (line-1 injection corruption).

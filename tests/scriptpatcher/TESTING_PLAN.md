# SimpleScriptPatcher regression testing plan

## Problem

Changes to `src/ui/simplescriptpatcher.cpp` have repeatedly broken real tables
in ways that only surface when the table is loaded on device and the VBS
compiler hits the bad line. Recent example: the `vpx_ssc_tmpMOD 10` bug in
Castlevania SOTN 1.1 — the patcher emitted a fused identifier whenever the
original script had a word operator (`MOD`, `AND`, `OR`, etc.) immediately
after `)` with no space. Undetectable without running the table.

Current state of `tests/scriptpatcher/`:
- `test_external_vbs.py` is a skeleton. It documents expected regex patterns
  for a handful of transformations but never actually invokes the C++ patcher.
- `vbs_samples/` contains the input snippets but no golden outputs.
- No CMake target, no CTest wiring, no CI.

Net coverage today: zero automated regression detection.

## Goal

Any future change to `simplescriptpatcher.cpp` should produce a clear pass/fail
signal against a corpus of real tables before the change ever ships to device.

## Approach: two layers

### Layer 1 — Invariant checks (cheap, catches whole bug classes)

Rules the patcher's output must always satisfy regardless of input. Run these
against any patched `.vbs` and fail loudly on violation.

Concrete invariants to implement:

1. **No fused injected identifiers.** For every identifier the patcher
   injects (`vpx_ssc_tmp`, `vpx_ssp_idx`, and any future `vpx_*_*`), the
   output must never contain the name immediately followed by a word character.
   Regex: `vpx_ssc_tmp[A-Za-z0-9_]`. Match = fail.
   - This single rule would have caught the Castlevania bug on day one.

2. **Keyword boundary preservation.** The patcher must never emit a word
   operator (`MOD|AND|OR|XOR|NOT|IS|EQV|IMP`) touching an identifier character
   on either side. Regex: `[A-Za-z0-9_](MOD|AND|OR|XOR|NOT|IS|EQV|IMP)\s` or
   `\s(MOD|AND|OR|XOR|NOT|IS|EQV|IMP)[A-Za-z0-9_]`.

3. **Structural integrity.** Count of `Sub`/`End Sub`, `Function`/`End Function`,
   `If`/`End If`, `Select Case`/`End Select` in the output must equal the
   input counts (or differ only by known additions the patcher is allowed to
   make — document which).

4. **No duplicate global injections.** Exactly one `Dim vpx_ssc_tmp` in the
   output. Same for every other injected global.

5. **Parenthesis balance preserved.** Count of `(` == count of `)` in the
   output (after stripping string literals and comments).

**Implementation:** ~80 lines of Python in
`tests/scriptpatcher/check_invariants.py`. Takes a directory of `.vbs` files,
runs the rules, exits non-zero on violation. No C++ build required — can be
pointed at any `.vbs` output including the post-crash reports in
`C:\Android\com.retroeki.pinball\logs\`.

**Why it's worth doing even without Layer 2:** invariants are maintenance-free.
They catch the *class* of bug (fused identifier, unbalanced keyword boundary)
without needing per-table golden files. A new table that happens to hit a new
regex edge case will fail the invariant check automatically.

### Layer 2 — Golden snapshot tests (catches unexpected behavioral changes)

Commit a corpus of real pre-patch `.vbs` inputs with their expected post-patch
outputs. On any patcher change, diff actual output against committed golden.
Any diff demands review.

**Structure:**
```
tests/scriptpatcher/
  corpus/
    inputs/              # pre-patch scripts (committed)
      castlevania_sotn_1_1.vbs
      black_knight_2000.vbs
      ... (5–10 tables to start)
    goldens/             # expected post-patch output (committed)
      castlevania_sotn_1_1.vbs
      ...
  cpp_test.cpp           # minimal harness
  run_corpus_tests.sh    # runs harness, diffs vs goldens
```

**Harness (`cpp_test.cpp`):**
- Links against `simplescriptpatcher.cpp` and its deps (re2, plog).
- Takes `<input_dir> <output_dir>` argv.
- For each `.vbs` in input_dir: reads, calls `SimpleScriptPatcher::Patch()`,
  writes to output_dir.
- No table loading, no SDL, no renderer — just the patcher.

**CMake:** add a `vpinball-patch-test` executable target in `tests/CMakeLists.txt`
(or top-level if no tests CMakeLists exists yet). Wire to CTest via `add_test`
so `ctest -R patcher` runs it. Keep the target out of the default build so
`make vpinball` stays fast.

**Runner:**
```bash
./vpinball-patch-test corpus/inputs build/test_outputs
diff -r build/test_outputs corpus/goldens
```

**Workflow on intentional patcher changes:**
1. Edit `simplescriptpatcher.cpp`.
2. Run `ctest -R patcher` — see which tables changed and how.
3. Audit each diff: intended? unintended?
4. If intended: `cp build/test_outputs/*.vbs corpus/goldens/`, review with
   `git diff`, commit.
5. If unintended: fix the patcher, repeat.

This is the safety net. You can't land a patcher change without knowing
exactly which tables it moves.

## Corpus sourcing

Layer 2 needs pre-patch `.vbs` files. Two options:

**Option A — extract from `.vpx` on demand.**
VPX tables are OLE compound files; the script lives in a stream named
`GameStg/GameItem` (or similar) and can be extracted with a short Python
script using `olefile`. One-time extraction of 5–10 tables into
`corpus/inputs/` and commit them.

**Option B — instrument the patcher to dump pre-patch scripts.**
Add an env var like `VPX_DUMP_PREPATCH=/sdcard/Android/data/.../prepatch/`
that, when set, writes the original `.vbs` to disk before patching. Load
each candidate table once with the flag set, pull the files off device,
commit into `corpus/inputs/`.

Recommendation: start with A for 5 tables (Castlevania SOTN + 4 others from
`logs/crash_reports_2026-04-16/` that have had issues historically). Add B
later if we want larger corpus growth without manual extraction.

## First corpus entry: Castlevania SOTN

The bug that motivated this plan — keep it as the canonical regression case.

**Pre-patch line (input):**
```vbs
Sub UpdateBonusLights
    Select Case BonusPoints(CurrentPlayer)MOD 10
        Case 0:bl1.State = 0 ...
```

**Expected post-patch (golden, with the fix):**
```vbs
Sub UpdateBonusLights
    vpx_ssc_tmp = BonusPoints(CurrentPlayer) : Select Case vpx_ssc_tmp MOD 10
        Case 0:bl1.State = 0 ...
```

**Pre-fix output (must fail the test):**
```vbs
    vpx_ssc_tmp = BonusPoints(CurrentPlayer) : Select Case vpx_ssc_tmpMOD 10
```
Both Layer 1 (invariant #1 regex match) and Layer 2 (golden diff) would
catch this.

## Suggested execution order for tonight

1. **Layer 1 first.** ~30 min. No build system changes.
   - Write `check_invariants.py`.
   - Run it against every `.vbs` in `C:\Android\com.retroeki.pinball\logs\`
     — establishes baseline, may surface latent issues in already-shipped
     patches.
   - Commit.

2. **Layer 2 scaffolding.** ~1–2 hours.
   - Write the minimal `cpp_test.cpp`.
   - Add CMake target.
   - Extract Castlevania SOTN pre-patch script (Option A) → `corpus/inputs/`.
   - Run harness → capture output → commit as golden.
   - Verify: revert the `" "` fix in `simplescriptpatcher.cpp:1483`, rerun,
     confirm test fails; re-apply fix, confirm test passes.

3. **Corpus expansion.** Incremental.
   - Add 4 more tables over time as issues surface.

## Non-goals

- Full VBScript parser. A tokenizer good enough for the invariants above is
  enough. Anything deeper is re-implementing Wine's VBScript and will
  eat weeks.
- Running the tables. This is a *patcher*-level test suite. Whether a
  patched script actually plays correctly on device is a separate problem
  (belongs in device-level smoke testing, not here).
- Testing the other patchers in the same family (`scriptpatcher.cpp`,
  `scriptpatcher_wine.cpp`, class transforms) in this first pass. Same
  harness can extend to them later — keep scope tight.

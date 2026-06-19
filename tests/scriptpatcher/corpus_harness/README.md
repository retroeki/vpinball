# Whole-patcher corpus harness (Layer 2)

Runs the **real** `SimpleScriptPatcher::PatchScript()` over an entire library of
extracted table scripts, so the *complete* patcher pipeline can be linted and
diffed at corpus scale. This is the "Layer 2 golden snapshot" harness that
[`../TESTING_PLAN.md`](../TESTING_PLAN.md) lists as planned.

Unlike `../test_controller_pause_if.py` (which re-implements one rule's regex in
Python), this compiles the **actual C++ patcher** against the **vendored RE2**,
so it is byte-faithful to the on-device engine and never drifts out of sync with
the C++ source.

## What it exercises

The whole `PatchScript` pipeline, in order:

- every **enabled core rule** (`PatchMultiplicationInSubCall`, `PatchControllerPause`, …),
- `InjectHelpers` (when any rule fired),
- the per-table `tablepatches::ApplyTableSpecificPatches` layer (keyed on the `.vpx` filename),
- the final CRLF-normalize + non-ASCII-sanitize pass.

The input file's stem is reused as `"<stem>.vpx"` so the filename-keyed
per-table layer fires exactly as on device.

## How it builds

Self-contained: `patch_corpus.cpp` + `src/ui/simplescriptpatcher.cpp` +
`src/ui/tablepatches.cpp`, with a minimal [`stdafx.h`](stdafx.h) shim in place of
the engine's heavy precompiled header (the patcher uses no app types -- only
std, plog, and RE2). Abseil + RE2 come from the repo's vendored sources via
`add_subdirectory`, mirroring the main build.

## Running it

```bash
# 1. Extract pre-patch scripts (Windows -- olefile lives there):
python ../extract_vpx_scripts.py "G:\VPX Files" "C:\vpx_patcher_corpus\scripts_raw"

# 2. Build + patch + lint (WSL -- needs g++/cmake):
./run.sh /mnt/c/vpx_patcher_corpus/scripts_raw /mnt/c/vpx_patcher_corpus
```

`run.sh` builds `patch_corpus`, runs it over the corpus (writing patched `.vbs`
+ a `patch_report.csv` of which tables changed and why), then lints the patched
output with [`../check_invariants.py`](../check_invariants.py) -- the structural
oracle that flags the bug *classes* the patcher has historically produced
(dangling `If`, unbalanced `Sub`, empty single-line sub, …). Any violation =
a table the current patcher turns into structurally-broken VBScript.

Manual invocation:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/patch_corpus <in_dir> <out_dir> [report.csv]
python3 ../check_invariants.py <out_dir> --summary
```

## Golden baseline (regression guard for future patcher changes)

Snapshot the patched output of a known-good patcher state:

```bash
cp -r /mnt/c/vpx_patcher_corpus/scripts_patched /mnt/c/vpx_patcher_corpus/baseline
```

After any later patcher change, re-run and diff against the baseline:

```bash
diff -rq /mnt/c/vpx_patcher_corpus/baseline /mnt/c/vpx_patcher_corpus/scripts_patched
```

Every changed file is a table whose patched output moved -- review each as
*intended fix* vs *regression*. Combined with the invariant lint, this is the
no-regression gate for editing `SimpleScriptPatcher`.

## Caveats

- The structural lint is a **line-oriented** checker, not a VBScript parser; it
  catches the historical patcher bug *classes*, not arbitrary semantic errors.
  The ultimate oracle is a genuine Wine VBScript compile (device ground-truth) --
  a heavier future escalation, not wired in here.
- `patch_corpus` always exits 0 (patching is not pass/fail); the lint step is
  what fails on a violation.
- Keep generated corpora (`scripts_raw`, `scripts_patched`, `baseline`, build
  dir) **out of the repo** -- they are large. Point the harness at a scratch dir
  like `C:\vpx_patcher_corpus\`.

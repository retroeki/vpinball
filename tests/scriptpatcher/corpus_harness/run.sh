#!/usr/bin/env bash
# license:GPLv3+
#
# Whole-patcher corpus test driver. Run in WSL (needs g++/cmake; the build
# compiles the vendored abseil + RE2).
#
#   ./run.sh <raw_scripts_dir> <work_dir>
#
# Prerequisite -- extract the pre-patch scripts first (on Windows, where the
# olefile package lives):
#
#   python ../extract_vpx_scripts.py "G:\VPX Files" "<raw_scripts_dir>"
#
# Then this driver: builds patch_corpus, runs the REAL patcher over every
# script, and lints the patched output with the structural invariant checker.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
RAW="${1:?usage: run.sh <raw_scripts_dir> <work_dir>}"
WORK="${2:?usage: run.sh <raw_scripts_dir> <work_dir>}"

BUILD="$WORK/build"
PATCHED="$WORK/scripts_patched"
REPORT="$WORK/patch_report.csv"

echo "=== configure + build patch_corpus ==="
cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target patch_corpus -j"$(nproc)"

echo
echo "=== run the real SimpleScriptPatcher over the corpus ==="
"$BUILD/patch_corpus" "$RAW" "$PATCHED" "$REPORT"

echo
echo "=== invariant lint (structural oracle) ==="
# Non-zero exit if any patched script violates a known bug-class invariant.
python3 "$HERE/../check_invariants.py" "$PATCHED" --summary

#!/usr/bin/env python3
"""
Extract the VBScript from a folder of .vpx tables into .vbs files.

VPX tables are OLE2 compound files; the script lives in the `GameStg/GameData`
stream as a BIFF `CODE` record (4-byte length prefix + raw text). This realizes
"Corpus sourcing, Option A" from TESTING_PLAN.md: pull pre-patch scripts so a
patcher change can be diffed across a real table library.

Usage:
    python tests/scriptpatcher/extract_vpx_scripts.py <vpx_dir> <out_dir>

Then lint / diff the extracted (pre-patch) or patched output with
check_invariants.py. Requires `olefile` (pip install olefile).
"""
import glob
import os
import struct
import sys

import olefile


def extract_code(path):
    ole = olefile.OleFileIO(path)
    try:
        target = None
        for s in ole.listdir():
            if s[-1] == "GameData" and "GameStg" in s:
                target = s
                break
        if target is None:
            for s in ole.listdir():
                if s[-1] == "GameData":
                    target = s
                    break
        if target is None:
            return None
        data = ole.openstream(target).read()
    finally:
        ole.close()
    idx = data.find(b"CODE")
    if idx < 0:
        return None
    p = idx + 4
    slen = struct.unpack_from("<I", data, p)[0]
    if slen <= 0 or slen > len(data):
        return None
    return data[p + 4:p + 4 + slen].decode("latin-1")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    files = sorted(glob.glob(os.path.join(src, "*.vpx")))
    ok, fail = 0, []
    for f in files:
        base = os.path.splitext(os.path.basename(f))[0]
        try:
            code = extract_code(f)
        except Exception as exc:  # noqa: BLE001 - report and continue
            fail.append((base, f"error: {exc}"))
            continue
        if not code:
            fail.append((base, "no CODE/GameData stream"))
            continue
        with open(os.path.join(out, base + ".vbs"), "w", encoding="latin-1") as o:
            o.write(code)
        ok += 1
    print(f"Extracted {ok}/{len(files)} scripts to {out}")
    for base, why in fail:
        print(f"  SKIP {base}: {why}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

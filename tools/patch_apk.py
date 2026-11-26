#!/usr/bin/env python3
"""Replace the ARM64 shotmeter library in an APK without changing other payloads."""
import argparse
import os
import re
import zipfile

TARGET = "lib/arm64-v8a/libshotmeter.so"
REQUIRED_GAME_LIB = "lib/arm64-v8a/libnba2k20_clean_opt.so"
SIGNATURE_RE = re.compile(r"^META-INF/[^/]+\.(?:RSA|DSA|EC|SF)$", re.I)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("base_apk")
    p.add_argument("shotmeter_so")
    p.add_argument("output_apk")
    a = p.parse_args()
    for path in (a.base_apk, a.shotmeter_so):
        if not os.path.isfile(path):
            p.error("input file not found: " + path)
    os.makedirs(os.path.dirname(os.path.abspath(a.output_apk)), exist_ok=True)
    with zipfile.ZipFile(a.base_apk, "r") as zin:
        names = zin.namelist()
        if names.count(TARGET) != 1:
            raise SystemExit(f"Expected exactly one {TARGET}; found {names.count(TARGET)}")
        if REQUIRED_GAME_LIB not in names:
            raise SystemExit(f"Base APK is missing required game library: {REQUIRED_GAME_LIB}")
        with open(a.shotmeter_so, "rb") as f:
            replacement = f.read()
        if replacement[:4] != bytes((0x7f, 0x45, 0x4c, 0x46)) or len(replacement) < 64:
            raise SystemExit("Replacement is not a valid ELF shared library")
        # AArch64 ELF machine value is 183, little-endian.
        if replacement[18:20] != bytes((0xb7, 0x00)):
            raise SystemExit("Replacement ELF is not AArch64")
        with zipfile.ZipFile(a.output_apk, "w") as zout:
            zout.comment = zin.comment
            for info in zin.infolist():
                upper = info.filename.upper()
                if info.filename == TARGET:
                    zout.writestr(info, replacement)
                elif upper.startswith("META-INF/") and (
                    SIGNATURE_RE.match(info.filename) or upper == "META-INF/MANIFEST.MF"
                ):
                    # v1 signature metadata is invalidated by any archive change.
                    continue
                else:
                    zout.writestr(info, zin.read(info.filename))
    with zipfile.ZipFile(a.output_apk, "r") as check:
        bad = check.testzip()
        if bad:
            raise SystemExit("Archive CRC verification failed at " + bad)
        if TARGET not in check.namelist():
            raise SystemExit("Patched library missing from output")
    print("Patched APK:", a.output_apk)
    print("Replaced:", TARGET)
    print("Removed stale v1 signature metadata; output must be signed before install")

if __name__ == "__main__":
    main()

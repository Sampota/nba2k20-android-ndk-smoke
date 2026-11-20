# NBA 2K20 Mobile Shotmeter — Project Handoff v31
Date: 2026-11-20

## New verified result: OBB backup is readable on Android 15

User installed and ran OBB Access Probe v1.1 (package `com.sampota.nba2k20obbprobe`) and supplied its complete report.

Device:
- Android 15 / API 35
- External storage mounted
- `READ_EXTERNAL_STORAGE` granted
- Probe reports legacy external-storage mode true

Original game's app-specific OBB directory:
- `/storage/emulated/0/Android/obb/com.t2ksports.nba2k20and`
- Does not exist on this device.

User's backup directory:
- `/storage/emulated/0/Download/com.t2ksports.nba2k20and`
- Exists, is a directory, and is readable.
- Contains exactly the expected two OBB files.

Verified files:
- `main.98.com.t2ksports.nba2k20and.obb`
  - 1,953,173,504 bytes
  - Exists, is a regular file, canRead=true
  - Read-only open succeeded; 4 bytes read
  - First bytes: `BF B3 00 AA`
- `patch.98.com.t2ksports.nba2k20and.obb`
  - 1,382,148,096 bytes
  - Exists, is a regular file, canRead=true
  - Read-only open succeeded; 4 bytes read
  - First bytes: `BF B3 00 AA`

Total OBB data: 3,335,321,600 bytes (about 3.11 GiB).
The reported lengths exactly match the expected sizes from the original APK's `assets/url/gameobb.json` audit.

Probe v1.1 build:
- GitHub Actions run: https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44677754592
- Run ID: 44677754592
- Branch: `obb-access-probe`
- Commit: `a528851f27c5463c245220240ff8511c37e0d823`
- Conclusion: success
- Artifact ID: 14602577730
- Artifact name: `nba2k20-obb-access-probe-api28`

Safety:
- The probe performed no file creation, modification, movement, rename, or deletion.
- The original game OBB directory remains absent.
- Both backup OBBs remain in Downloads.
- Do not move, rename, delete, or overwrite the user's backup files.
- No OBB copy has been authorized or performed.

## Updated project assessment

OBB source discovery and read access are now confirmed. This removes the previous uncertainty about whether the user has usable expansion files and whether the diagnostic app can read them on Android 15.

This does NOT yet prove that a separately signed/repackaged game can locate or consume the OBBs. The original game package's signing key is unavailable, so a modified build cannot be installed as an update to the original package. The modified build will need a distinct package identity, and its expected OBB directory and filenames must be reconciled with the package identity and expansion-downloader code.

## Next engineering steps

1. Inspect the exact OBB path/name resolution behavior in the APK's expansion downloader and app bootstrap. Confirm whether `Context.getObbDir()`, `Context.getPackageName()`, and `gameobb.json` determine the path and filename.
2. Select the final modified APK package ID before any OBB staging. Do not assume the diagnostic package ID is the final game package ID.
3. Design a controlled Storage Access Framework (SAF) import flow that lets the user grant access to the existing Downloads folder and copies the two files into the final app's own OBB directory, using the exact names required by the final package.
4. Before copying, check available storage and verify expected source lengths. Preserve the user's Downloads originals. Copy only after explicit user approval, and verify destination sizes and hashes.
5. Do not build/install the modified game APK until the package/OBB resolution plan is verified. Do not claim in-game readiness until a separate modified APK has been installed, launched, and tested.

## Overall status

- OBB backup exists: VERIFIED
- Both OBB files readable from Downloads on Android 15: VERIFIED
- Original game OBB directory exists: NO
- OBB staging/copy to modified package: NOT DONE
- Modified game APK packaging/install: NOT DONE
- Native hook runtime verification: NOT DONE
- Shotmeter / GREEN GIANT in-game behavior: NOT DONE

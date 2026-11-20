# NBA 2K20 Mobile Shotmeter — v30 OBB Diagnostic Build Handoff
Date: 2026-11-20

Append-only continuation of v29. The diagnostic APK is separate from the game project and does not change the original APK, OBB files, IFF, or native library.

## 1. Read-only OBB access diagnostic APK built successfully

A separate package was built:
- Package ID: `com.sampota.nba2k20obbprobe`
- APK: `app-debug.apk`
- User-facing filename: `nba2k20-obb-access-probe.apk`
- Size: 13,172 bytes
- SHA-256: `e0bcdfb0ea5760403d2bff3e839e128e1bb1a0e9b7d36c93b99283c11a21bb9c`
- GitHub Actions run #2: https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44676863301
- Run ID: `44676863301`
- Head commit: `6759ce7fadefda0acc8b8b4e34214f5a7d990f71`
- Build job ID: `136090654181`, conclusion `success`
- Artifact ID: `14602113582`
- Artifact: `nba2k20-obb-access-probe-api28` (14-day artifact retention)
- Artifact digest: `sha256:4d3fe8508e120295bcab53249c8a9696d5367b99de64517c919a11aced014b53`

The archive was downloaded, extracted and checked. It contains a single `app-debug.apk` (13,172 bytes). The APK itself was parsed and verified:
- Android package: `com.sampota.nba2k20obbprobe`
- min SDK 21 / target SDK 28 / compile SDK 35
- `debuggable=true`
- `allowBackup=false`
- `requestLegacyExternalStorage=true`
- sole declared permission: `android.permission.READ_EXTERNAL_STORAGE`
- no INTERNET permission
- no WRITE_EXTERNAL_STORAGE permission
- no MANAGE_EXTERNAL_STORAGE permission
- single launcher Activity: `com.sampota.nba2k20obbprobe.MainActivity`
- JAR signing verification succeeds with its debug signer (self-signed debug certificate as expected).

## 2. Diagnostic behavior

The app displays the current Android version, API level, package name, runtime READ_EXTERNAL_STORAGE grant, external storage state, legacy-storage flag, and this diagnostic app's own `getObbDir()`.

It then checks the expected original game directory:
`Android/obb/com.t2ksports.nba2k20and`

Expected files:
- `main.98.com.t2ksports.nba2k20and.obb`
- `patch.98.com.t2ksports.nba2k20and.obb`

For both it reports exists/isFile/canRead/length. If present, it attempts to open read-only and read only four bytes. It does not read the whole file and it has no code path that writes, creates, moves, renames, copies, or deletes files.

## 3. User-side diagnostic procedure

1. Install `nba2k20-obb-access-probe.apk` as a separate utility. It has a different package ID from NBA 2K20 and will not update or replace the game.
2. Launch **NBA 2K20 OBB Access Probe**.
3. Grant the requested **read-only Files/Media** permission, if Android prompts.
4. Tap **Run read-only OBB check**.
5. Tap **Copy report** and paste the complete report into the project chat.

Do not grant any write or all-files permission; the diagnostic does not request those permissions. Do not uninstall NBA 2K20, move/copy/rename its OBB files, or otherwise alter the original game data.

## 4. Interpretation

- Both OBB files exist and read-only opens succeed: direct reuse from this separate identity may be feasible, but the actual NBA 2K20 downloader/game loader must still be validated before building its separate-package APK.
- Files exist but opening is denied: the separate-package path cannot safely assume access to the original expansion files.
- Files are absent: inspect the actual external-storage path and game installation state; do not relocate any data based on a guess.
- `getObbDir()` will show the diagnostic package's own OBB directory; this should differ from the original package directory.

The result is an empirical permission/path check, not a test of the game loader itself.

## 5. Current project status

Native v26 four-library candidate remains available separately. The OBB diagnostic app is built and verified, but has not yet been installed/run on the user's phone.

Still unverified:
- target-device read access to original OBB files from the separate package
- actual runtime loading of the four native libraries
- ShadowHook callback execution in NBA 2K20
- a safe path around original package signing and game OBB requirements
- second independent meter renderer and GREEN GIANT behavior

**Status: OBB diagnostic APK BUILT AND STATICALLY VERIFIED; AWAITING READ-ONLY DEVICE REPORT. GAME MOD APK NOT BUILT OR TESTED.**

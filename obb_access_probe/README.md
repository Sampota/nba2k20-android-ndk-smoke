# NBA 2K20 OBB Access Probe

A separate, read-only Android diagnostic APK. It is not the game and does not include or modify any NBA 2K20 files.

## Purpose

Check whether an app installed under a separate package identity can read the existing OBB files at:

- `Android/obb/com.t2ksports.nba2k20and/main.98.com.t2ksports.nba2k20and.obb`
- `Android/obb/com.t2ksports.nba2k20and/patch.98.com.t2ksports.nba2k20and.obb`

The diagnostic package is `com.sampota.nba2k20obbprobe` and targets API 28 to mirror the legacy-storage target of the supplied game APK. It requests READ_EXTERNAL_STORAGE only. It does not request write access, all-files access, or internet access.

The app reports:
- Android version and SDK level
- whether legacy external-storage mode is active
- the diagnostic app's own `getObbDir()`
- existence/readability and actual size of each original OBB
- whether opening each file read-only and reading its first four bytes succeeds

The app never reads the full OBB. It never creates, writes, moves, renames, or deletes files.

## Build

Use Gradle 8.9, JDK 17, Android Gradle Plugin 8.7.3, and Android SDK 35:

`gradle --no-daemon :app:assembleDebug`

Output:
`app/build/outputs/apk/debug/app-debug.apk`

## Interpretation

- Both OBB files open successfully: a separate-package route may be able to reuse the existing expansion data, subject to testing the game loader itself.
- File exists but open fails with a permission error: direct reuse is blocked for this test identity; do not build the game APK on the assumption it can read those files.
- Files are missing: this device path or OBB installation is different; do not copy or modify anything until the path is confirmed.

The diagnostic is deliberately independent of the original game package and APK.

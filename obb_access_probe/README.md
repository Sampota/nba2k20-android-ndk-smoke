# NBA 2K20 OBB Access Probe

A separate, read-only Android diagnostic APK. It is not the game and does not include or modify any NBA 2K20 files.

## Purpose

Check whether a separate package can read the original expansion files in the standard OBB directory and inspect the user's already-existing backup folder in Downloads:

- `Android/obb/com.t2ksports.nba2k20and/`
- `Download/com.t2ksports.nba2k20and/`

The diagnostic package is `com.sampota.nba2k20obbprobe`, version 1.1, and targets API 28. It requests READ_EXTERNAL_STORAGE only. It does not request write access, all-files access, or internet access.

It reports:
- Android version, SDK level, and legacy-storage status
- the diagnostic app's own `getObbDir()`
- existence and readability of the original game OBB directory
- existence, names, sizes and readability of files in the existing Downloads backup folder
- whether the expected main/patch OBB files can be opened read-only and their first four bytes read

The app never reads an entire OBB file. It never creates, writes, moves, renames, copies, or deletes files.

## Build

Use Gradle 8.9, JDK 17, Android Gradle Plugin 8.7.3, and Android SDK 35:

`gradle --no-daemon :app:assembleDebug`

## Interpretation

- If the Downloads folder lists the two OBBs and both read-only open tests succeed, a safe manual copy plan can be evaluated. No copy is performed by the diagnostic.
- If the files are present but opening is denied, use Android's system file picker to grant read access rather than changing permissions or moving data.
- If the filenames differ, report the exact names and sizes; do not rename anything yet.

The diagnostic is deliberately independent of the original game package.

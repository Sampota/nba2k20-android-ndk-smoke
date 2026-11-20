# NBA 2K20 Mobile Shotmeter — v28 APK / OBB Integration Audit
Date: 2026-11-20

Append-only continuation of v27. No original APK, OBB, game library, or IFF was modified.

## 1. Original APK revalidated

Materialized the Library copy of `NBA 2K27_98.0.2.apk` and rechecked its SHA-256:
`6f949b86daec9eb76275a7942a0db01afad8747de1fdd89100bb2eab1c5d0d86`

APK archive directory opened successfully. It contains 1,384 ZIP entries. The ARM64 native library directory includes:
- `libc++_shared.so`
- `libfmod.so`
- `libnba2k20_clean_opt.so` (original filename in APK)
- Other original support libraries

There are no `.obb` files inside the APK archive.

The prior manifest audit remains applicable:
- package: `com.t2ksports.nba2k20and`
- version: 98.0.2 / code 98
- minSdk 18 / targetSdk 28
- `extractNativeLibs=true`
- Application: `sPfSUumNwmBLUIcV.ProxyApplication`
- component factory: `sPfSUumNwmBLUIcV.ProxyComponentFactory`
- game NativeActivity: `com.t2ksports.nba2k20and.gameactivity`
- manifest-selected native library: `nba2k20_clean_opt`

## 2. OBB configuration confirmed

The valid JSON file `assets/url/gameobb.json` was read directly from the APK. Its SHA-256 is:
`ed4321c3ba209cb4c1ef730bb2ba5dc5b820f9034b234454f7d054634f15a115`

It specifies:
- `saveDir`: `android/obb/com.t2ksports.nba2k20and`
- game class: `com.t2ksports.nba2k20and.gameactivity`
- main OBB: `main.98.com.t2ksports.nba2k20and.obb`, declared length 1,953,173,504 bytes
- patch OBB: `patch.98.com.t2ksports.nba2k20and.obb`, declared length 1,382,148,096 bytes
- combined declared OBB size: 3,335,321,600 bytes (3.106 GiB)

The APK DEX contains references to `getObbDir`, `getObbDirs`, `OBBDownloader`, and `canWriteOBBFile`. These strings indicate package-sensitive OBB handling needs further control-flow/decompilation validation; strings alone do not prove the exact path resolution behavior.

The separate `assets/obb.json` starts with similar downloader metadata but has trailing extra data and is not valid standalone JSON. Use the well-formed `assets/url/gameobb.json` when inspecting the intended game OBB values.

## 3. Package-identity risk

The OBB configuration and target filenames explicitly refer to the original package identity. A separate test APK package name may change the app-specific OBB directory returned by Android APIs and may not find the existing main/patch OBB files.

Do not assume that an alternate package can read the original package's OBB directory on the user's Android 15 device. Do not copy or rename 3.106 GiB of OBB data without a deliberate, verified plan. Do not ask the user to uninstall or overwrite the original game.

The original APK's v2 signing key is still unavailable. A rebuilt APK signed with a different key cannot be installed as an update over the existing same-package app. A new package identity avoids overwriting the original but introduces the OBB/storage issue above.

## 4. Consequence for the next implementation step

Do not build or install an APK yet.

Next inspect the DEX/application startup path around `ProxyApplication`, `ProxyComponentFactory`, `OBBDownloader`, `getObbDir`, and `canWriteOBBFile`. Determine whether:
1. the game can be packaged as a separate identity while using the original OBB files safely without copying them;
2. the game has a configurable path that can be changed without modifying the original OBB;
3. a same-package test install would require explicit user approval to remove/replace the original app, and how app data would be preserved/restored.

Also confirm the exact target Android storage behavior before relying on cross-package OBB access. Only then choose an APK strategy.

**Status: four-library ARM64 runtime candidate exists; original APK/OBB storage strategy is not resolved; no APK packaged, signed, installed, or runtime-tested.**

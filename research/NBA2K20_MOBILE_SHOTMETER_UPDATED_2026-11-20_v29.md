# NBA 2K20 Mobile Shotmeter — v29 Package / OBB / Signing Audit
Date: 2026-11-20

Append-only continuation of v28. No APK, OBB, original native library, or IFF was modified.

## 1. Embedded downloader code confirms package-bound OBB lookup

The original APK contains a valid embedded DEX at outer APK `classes.dex` offset `15467428` (DEX 035, declared size 5,749,324 bytes; Adler32 and SHA-1 header checks pass).

Relevant embedded class:
`Lcom/google/android/vending/expansion/downloader/Helpers;`

Static DEX inspection shows:
- `getSaveFilePath(Context)` checks `Build.VERSION.SDK_INT`. For API 19+, it calls `Context.getObbDir()` and returns that directory's path. For older API levels, it composes external storage + the expansion path + `Context.getPackageName()`.
- `getExpansionAPKFileName(Context, boolean, int)` composes either the main or patch filename from the expansion prefix, version, `Context.getPackageName()`, and `.obb`.
- `generateSaveFileName(Context, String)` uses `getSaveFilePath(Context)`.
- The manifest declares `com.t2ksports.nba2k20and.downloadservice`, which extends the embedded Google Expansion Downloader service class.

The APK's well-formed `assets/url/gameobb.json` declares:
- save directory: `android/obb/com.t2ksports.nba2k20and`
- main file: `main.98.com.t2ksports.nba2k20and.obb`, declared 1,953,173,504 bytes
- patch file: `patch.98.com.t2ksports.nba2k20and.obb`, declared 1,382,148,096 bytes
- combined declared size: 3,335,321,600 bytes (~3.106 GiB)

Neither OBB file is inside the APK or currently present in the Project Library.

## 2. The visible Application wrapper does not restore the old package ID

Primary DEX inspection of `sPfSUumNwmBLUIcV.ProxyApplication` shows:
- `attachBaseContext()` stores the result of native `JniBridge.rapn()` into an instance field.
- `getPackageName()` tests whether that field is empty. If empty it returns an empty string; if non-empty it delegates to `Application.getPackageName()`.
- It does not return the stored native string as a replacement package name.

Therefore the visible Java wrapper does not prove or implement a package-name spoof back to `com.t2ksports.nba2k20and`. A separately named test package would still have its own Android package identity for ordinary Context operations. The service Context's `getObbDir()` and `getPackageName()` would normally resolve according to that installed test package.

## 3. Android storage constraints

Official Android documentation:
- `Context.getObbDir()` is the current application's OBB directory; the API reference notes its content is removed when that app is uninstalled:
  https://developer.android.com/reference/android/content/Context#getObbDir()
- Android 11+ storage documentation says apps cannot access another app's dedicated app-specific external-storage directory:
  https://developer.android.com/about/versions/11/privacy/storage#access-to-data-from-other-apps
- The all-files-access guidance says `MANAGE_EXTERNAL_STORAGE` does not grant access to app-specific directories belonging to other apps:
  https://developer.android.com/training/data-storage/manage-all-files

The supplied APK declares target SDK 28, `requestLegacyExternalStorage=true`, READ/WRITE_EXTERNAL_STORAGE, and MANAGE_EXTERNAL_STORAGE. However, those declarations are not proof that a different package can read the original package's OBB directory on the Android 15 device. Do not rely on cross-package OBB access without an actual, permitted device verification.

## 4. Signing certificate audit

The APK has an APK Signature Scheme v2 signing block. The embedded signing certificate is:
- Subject: `C=PH, ST=PANGASINAN, L=MANGATAREM, O=MODDING, OU=NBA2K, CN=JDMENU EVOLUTION`
- SHA-256 fingerprint: `65a5e8ce727fc69890199d476372f10640836f1925c752adfc555cae5669d03c`
- Validity: 2026-08-03 through 2027-08-03

This is not the standard Android debug certificate. The signing private key is not included in the APK or current project files. A modified APK signed with a different key cannot be installed as an update to an already-installed package with the original signing identity. Do not attempt to replace or uninstall the original app without an explicit, data-preserving plan and user approval.

## 5. Safe integration assessment

A separate package avoids overwriting the original app, but its default OBB folder and generated OBB filenames will differ. The stock downloader code derives both from the installed Context. Simply changing the manifest package name is therefore not enough.

Potential paths are not yet proven:
1. An authorized same-signer build, if the legitimate signing owner can provide a signed integration build. The project does not contain that signing key.
2. A separate-package test install with both original OBB files copied to the new package's own OBB directory, and the filename/path assumptions adapted to that package. This requires a verified safe transfer route for ~3.106 GiB of OBB data; no OBB files are currently available in the project.
3. A different authorized on-device instrumentation route that does not replace the installed package. No such route has yet been verified.

Do not uninstall the installed original, overwrite it, or copy/move/delete its OBBs as part of this investigation.

## 6. Next move

Before producing an APK, determine whether a safe OBB-preserving migration to a separate package is practical on the user's Android 15 device. Otherwise keep the work at the native candidate / offline analysis stage until an authorized same-signer route or another non-destructive runtime method is available.

**Status: four-library ARM64 candidate assembled; exact hook exports confirmed; package-bound OBB storage and signing remain unresolved; no APK packaged, installed, or runtime-tested.**

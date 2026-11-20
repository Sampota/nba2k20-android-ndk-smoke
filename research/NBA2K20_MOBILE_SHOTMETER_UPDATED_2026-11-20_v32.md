# NBA 2K20 Mobile Shotmeter — Project Handoff v32
Date: 2026-11-20

## User's clarified deployment goal

The user does NOT want a new NBA 2K20 Mobile game or a repackaged/renamed APK.

Target:
- Keep the original installed NBA 2K20 Mobile APK unchanged.
- Keep the original game native library unchanged.
- Keep the original OBB files unchanged and in their current working location.
- Produce exactly one new custom native library: `libshotmeter.so`.
- Load that one custom library into the original game's process so it can observe shot timing and eventually provide the second meter / GREEN GIANT feedback.

Do not continue with the separate-package APK/OBB import plan unless the user changes this requirement.

## Step 9 investigation: stock NativeActivity loading constraint

Repository audit:
- Current original APK manifest audit records NativeActivity library metadata `android.app.lib_name = nba2k20_clean_opt`.
- The audited app activity is `com.t2ksports.nba2k20and.gameactivity`.
- The original native library filename selected by the APK is therefore `libnba2k20_clean_opt.so`.
- Current project CMake does not produce the requested standalone `libshotmeter.so`; it builds `libnba2k20_clean_opt.so` (proxy candidate), `libshotmeter_probe.so`, and `libshadowhook.so`, plus a smoke-test library.

Android platform verification:
- Android's NativeActivity reads `android.app.lib_name` from Activity metadata.
- It passes that library name to the app ClassLoader's `findLibrary()`.
- It then passes the resulting path to `loadNativeCode()`.
- Official API reference: https://developer.android.com/reference/android/app/NativeActivity
- Current AOSP implementation: https://android.googlesource.com/platform/frameworks/base/+/HEAD/core/java/android/app/NativeActivity.java
- AOSP implementation evidence: the `onCreate()` method reads `META_DATA_LIB_NAME`, calls `classLoader.findLibrary(libname)`, and calls `loadNativeCode(path, ...)`.

Conclusion:
- The unchanged original APK's normal NativeActivity startup will request its declared game library, not an arbitrary additional `libshotmeter.so`.
- Merely placing `libshotmeter.so` next to the original library is not sufficient; no automatic loading is implied.
- With the original APK and original game library both unchanged, an external mechanism must arrange for the single custom library to be loaded into the running game process.
- A standalone `.so` cannot self-load into a process that never loads it.

## Feasible deployment branches

A. Rooted device / authorized process injection:
- A root-level injector or equivalent authorized mechanism can load `libshotmeter.so` into the game process.
- The one new library can contain the probe and hook logic.
- To deliver a single new `.so`, ShadowHook must either be statically incorporated/merged into that output (subject to licensing and ABI/build validation) or replaced with a self-contained hook implementation.
- Must verify target-process identity, library load success, hook installation, and callback execution before claiming runtime success.
- This path still needs an actual Android-device integration test.

B. Unrooted device, original APK untouched:
- The stock app has no identified code path that loads a separate user-supplied library.
- Without root/injection or application cooperation, there is no supported way established so far to make the unmodified NativeActivity load an additional arbitrary `.so`.
- Do not imply that copying the file into Downloads, Android/data, or beside the game library will solve this.

C. APK modification:
- Changing NativeActivity metadata or adding a loader/shim to the APK could load a custom library, but this changes the APK.
- Original signing private key is unavailable, so the modified APK cannot be installed as a valid update to the installed original package.
- A different package ID creates a separate app and introduces the OBB path issue the user explicitly wants to avoid.
- This is not the current plan.

## Important OBB clarification

The user explicitly clarified that:
- The original working OBB files are in the original game's `Android/obb` location and are essential to the original installation.
- A separate backup copy exists; an earlier backup was deleted, but the user says another copy is available.
- The original OBB files must not be moved, renamed, copied over, or modified as part of the one-library task.
- A prior probe report had reported the original package OBB path as absent while confirming a Downloads backup; this conflicts with the user's later clarification. Treat the original OBB files as protected and do not infer they are absent based only on the old probe.

## Existing native research remains useful

- ShotIndicatorComponent hooks are already implemented in the read-only probe source.
- The probe observes DoDraw, EVT_UserShotReleased, and SetMaterialEnabled.
- Timing enum 3 is a strong perfect-release candidate based on static analysis, but runtime classification still needs validation.
- IFF analysis identified shot-indicator resources, including indicator_shot_cock_bar.
- No second meter rendering or GREEN GIANT effect is implemented yet.
- Runtime hooks have not been verified inside the actual game.

## Immediate next action / user input required

Ask the user one gating question: **Is the Android 15 phone rooted?**

- If yes: research and select a root-level loader/injector path that can load one external `libshotmeter.so` into the original game process without modifying APK, original .so, or OBB. Then consolidate the probe and hooking engine into one deliverable and build an isolated integration test.
- If no: explain plainly that all three constraints (original APK untouched, no root/injection, and one new external .so) cannot be satisfied simultaneously. Do not build a misleading library artifact that has no way to enter the game process.

## Status after v32

- One-new-custom-library requirement: CONFIRMED
- Original APK / original game library / OBB preservation: REQUIRED
- NativeActivity loading mechanism: VERIFIED FROM AOSP
- Stock APK automatically loading `libshotmeter.so`: NO
- External process-loading route: NEEDS ROOT STATUS / DEVICE VERIFICATION
- One-file consolidation: NOT STARTED
- Runtime loading / hooks / in-game meter: NOT VERIFIED

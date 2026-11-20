# nba2k20-android-ndk-smoke

## Purpose

This repository verifies that a genuine Android NDK toolchain can compile a small
ARM64 Android shared library. It is a toolchain smoke test only; it is not the
NBA 2K20 game library, does not modify the APK, and is not an installable game
modification.

## Build configuration

- Android NDK: `30.0.16248370`
- ABI: `arm64-v8a`
- Android API floor: `android-21`
- CMake: `4.0.2` (required by pinned ShadowHook v2.0.1)
- Outputs: `libshotmeter_ndk_smoke.so`, `libshotmeter_probe.so`, and `libnba2k20_clean_opt.so` (proxy candidate)

The GitHub Actions workflow runs on pushes to `main` and can also be started
manually from the repository's **Actions** tab.

## Expected checks

The workflow verifies the Android NDK toolchain file and CMake installation,
builds the shared library, checks that the ELF machine is AArch64, checks for
the Android `liblog.so` dependency, confirms the exported
`shotmeter_ndk_smoke_probe` function, and uploads the library as an artifact.

A successful smoke test proves only that this isolated NDK build works. It does
not establish runtime compatibility with NBA 2K20 or authorize packaging this
library into the game.


## Read-only shotmeter capture probe

The project also builds `libshotmeter_probe.so`, a first implementation-stage module
that uses the pinned upstream ShadowHook v2.0.1 inline-hook library for ARM64.
ShadowHook is pinned by release tag in CMake rather than tracking the mutable
upstream default branch.

The probe attempts hooks against the renamed original game ELF
`libnba_clean_opt.so` using these exported symbol names:

- `_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR`
- `_Z20EVT_UserShotReleasedP12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGff`

The first hook calls the original draw function, then reads the meter component's
tracked-player pointer, live value (+0x10), visual-progress value (+0x14), state
(+0x18), release-timing enum (+0x1C), release error (+0x20), and resource pointer
(+0x28). The second hook calls the original release-event function, then records
the actor, timing enum, and two float arguments.

**This is telemetry only.** It does not write game memory, change timing, display
a second meter, classify a release as confirmed GREEN, or play the GREEN GIANT
audio. It must be loaded and its install function called only after the original
ELF has been loaded under the SONAME-compatible filename
`libnba_clean_opt.so`. The probe and ShadowHook libraries are build outputs
only; neither has been integrated into the original APK.

## Current build settings

- Android NDK: `30.0.16248370`
- ABI: `arm64-v8a`
- Android API floor: `android-21`
- CMake: `4.0.2` (installed from the pinned Python package in CI)
- ShadowHook: release tag `v2.0.1`
- New output: `libshotmeter_probe.so` and dependency `libshadowhook.so`

The workflow validates the AArch64 ELF headers, the probe's dependency on
`libshadowhook.so`, and the probe's exported C entry points. A successful
compile is not evidence that the hook works inside NBA 2K20. Runtime hook
installation and observed logs still require a separately integrated test APK
and an on-device startup/gameplay test.


## NativeActivity forwarding proxy candidate

The repository now also builds a separate ARM64 NativeActivity proxy candidate as
`libnba2k20_clean_opt.so`. It exports `ANativeActivity_onCreate` and
`JNI_OnLoad`, loads the original game ELF by its existing SONAME
(`libnba_clean_opt.so`), forwards both entry points, and calls the read-only
probe installer before forwarding NativeActivity startup.

The intended separate test APK layout is:

- `libnba2k20_clean_opt.so` — the new proxy candidate selected by the existing
  manifest library name
- `libnba_clean_opt.so` — the untouched original game library, renamed only
  to match its existing DT_SONAME
- `libshotmeter_probe.so` — read-only capture hooks
- `libshadowhook.so` — pinned hook-engine dependency

This layout has **not** been packaged into an APK or tested on a device. The
proxy candidate is not ready for users to install. The NativeActivity forwarding
path, Android linker namespace lookup, protected application startup, original
JNI initialization order, and actual in-game hook execution remain unverified.
The CI checks only confirm the ELF architecture, required entry-point exports,
DT_SONAME, and declared shared-library dependencies.


## ShadowHook shared-mode chaining correction

The probe uses ShadowHook's shared hook mode. Each proxy calls the next handler
using `SHADOWHOOK_CALL_PREV` and invokes `SHADOWHOOK_POP_STACK` before every
return. The earlier implementation called a saved raw original address directly;
that was corrected because shared mode requires ShadowHook's proxy-chain API and
per-call stack cleanup. The latest CI build (run #19) compiles and verifies the
corrected probe and proxy artifacts.


## Current Android 15 investigation

The latest engineering handoff is
[`research/NBA2K20_MOBILE_SHOTMETER_UPDATED_2026-11-20_v39.md`](research/NBA2K20_MOBILE_SHOTMETER_UPDATED_2026-11-20_v39.md).

v37 attached successfully but ShadowHook initialization returned error 12
(`Init linker mod failed`) on the Android 15/API 35 target. Source inspection
also found that ShadowHook v2.0.1 expects a separately loaded
`libshadowhook_nothing.so`, which the prior single-ELF embedding omitted.
Because the final requirement remains one delivered custom library, v38 adds
a Dobby-based single-file diagnostic candidate. Its initial test hooks only a
local function inside the agent; it does not yet hook NBA 2K20 or implement
GREEN GIANT. The v38 artifact must pass CI and an on-device self-hook test
before any game hook work resumes.


## Dobby v38 runtime validation and v39 read-only hooks

The v38 Dobby self-hook has now passed on the user's non-rooted Android 15
phone: the original trampoline returned 105 and the replacement returned 112.
This validates Dobby's basic runtime code-patching path in the attached process,
not any NBA 2K20 gameplay hook.

A separate v39 candidate now repeats that self-test and attempts three
observation-only hooks against the already-loaded game ELF. It uses
`RTLD_NOLOAD` so it will not load, replace, rename, or modify the original
game library. It records callback counts and event arguments without changing
gameplay state, timing, audio, or rendering. v39 must pass CI and then be
verified on-device before it is considered usable.

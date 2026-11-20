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
- CMake: `3.22.1`
- Output: `libshotmeter_ndk_smoke.so`

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

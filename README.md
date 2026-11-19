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

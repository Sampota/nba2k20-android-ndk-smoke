# NBA 2K20 Mobile Shotmeter — v27 Engineering Handoff
Date: 2026-11-20

This is an append-only continuation of v26. It does not replace earlier handoffs. The original APK, original IFF, and original game ELF remain untouched.

## 1. Four-library ARM64 runtime candidate assembled

The successful GitHub Actions v26 proxy artifact was downloaded and combined locally with the original game ELF copied byte-for-byte and renamed to match its existing DT_SONAME.

Archive created:
- `/mnt/data/shotmeter-runtime-candidate-arm64-v26-four-libraries.zip`
- SHA-256: `0fe9d0229cde0d4c27e2a064b825a50d9629b016e3daac67febdb706e2c1b906`
- ZIP integrity and contents were checked.
- Internal layout: `shotmeter-runtime-v26/lib/arm64-v8a/`

Four libraries:
1. `libnba2k20_clean_opt.so` — CI-built NativeActivity proxy; SHA-256 `85311663400b7f3fb740a01b9bac84c26e8cf1bd4370832db5eef7fa84f5c176`
2. `libnba_clean_opt.so` — original NBA 2K20 game library copied byte-for-byte and renamed only; SHA-256 `f0b31d2050700e3d49ba53b0c6a1ce4896137e07e77b7aeed2066ebbf1de4d09`
3. `libshotmeter_probe.so` — read-only probe v0.2.0; SHA-256 `a844e389ab29c7c59cb0fa4bcd631eaec090dc702eabf2739cdb94abebc16ba2`
4. `libshadowhook.so` — ShadowHook v2.0.1; SHA-256 `5b65932283c5f1379c604f97706e91a8fef6ea75419eb5cbfe9458d51faae4ce`

The original ELF's source and bundled hashes match exactly. Its SONAME remains `libnba_clean_opt.so`, matching the renamed bundle filename. No relinking, patching, or stripping of the original ELF was performed.

## 2. ELF and dependency inspection

All four shared libraries are ELF64 AArch64.
- Proxy SONAME: `libnba2k20_clean_opt.so`
- Probe SONAME: `libshotmeter_probe.so`
- ShadowHook SONAME: `libshadowhook.so`
- Original SONAME: `libnba_clean_opt.so`

New dependency chain:
- Proxy needs `libshotmeter_probe.so`, plus Android system libraries.
- Probe needs `libshadowhook.so`, plus Android system libraries.
- ShadowHook needs Android system libraries.
- The original game ELF retains its original DT_NEEDED list without edits.

Original game ELF dependencies:
`libGLESv3.so`, `libandroid.so`, `libEGL.so`, `libOpenSLES.so`, `liblog.so`, `libm.so`, `libz.so`, `libc++_shared.so`, `libfmod.so`, `libstdc++.so`, `libdl.so`, `libc.so`.

The prior APK audit found ARM64 `libc++_shared.so` and `libfmod.so` packaged in the original APK; `libstdc++.so` was not a packaged ARM64 APK entry. Its Android system-runtime resolution and the complete linker dependency closure still require confirmation on the target device. Do not claim runtime load success from static ELF inspection.

## 3. Hook target symbol verification

The original library's actual dynamic symbol table was materialized and searched. All three exact hook symbol names are present as GLOBAL DEFAULT FUNC exports:

- `_ZNK22ShotIndicatorComponent18SetMaterialEnabledE10string_crc5_bool` — address `0x1B3C82C`, size 88 bytes
- `_Z20EVT_UserShotReleasedP12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGff` — address `0x11EAA38`, size 456 bytes
- `_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR` — address `0x1B3B6BC`, size 3900 bytes

This confirms the exact symbol spellings expected by the probe exist in the supplied original ELF. It does not confirm successful ShadowHook relocation, runtime registration, or callback execution.

The current probe source is `src/shotmeter_probe.c`, blob SHA `e783c4610ed1aa061e0c5c3eed70105ceff1510b`. The probe calls the previous handler/original function, reads the meter/material context, logs samples, and uses shared-mode stack cleanup. It does not modify game memory, render a second meter, classify a confirmed GREEN, or play audio.

## 4. CI provenance

GitHub repository:
https://github.com/Sampota/nba2k20-android-ndk-smoke

Successful GitHub Actions run #26:
https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44670149788

Run ID: `44670149788`
Head commit: `d68ebbf713bdf3fc0526c1ed659784a119e3ff29`
Build job: `build-arm64`, conclusion `success`
Proxy artifact ID: `14599536678`

The workflow verified the Android NDK configuration, ARM64 ELF architecture, expected proxy entrypoints/SONAME, probe exports, and artifact uploads. It did not package the original library, create an APK, or perform a device test. The four-library ZIP in this handoff was assembled locally from that artifact and the original user-supplied ELF.

## 5. Remaining blockers

1. Build a separate test APK using this candidate library set, after checking NativeActivity manifest metadata and protected Application/ComponentFactory startup behavior.
2. Resolve signing and package identity. The original APK's signing key is not available in the project, so a differently signed APK cannot update the installed original under the same package identity. A separate package identity may affect private app data and OBB access. Do not ask the user to uninstall or overwrite the original game.
3. Confirm complete native dependency resolution on the target device, including the original ELF's `libstdc++.so` dependency.
4. Launch the separate test APK and inspect Logcat for proxy/original load messages and ShadowHook registration errors.
5. Verify callback execution during ordinary jumpshot, free-throw, fake-shot, and post-release feedback transitions.
6. Use real captured material-key/state transitions to complete material-role identification before implementing a second renderer.
7. Implement the independent second meter and GREEN GIANT only after runtime capture is proven; verify green-miss behavior rather than treating enum 3 alone as final user-facing confirmation.

The original app has not been repacked, signed, installed, or tested. No APK is produced by this work.

## 6. Current status and next move

**Status: IN PROGRESS — four-library ARM64 candidate assembled and statically audited; NOT READY TO TEST.**

Next move: inspect the original APK's full startup/manifest/asset-storage requirements and prepare a separate-package integration plan that preserves the installed original and its game data. Only after that plan is sound should a test APK be built.
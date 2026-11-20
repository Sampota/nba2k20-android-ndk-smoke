# NBA 2K20 Mobile Shotmeter — v26 Engineering Addendum
Date: 2026-11-20

This is a supplemental continuation of the Library handoff v25. It does not replace or rewrite earlier handoffs. The original APK, `GAMEDATA.IFF`, and original game library remain untouched.

## 1. Higher-confidence payload selection for texture hash 0xF6589499

The texture header for hash `0xF6589499` is at first decompressed-block offset `0x0290`. It declares ETC2 RGBA8 (format 16), 512×512 dimensions, and raw tag `1`.

The parser's texture resolver stores the tagged offset as `rawTag - 1`; therefore raw tag 1 yields payload-relative offset zero. It then forms candidate payload locations from decompressed resource-block bases, constrained by each block's available size and by overlap checks against known texture/header allocations. When the metadata block is known, the parser prefers a candidate in the immediately following block when that candidate survives validation.

For this resource:
- Metadata/header block: first decompressed block (index 0), offset `0x0290`
- Payload-relative offset: `0x00000000`
- ETC2 RGBA8 payload length: `0x40000` bytes
- First decompressed block length: 2,612,628 bytes
- Second decompressed block length: 32,477,264 bytes
- Best parser-model candidate: second decompressed block + `0x00000000`

The first-block candidate at offset zero would overlap the header at `0x0290`, so it is not a valid texture allocation. The next-block candidate has sufficient space and matches the parser's preferred metadata-block-plus-one selection. This upgrades the earlier offset-zero extraction from an arbitrary diagnostic guess to a **strong parser-derived candidate**.

A prior diagnostic ETC2 decode of second-block offset zero produced a coherent 512×512 basketball-net-like pattern. That visual result is consistent with the candidate but is not, by itself, proof of the original game's runtime resource resolver. The mapping is therefore recorded as parser-model-supported, not runtime-confirmed. Do not call this texture the final timing-fill artwork until the complete material/UV role is established.

## 2. Read-only material-state capture hook added

The original library exports:

- Symbol: `_ZNK22ShotIndicatorComponent18SetMaterialEnabledE10string_crc5_bool`
- Address: `0x1B3C82C`
- Size: 88 bytes

Static analysis shows this function matches a supplied 32-bit material CRC against the shot-indicator scene's 0x40-byte material entries and writes the enabled state at entry `+0x1C`.

The probe source now installs a ShadowHook shared-mode symbol hook for this function. The proxy:
1. Calls the next handler/original function through `SHADOWHOOK_CALL_PREV`.
2. Reads (does not write) the component's state `+0x18`, release enum `+0x1C`, normalized timing value `+0x10`, and progress `+0x14`.
3. Logs the supplied material CRC and enabled flag together with those component fields.
4. Calls `SHADOWHOOK_POP_STACK` on exit.

The existing `DoDraw` and `EVT_UserShotReleased` hooks remain observation-only. A material sample counter getter was added to the C header and is checked by CI. Standard `<stdbool.h>` was added for the bool argument type.

Source file: [src/shotmeter_probe.c](https://github.com/Sampota/nba2k20-android-ndk-smoke/blob/main/src/shotmeter_probe.c)

Relevant source commits:
- Hook implementation: `f4f6ad18e61f3c6d2e5757a4064a6fef4ebf5161`
- C bool include: `fb1e0c6db88a0d7cd35fda60dc87abc71c2f7d1d`
- Public sample-counter declaration: `a1b309bee18cf782710223e2fe712a6e6c671514`
- CI export verification: `d68ebbf713bdf3fc0526c1ed659784a119e3ff29`

## 3. ARM64 CI result

GitHub Actions run #26 completed successfully:

https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44670149788

The job passed:
- Android NDK installation and toolchain verification
- ARM64 CMake configuration
- Native build of the smoke library, probe, and NativeActivity proxy
- ELF64/AArch64 checks
- Probe dependency/export checks, including `shotmeter_probe_get_material_sample_count`
- Proxy SONAME and NativeActivity/JNI entry-point checks
- Artifact uploads

Artifacts:
- [NativeActivity proxy candidate ARM64](https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44670149788/artifacts/14599536678)
- [Read-only capture libraries ARM64](https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44670149788/artifacts/14599368688)

The uploaded proxy-candidate artifact contains three libraries (proxy, probe, ShadowHook). It does **not** include the original game library renamed to its SONAME, and it is not a complete four-library set or an APK.

## 4. What this does and does not establish

Established:
- The material-toggle hook source compiles for ARM64 Android.
- The new material sample counter is exported and verified by CI.
- The probe has a direct observation point for material enable/disable transitions, correlated with component state/timing.
- The `0xF6589499` payload's second-block offset-zero location is strongly supported by the parser's candidate-selection model.

Not established:
- That the hook has loaded or executed inside NBA 2K20.
- Which material keys toggle at each runtime state/timing enum.
- The exact native engine payload-resolution behavior.
- That the 512×512 pattern is the final timing-fill texture.
- The literal localized GREEN label or green-miss behavior.
- A separate second-meter renderer, GREEN GIANT playback, completed APK, installation, or in-game test.

## 5. Next move

Once the candidate libraries are integrated into a separate test APK without altering the clean originals, capture Logcat through ordinary jumpshot, free throw, release, fake-shot, and shot-feedback transitions. The `SetMaterialEnabled` log will identify the material key/enable transitions; `DoDraw` samples provide the corresponding live state and `+0x10`/ `+0x14` values. Use those observations to finish the material-role map before designing a second scene/material instance.

**Project status: IN PROGRESS — READ-ONLY MATERIAL-TOGGLE HOOK ADDED AND ARM64 CI BUILD PASSED; RUNTIME CAPTURE, MATERIAL STATE MAP, INDEPENDENT SECOND RENDERER, AND GREEN GIANT BEHAVIOR REMAIN OPEN. NOT READY TO TEST.**

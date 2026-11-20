# NBA 2K20 Mobile Shotmeter — v40 timing investigation handoff

Date: 2026-11-20

## Scope

v40 is a **read-only timing observer** built as one ARM64 Android JVMTI agent ELF with Dobby embedded. It supplements v39; it does not replace the original APK, original game ELF, OBB files, or v39 artifact.

No game process was modified during this static pass. No original binary was edited.

## Corrected static findings from the preserved original game ELF

Original ELF:
- File: `libnba2k20_clean_opt.so`
- ELF: 64-bit AArch64, Android 27
- Size: 46,350,512 bytes
- Dynamic symbol `GameData_Items`: address `0x3A4DC48`, size 3,196 bytes

### Runtime curve selector

`HUR_GetShotReleaseTiming` at `0x120AD0C` and the two timing-boundary helpers read a 32-bit integer from `GameData_Items + 0x18`.

The code converts it to float and multiplies by `0.25`. It then clamps the normalized curve coordinate at the endpoints and, for interior values, interpolates between adjacent entries in five-point curves:

- `0x376D2A0`
- `0x376D2C0`
- `0x376D2E0`
- `0x376D300`

The BSS addresses are runtime-initialized data, not fixed initialized values in this ELF file. The raw selector's gameplay meaning is not yet established; do not call it difficulty, user skill, or a specific preset without more evidence.

### Effective-rating multiplier

The classifier obtains the effective shot rating using `HUR_GetEffectiveShotRating`, then calculates:

`M = clamp((R - 0.4) / 0.49999997 + 0.5, 0.5, 1.5)`

Each of the four interpolated timing thresholds is multiplied by this rating-derived factor.

### Six-value threshold output

The exported helper `HUR_GetReleaseTimingThresholdValues` at `0x120AA10` writes six floats to the supplied output array:

- `values[0]`: negative maximum-float sentinel
- `values[1..4]`: the four rating-adjusted, interpolated timing thresholds
- `values[5]`: positive maximum-float sentinel

This output is the preferred direct source for the actual thresholds used by the current game state.

### Classifier and release event

The exported classifier is:

`_Z24HUR_GetShotReleaseTimingPK12AI_NBA_ACTORf`

at `0x120AD0C`. It receives the actor and a timing-error float, checks whether release timing is available, obtains the four threshold values, applies the effective-rating multiplier, and returns a class from 1 through 5 (or 0 when timing is unavailable).

The standard release path in `MVS_ReleaseJumper` passes the class returned by `HUR_GetShotReleaseTiming` directly to `EVT_UserShotReleased`. The stock `ShotFeedbackComponent::ShouldShow` standard-jumpshot branch also explicitly tests for class 3 after its local-controller and ordinary-jumpshot checks. This makes class 3 the relevant stock special-feedback class, but does not, by itself, prove the literal localized wording or the same behavior in every shot mode.

### Correction to the previous conversational estimate

The previously supplied fixed table of `±0.15` through `±0.09` values was **not verified** from the preserved ELF and must not be treated as the game's actual active timing window. The four curve arrays are in runtime-initialized BSS; the active values depend on that data, `GameData_Items + 0x18`, drill state, and the effective shot rating. The actual numeric values must be captured from the game's own threshold helper at runtime or recovered from the corresponding tuning data source.

Also, do not convert these values to milliseconds. The classifier operates on the game's normalized timing-error scale.

## v40 implementation

New source:
- `src/shotmeter_dobby_v40_timing_agent.cpp`

New single-file build output:
- `libshotmeter-v40.so`

The v40 observer hooks four already-exported functions using Dobby:
1. `EVT_UserShotReleased` — event enum and both float arguments
2. `HUR_GetShotReleaseTiming` — classifier input error and returned class
3. `HUR_GetReleaseTimingThresholdValues` — all six output values, including the four active adjusted thresholds
4. `HUR_GetReleaseTimingWindowBoundsForTiming` — class argument and the two returned bound values

Each proxy calls the original function exactly once and records values after the original returns. It does not read/write game-object fields, alter timing, change material state, render anything, or play audio. Dobby patches target function entries only in the running process's memory while attached; the original library file on disk is not modified.

Diagnostic:
- `files/shotmeter.dobby-v40-timing-diagnostic`
- Refreshes once per second for 180 seconds after attach.
- Reports registration codes, callback counts, classifier error/class, six threshold values, class-specific bound outputs, and release-event values.

The agent uses `RTLD_NOLOAD` to find the already-loaded game ELF. Dobby is statically embedded; there is no separate Dobby shared-library dependency.

## CI state

- v40 source file added.
- CMake target `shotmeter_dobby_v40_timing_agent` added, output `libshotmeter-v40.so`.
- Workflow updated to verify AArch64, embedded hook-engine dependencies, `Agent_OnAttach` / `Agent_OnUnload`, diagnostic marker, and all four target symbols.
- Build run #83 (CMake target included; build job) succeeded.
- Strict v40 artifact-verification run #84 completed successfully:
  https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44725415165
- Artifact ID: `14624011323`
- Artifact name: `shotmeter-dobby-v40-timing-observer-arm64`
- Extracted output: `libshotmeter-v40.so`, 801,048 bytes.
- SHA-256: `56eaa1c9ebd432458fb7b96be97d40cec3036b2af77e55833ce73a85652523bd`
- ELF64 AArch64; exported `Agent_OnAttach` / `Agent_OnUnload`; only Android system dynamic dependencies (`liblog.so`, `libdl.so`, `libm.so`, `libc.so`). No separate Dobby, ShadowHook, or C++ shared-library dependency.

## What remains

- v40 CI verification and artifact extraction/checksum are complete. No on-device attachment or gameplay test was performed.
- Do not attach or ask the user to take more shots unless they later authorize an on-device capture.
- The game-specific numeric threshold values still require runtime/tuning-data evidence; v40 is prepared to capture them if that work is resumed.
- GREEN GIANT visual/audio effects, player eligibility filtering, duplicate suppression, and final deployment are not implemented in v40.
- The one-library non-root deployment path remains subject to the existing debug/JVMTI attach constraints. v40 is a diagnostic agent, not a permanent auto-loader.

**Status: STATIC CLASSIFIER AND RATING FORMULA REINSPECTED; V40 READ-ONLY TIMING OBSERVER BUILT, STRICT CI VERIFICATION PASSED, AND ARTIFACT CHECKSUM VERIFIED. NO ON-DEVICE V40 TEST WAS PERFORMED.**

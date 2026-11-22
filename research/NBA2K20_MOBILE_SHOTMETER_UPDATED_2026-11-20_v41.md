# NBA 2K20 Mobile — v41 feedback-path and diagnostic-reset handoff

Date: 2026-11-20

## Confirmed runtime timing baseline

The v40 device captures include two independently confirmed Excellent
standard-jumpshot examples:

| Run | Release count | Last class | Last event arg0 | Live class-3 bounds |
|---|---:|---:|---:|---:|
| First fresh process | 1 | 3 | 0.0832899 | -0.1206 to +0.1206 |
| Second fresh process | 2 | 3 | 0.0174393 | -0.135 to +0.135 |

The active bounds changed between processes. v41 retains live threshold
observation and does not hardcode a universal numeric window. Class 3 remains
the best-supported Excellent/Perfect class for tested standard jumpshots.

## v41 source/output

- Source: `src/shotmeter_dobby_v41_feedback_agent.cpp`
- Expected output: `libshotmeter-v41.so`
- Single ARM64 agent ELF with Dobby statically embedded.
- Retains all four v40 timing taps.
- Adds read-only hooks for the stock feedback functions:
  - `_ZN21ShotFeedbackComponent10ShouldShowEP12AI_NBA_ACTOR`
  - `_ZN21ShotFeedbackComponent4DrawEv`
  - `_ZN21ShotFeedbackComponent6DoDrawEi`
- Each proxy forwards the original exactly once and then records a result/count.
- Uses `RTLD_NOLOAD` for the already-loaded game ELF.

## Diagnostic reset

Diagnostic path:
`files/shotmeter.dobby-v41-feedback-diagnostic`

Reset marker:
`files/shotmeter.dobby-v41-reset`

Termux command:
```sh
adb shell run-as com.t2ksports.nba2k20and touch files/shotmeter.dobby-v41-reset
```

The reporter polls once per second, consumes/removes the marker, clears
sample counters and last samples, increments `diagnostic_reset_count`, and
continues updating once per second for the game-process lifetime. Hook
registrations are not removed or reinstalled.

## Boundary and safety

v41 is a diagnostic observer, not the custom visual overlay. It does not play
audio, draw custom graphics, write original meter/component fields, alter
shot timing, or modify the original game library. The feedback hooks help
identify the stock visibility and rendering callback path before adding a
separately owned green visual resource.

## Verification

CI has not yet run for v41. Do not install or test until the ARM64 build,
dependency/export/string checks, and artifact checksum are verified. Never
attach v41 twice to the same game PID.


## v41 build and binary verification — 2026-11-20

GitHub Actions:
- Run #88: https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44740074133
- Run ID: `44740074133`
- Result: **success**; build, ELF verification, and v41 artifact upload all passed.
- Artifact ID: `14630098624`
- Artifact: `shotmeter-dobby-v41-feedback-path-arm64`
- Extracted binary size: `816072` bytes.
- Binary SHA-256: `c41b72b0bdce7a1b588266c1d4074e3333d243c00249d99d5b23f9ba0d277f45`.
- Artifact ZIP SHA-256: `0f5c031b73351ca40d8f07bd2304bdd006455dd120b418eb63bf1db1509a2088`.

Independent post-build checks:
- ELF64 AArch64 shared object; Android API 21 minimum; NDK r30.
- LOAD segments are aligned to `0x4000` (16 KB).
- Dynamic dependencies are only `liblog.so`, `libdl.so`, `libm.so`, and `libc.so`; Dobby is embedded.
- Exports `Agent_OnAttach` and `Agent_OnUnload`.
- Contains all four v40 timing symbols and all three stock ShotFeedbackComponent symbols.
- Contains the v41 diagnostic and reset marker names.

**Deployment status:** binary built and statically verified, but not yet attached to a game process. Runtime ABI/callback stability and on-device reset behavior remain to be validated. It is not a custom visual overlay.

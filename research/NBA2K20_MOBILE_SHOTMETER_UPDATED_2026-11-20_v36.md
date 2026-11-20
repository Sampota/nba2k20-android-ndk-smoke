# NBA 2K20 Mobile Shotmeter — Project Handoff v36
Date: 2026-11-20

## Active user requirements

- Original NBA 2K20 Mobile APK stays installed and unchanged.
- Original game native ELF and original OBB files stay untouched and in place.
- Exactly one custom native deliverable: `libshotmeter.so`.
- Non-rooted, phone-only deployment on the user's Infinix X669 running Android 12.
- No new/repackaged game APK, no second package, and no desktop/laptop requirement.
- Do not move, rename, overwrite, or delete original game files.

GREEN GIANT is still a later feature: detect a verified perfect release, then display a custom green visual/audio feedback effect with player filtering and deduplication. It is not implemented.

## Official Android ART/JVMTI attachment path

Android's official ART TI documentation confirms an already-running debuggable app can receive an agent with:

```sh
adb shell cmd activity attach-agent <process> /path/to/libagent.so
```

The documentation states that:
- The target application must be marked debuggable.
- ART loads the given file path using `dlopen`.
- The agent is copied to app-private storage using `run-as` as one documented delivery method.
- Android 8.0 and later support ART TI.

Official documentation: https://source.android.com/docs/core/runtime/art-ti

Android 12 AOSP ActivityManagerService source:
https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android12-release/services/core/java/com/android/server/am/ActivityManagerService.java

The Android 12 shell command does not print a success message on its normal return path. Therefore, the user's previous silent return does not establish either success or failure. AOSP does show it delegates to ActivityManagerService, which resolves the process, checks debuggability when required, and calls `IApplicationThread.attachAgent(path)`.

## Device and package evidence collected in this conversation

- Device: Infinix X669, Android 12, non-rooted.
- Package: `com.t2ksports.nba2k20and`.
- Installed package dump explicitly showed `DEBUGGABLE` in `flags` and `pkgFlags`.
- `run-as com.t2ksports.nba2k20and id` succeeds (UID 10333).
- ABIs include `arm64-v8a`.
- Game process during previous test: PID `16123`; future PID may change.
- App-private destination:
  `/data/user/0/com.t2ksports.nba2k20and/files/`

Previously installed agent:
- `libshotmeter.so`, 556,368 bytes.
- SHA-256: `f8e4c8e204e5e215f58a647f7e08f4712c7e38506def0c89b407751a50a609ad`.
- ARM64 ELF64, with `Agent_OnAttach` and `Agent_OnUnload` exports.
- This old build's callback execution was not demonstrated.

## Previous on-device attempt (inconclusive)

Command used:
```sh
adb shell cmd activity attach-agent 16123 /data/user/0/com.t2ksports.nba2k20and/files/libshotmeter.so
```

The shell returned silently. That is normal for the AOSP success path, but not proof the agent loaded.

No output was returned by:
- `adb logcat -d -t 5000 -s ShotmeterAgent:I ShotmeterProbe:I`
- Generic agent log searches.

ADB shell reading `/proc/16123/maps` returned `Permission denied`. Do not retry that check. Earlier BufferQueue logs showed the game surface rendering, but not that the agent was loaded.

The user was told it was safe to shut down the game while we revise the build.

## v36 code and CI changes

Repository: https://github.com/Sampota/nba2k20-android-ndk-smoke

Updated `src/shotmeter_agent.c`:
- Added an ELF constructor that writes `shotmeter.library-loaded` when the dynamic linker maps the library.
- Added an `Agent_OnAttach` marker: `shotmeter.agent-attached`.
- Added a post-installer marker: `shotmeter.probe-result`, containing the probe status mask.
- Marker files are app-private diagnostics under the app's `files` directory. They do not modify APK, original game ELF, OBBs, or game memory.
- The agent continues to return `JNI_OK` after hook-install errors so ART attachment and hook registration remain distinct.

Updated `.github/workflows/android-ndk-smoke.yml`:
- CI checks that all three marker strings are embedded in the ARM64 agent binary.

Commits:
- Agent source: `0183157ca0bafa6174acce930ea75b44a22fc5c2`
- CI marker checks: `2a10bb630d7b5fb5a5dde9cdaa7013844c978c08`

Workflow run triggered by the latest commit:
- Run #42, ID `44694397735`
- https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44694397735
- At the time of this handoff, toolchain setup and CMake configure completed successfully; the native build step was still running.
- Do not claim a successful revised artifact until build, ELF checks, marker-string checks, and artifact upload all complete.

## How to interpret v36 marker files

The agent should write:
1. `shotmeter.library-loaded`: ELF constructor executed; linker mapped the library.
2. `shotmeter.agent-attached`: ART invoked `Agent_OnAttach`.
3. `shotmeter.probe-result`: probe installer returned; content is `probe_status=0x...`.

Interpretation:
- No markers: no evidence of successful library load. Investigate exact path, file permissions/SELinux, and linker/ART errors.
- Only library-loaded: library mapped, but ART callback not demonstrated.
- agent-attached exists: ART callback ran.
- probe-result exists: hook installer ran. `probe_status=0x0` means all three hook registration calls returned success; it still does not prove any callback has fired during gameplay.
- Nonzero mask: compare bit flags with the ShadowHook per-symbol error logs.

## Next controlled on-device test after CI artifact is ready

1. Verify new artifact size and SHA-256.
2. Copy only the new `libshotmeter.so` into the app-private `files` folder using the already-working `adb push` and `run-as` workflow. Do not change original files.
3. Launch the original game and wait until it is running normally.
4. Attach by package process name (Android 12 help accepts process name or PID):

```sh
adb shell cmd activity attach-agent com.t2ksports.nba2k20and /data/user/0/com.t2ksports.nba2k20and/files/libshotmeter.so
```

5. Check the markers:

```sh
adb shell run-as com.t2ksports.nba2k20and ls -l files/shotmeter.library-loaded files/shotmeter.agent-attached files/shotmeter.probe-result
```

6. If present, read the probe status:

```sh
adb shell run-as com.t2ksports.nba2k20and cat files/shotmeter.probe-result
```

7. Only after ART callback execution is proven should we validate hook registrations and then test live gameplay callbacks.

## Hook research status

The current read-only probe targets:
- `_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR`
- `_Z20EVT_UserShotReleasedP12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGff`
- `_ZNK22ShotIndicatorComponent18SetMaterialEnabledE10string_crc5_bool`

ShadowHook v2.0.1 is embedded in the same agent ELF; the final single-file agent is intended to have no separate `libshadowhook.so` runtime dependency.

The probe is telemetry only. No second meter, shot timing modification, green effect, or sound is implemented. Timing enum 3 remains only a static perfect-release candidate pending runtime verification.

The original APK's NativeActivity metadata selects `nba2k20_clean_opt`; placing `libshotmeter.so` nearby does not automatically load it. The ART attach mechanism is the current non-root deployment path, subject to on-device proof.

## Status at v36

- One custom `libshotmeter.so`: REQUIRED
- Non-rooted Android 12 target: CONFIRMED
- Installed package reports `DEBUGGABLE`: CONFIRMED
- Official ART JVMTI attach route on Android 12: DOCUMENTED
- Previous attach command: SILENT RETURN; callback NOT PROVEN
- Agent/probe logcat filters: EMPTY
- Shell access to `/proc/<pid>/maps`: BLOCKED
- Independent ELF-load / callback / installer markers: ADDED
- CI marker-string checks: ADDED
- Revised ARM64 CI build: IN PROGRESS at run #42 when this note was created
- New artifact/hash: PENDING
- On-device marker verification: PENDING
- Hook registrations and in-game telemetry: NOT VERIFIED
- GREEN GIANT behavior: NOT IMPLEMENTED

This v36 handoff reflects the active Android 12 test state in this conversation and supersedes older notes where their device/OS claims conflict with the user's current phone.

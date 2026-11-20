# NBA 2K20 Mobile Shotmeter — Project Handoff v35
Date: 2026-11-20

## Non-root deployment status

The user selected the non-rooted Android 15 path. Preserve the original APK, original game ELF, and original OBB files unchanged and in place. Deliver exactly one new native library: `libshotmeter.so`.

## Breakthrough: exact APK is debuggable

The exact Library APK `NBA 2K27_98.0.2.apk` was parsed read-only. Its `<application>` has `android:debuggable=true`. Package: `com.t2ksports.nba2k20and`.

This enables Android's supported ART TI / JVMTI attach mechanism on Android 8+:
https://source.android.com/docs/core/runtime/art-ti

Android's documented command:
`adb shell cmd activity attach-agent [process] /path/to/agent/libagent.so[=options]`

ART loads the specified library into the running app process and calls `Agent_OnAttach`. ART and ActivityManager only allow this for debuggable apps. The docs recommend copying the agent into app-private data with `run-as`.

The installed app must still be confirmed to be this debuggable build. No device attach has yet been performed.

## Single-file agent implementation

Committed source and CMake:
- `src/shotmeter_agent.c`: exports `Agent_OnAttach(JavaVM*, char*, void*)` and `Agent_OnUnload(JavaVM*)`.
- `Agent_OnAttach` calls the existing read-only probe installer and logs the status. It returns `JNI_OK` so an agent-load success can be distinguished from individual hook-install failures.
- CMake compiles ShadowHook v2.0.1 source as position-independent objects and merges them into the same shared object.
- Final target output: `libshotmeter.so`.
- The final library has no `DT_NEEDED libshadowhook.so` dependency.

ShadowHook license is MIT; upstream source: https://github.com/bytedance/android-inline-hook

## CI result: successful ARM64 build

Workflow run #38:
- Run ID: `44680920538`
- URL: https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44680920538
- Commit: `cb251c241bbfb8605005d9acac853ff1aaa73ed0`
- Conclusion: **success**
- All steps completed, including the new single-file non-root JVMTI artifact upload and verification.

Artifact:
- Name: `shotmeter-nonroot-jvmti-agent-arm64`
- Artifact ID: `14603904565`
- ZIP digest: `sha256:cef951ecdd95d97e266e868328e457334a08fb982e145d265a478a0a598867f6`

The artifact was downloaded and the ELF inspected directly:
- File: `libshotmeter.so`
- Size: `556,368 bytes`
- SHA-256: `f8e4c8e204e5e215f58a647f7e08f4712c7e38506def0c89b407751a50a609ad`
- ELF64, little-endian, AArch64, shared object.
- SONAME: `libshotmeter.so`.
- Dynamic dependencies: `liblog.so`, `libdl.so`, `libm.so`, `libc.so`.
- No `libshadowhook.so` dependency.
- Dynamic exports verified: `Agent_OnAttach`, `Agent_OnUnload`, `JNI_OnLoad`, and ShadowHook public API (including `shadowhook_init`).

This confirms a valid one-file ARM64 JVMTI agent build. It does **not** prove it can attach on the user's handset or that game hooks execute.

## Planned non-root device test (not yet run)

Use ADB over USB or Wireless Debugging. No root required.

1. Confirm the installed package is debuggable:
   `adb shell run-as com.t2ksports.nba2k20and id`
   A successful `run-as` confirms the installed app grants the app-context shell for this package.
2. Push the file:
   `adb push libshotmeter.so /data/local/tmp/libshotmeter.so`
3. Copy it into the game's private files directory:
   `adb shell run-as com.t2ksports.nba2k20and mkdir -p files`
   `adb shell run-as com.t2ksports.nba2k20and cp /data/local/tmp/libshotmeter.so files/libshotmeter.so`
   `adb shell run-as com.t2ksports.nba2k20and chmod 700 files/libshotmeter.so`
4. Start the original game and wait until it reaches a state where the native game library is loaded.
5. Attach the agent:
   `adb shell cmd activity attach-agent com.t2ksports.nba2k20and /data/user/0/com.t2ksports.nba2k20and/files/libshotmeter.so`
6. Inspect logs:
   `adb logcat -s ShotmeterAgent ShotmeterProbe`

Expected logs begin with `JVMTI attach accepted by ART`. Hook-registration messages then show whether the three existing target symbols resolved.

If the process restarts, the agent must be attached again. No permanent loader or automatic startup integration is included in this one-file build.

## Current truth

- Non-root loading mechanism: **official ART JVMTI attach identified**
- Exact APK debuggable flag: **true**
- One-file ARM64 agent: **built and CI-verified**
- ShadowHook merged into same ELF: **verified by no separate ShadowHook dependency**
- Original APK / game ELF / OBB modified: **no**
- Actual installed package debuggable check: **not yet run**
- JVMTI attach on the user's Android 15 handset: **not yet tested**
- Hook installation and in-game telemetry: **not yet verified**
- Second meter / GREEN GIANT behavior: **not implemented**

Next step is a controlled non-root ADB attach test, not another APK or OBB modification.

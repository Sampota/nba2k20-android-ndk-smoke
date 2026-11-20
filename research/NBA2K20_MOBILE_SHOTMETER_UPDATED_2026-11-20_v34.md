# NBA 2K20 Mobile Shotmeter — Project Handoff v34
Date: 2026-11-20

## Non-rooted deployment breakthrough

The user chose the **non-rooted Android 15 path**. Preserve:
- Original installed APK unchanged.
- Original game ELF unchanged.
- Original OBB files unchanged and in place.
- One custom native deliverable: `libshotmeter.so`.

Do not switch to root-only injection or a separate game installation.

## Direct manifest result: debuggable is TRUE

The exact Library APK `NBA 2K27_98.0.2.apk` was materialized read-only and its binary `AndroidManifest.xml` parsed directly. The `<application>` element contains `android:debuggable=true` (typed boolean value `0xffffffff`). This is a direct observation from the exact APK, not an inference from the project notes.

Package: `com.t2ksports.nba2k20and`.
The installed app must still be confirmed to match this APK/build before relying on the flag.

## Official Android-supported loader route

Android ART Tooling Interface (ART TI) documentation:
- https://source.android.com/docs/core/runtime/art-ti
- Runtime attachment command: `adb shell cmd activity attach-agent [process] /path/to/agent/libagent.so[=options]`
- ART allows agent attachment only to apps marked debuggable, with checks in both ActivityManager and ART.
- ART uses `dlopen` for the supplied library path.
- Documentation explicitly recommends embedding the agent in the APK's library directory or using `run-as` to copy it into app data.
- ART TI is supported on Android 8.0+; Android 15 is within that range.

The exact APK's debuggable=true flag therefore provides a credible non-root route to load one native agent into the original game's running process. This is not root injection and does not require changing the APK or original game ELF. It does require ADB/shell access and the installed app build to remain debuggable.

Useful supporting docs:
- Android Studio native debugging docs: https://developer.android.com/studio/debug/
- OWASP non-root LLDB attach instructions for debuggable apps: https://mas.owasp.org/MASTG/techniques/android/MASTG-TECH-0031/
- Android 15 SELinux run-as policy permits the run-as domain to ptrace debuggable apps: https://android.googlesource.com/platform/system/sepolicy/+/refs/tags/android-15.0.0_r1/private/runas_app.te

## Protected bootstrap analysis (retained from v33)

The APK's small protected DEX loads its own shell library via `System.load(absolutePath)` from app-private `dpt-libs/lib82bdeb9e9e4f939b.so`. No general user plugin API was found. That path is no longer the primary deployment plan because ART JVMTI attachment offers a separate supported mechanism.

## Implementation committed

New source:
- `src/shotmeter_agent.c`
- Exports `Agent_OnAttach(JavaVM*, char*, void*)` and `Agent_OnUnload(JavaVM*)`.
- `Agent_OnAttach` invokes the existing read-only `shotmeter_probe_install()` and logs the result.
- It returns `JNI_OK` even if individual hooks fail, so attach success and hook success remain separately diagnosable.

CMake update:
- Adds a `shotmeter_agent` shared target with output name `libshotmeter.so`.
- Compiles ShadowHook v2.0.1 source as position-independent object files and links them into the same final agent ELF.
- Adds a version script that exports `Agent_OnAttach`, `Agent_OnUnload`, and ShadowHook's public API symbols while hiding internal symbols.
- The historical standalone probe and NativeActivity proxy targets remain for prior experiments; they are not the new single-file deliverable.

Workflow update:
- CI checks the agent ELF is AArch64.
- CI checks `Agent_OnAttach` and `Agent_OnUnload` exports.
- CI checks the agent does not have a `DT_NEEDED` dependency on `libshadowhook.so`.
- A dedicated `shotmeter-nonroot-jvmti-agent-arm64` artifact is configured.

Commits:
- Agent entry point: `035777f46e55090d511976ac4c83b4f8df639dad`
- Embedded ShadowHook build setup: `6c1951684c734c44089eb6122c00f17c02badacf`
- Correct object-library linkage: `ca5ec94fa7a00c3fb637f15c6f1f21bf254148ca`
- CI validation/artifact: `cb251c241bbfb8605005d9acac853ff1aaa73ed0`

Workflow run for the object-linkage commit:
- Run ID: `44680892091`
- URL: https://github.com/Sampota/nba2k20-android-ndk-smoke/actions/runs/44680892091
- At handoff creation it was still in progress at CMake configure; no build success is claimed.

## Next steps

1. Wait for CI; fix compile/link/version-script errors if any.
2. Inspect built `libshotmeter.so`: AArch64 ELF, SONAME, exported `Agent_OnAttach`, expected log/dl dependencies, and no `libshadowhook.so` dependency.
3. Build a minimal non-destructive ADB install/attach test plan. Suggested documented flow after build:
   - push the agent to `/data/local/tmp/`;
   - use `run-as com.t2ksports.nba2k20and` to copy it into that app's private `files/` directory;
   - launch the original game and wait until its native library is loaded;
   - invoke `cmd activity attach-agent` against the game process and absolute private agent path;
   - inspect logcat for `ShotmeterAgent` and `ShotmeterProbe` messages.
4. Confirm actual installed package reports debuggable and supports `run-as`. Do not assume the installed build matches the archived APK until verified.
5. Runtime JVMTI attach and hook execution remain untested. Hook success is not established by CI alone.
6. If the game process restarts, the agent must be attached again unless a separate approved launcher/automation is later built.

## Status

- Non-rooted route: **credible, Android-supported JVMTI attach path identified**
- Original APK / original ELF / OBB preservation: **maintained in this work**
- Exact APK debuggable flag: **true**
- `Agent_OnAttach` source: **committed**
- Single-file ShadowHook embedding: **implemented in CMake, awaiting CI validation**
- Final `libshotmeter.so` build: **not yet verified**
- Actual device attach: **not yet tested**
- In-game hook execution: **not yet verified**

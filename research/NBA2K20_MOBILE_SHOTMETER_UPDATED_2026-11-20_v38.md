# NBA 2K20 Mobile Shotmeter — v38 engineering handoff

Date: 2026-11-20

## Current target and device facts

- Package: `com.t2ksports.nba2k20and`
- Target ABI: `arm64-v8a`
- Target phone: non-rooted Android 15, API 35 (confirmed by device shell output in this investigation).
- The app package is debuggable and the JVMTI attach path has already loaded the external diagnostic agent in the running game process.
- The app-private staging path is `/data/user/0/com.t2ksports.nba2k20and/files/libshotmeter.so`.
- Original APK, original game native library, and OBB files remain untouched.
- The current v37 diagnostic was staged and attached successfully. Its checksum was verified before attachment.

## v37 result — confirmed failure

The v37 marker contained:

```text
probe_status=0x8
shadowhook_init_code=12
shadowhook_init_message=Init linker mod failed
```

In the v37 source, bit `0x8` is `SHOTMETER_PROBE_INIT_FAILED`. ShadowHook initialization failed before the three read-only game hook registrations were attempted. No shotmeter gameplay hook has been verified, and GREEN GIANT is not active.

The upstream ShadowHook source maps error 12 to `SHADOWHOOK_ERRNO_INIT_LINKER`. Its initialization calls `sh_linker_init()`, which first uses XDL to resolve the linker internals `__dl__ZN6soinfo17call_constructorsEv` and `__dl__ZN6soinfo16call_destructorsEv`, then installs linker hooks. Error 12 can therefore represent a failure in linker-symbol resolution or a later part of that linker-hook setup; v37's public error code alone does not distinguish the exact sub-step.

## Android 15 linker string check

The device command:

```sh
adb shell grep -a -o 'call_[A-Za-z_]*' /apex/com.android.runtime/bin/linker64
```

returned:

```text
call_ref
call_ifunc_resolvery
```

This check did not find the two exact constructor/destructor symbol names ShadowHook v2.0.1 requests. It is evidence that those name strings were not found by this scan, not a complete proof that no equivalent linker functions exist under alternate names or can be found through another symbol source.

Upstream issue #113 describes the same ShadowHook error 12 on Android 15/arm64. Its reporter initially found the expected names with `strings` but XDL still failed to resolve them; a later comment reported success after adding the companion `libshadowhook_nothing.so`. Treat that report as a useful reproduction lead, not proof that the companion alone explains this device's result.

## Important v37 packaging discovery

The v37 CMake configuration embedded ShadowHook's main C sources into `libshotmeter.so`, but it did not provide the separate `libshadowhook_nothing.so` ELF required by ShadowHook's own linker implementation.

Inspection of the pinned ShadowHook v2.0.1 source shows:

- Upstream CMake builds a separate `shadowhook_nothing` shared target from `nothing/sh_nothing.c`.
- `sh_linker.c` explicitly opens `libshadowhook_nothing.so` while setting up its linker constructor/destructor monitoring and uses that ELF as a known object when scanning Android's internal `soinfo` layout.
- Compiling the tiny `sh_nothing.c` source into the main agent would not satisfy that lookup: ShadowHook calls `dlopen("libshadowhook_nothing.so", ...)` and expects a separately loaded ELF with that name.

Therefore, the v37 “main ShadowHook sources embedded into one ELF” arrangement is not equivalent to upstream's documented two-ELF integration. This is a confirmed packaging omission in our build, although it is not yet isolated as the exact first failure reached on this phone.

Because the project requirement is one delivered custom `libshotmeter.so`, the v38 diagnostic does not add a separate ShadowHook companion library.

## v38 direction — Dobby diagnostic candidate

A separate v38 diagnostic candidate has been added to the repository:

- Hook engine: Dobby, pinned to immutable commit `5dfc8546954ce3b3198132ab13fddb89ee92cdd7`.
- Intended packaging: compile Dobby's static target into one ARM64 JVMTI agent ELF; do not ship a separate `libdobby.so`.
- The v38 agent intentionally does **not** hook NBA 2K20 functions yet.
- Its only test is an isolated self-hook on a local, no-inline function inside the agent. Before hooking, input 5 should return 105. The replacement calls the original trampoline and adds 7, so the patched call should return 112.
- The agent writes `files/shotmeter.dobby-diagnostic` with the baseline, Dobby return code, trampoline result, patched result, and PASS/FAIL status.
- The workflow checks that the new ELF is AArch64, exports `Agent_OnAttach`/ `Agent_OnUnload`, and has no separate Dobby, ShadowHook, or shared C++ runtime dependency.

This is only a build and engine-feasibility test. Dobby's Android/ARM64 support in its upstream repository does not establish compatibility with this exact Android 15 device. Its self-hook must pass CI and then pass on-device before it is used against game functions.

## Current build/test status

- v37: built, transferred, checksum verified, attached; ShadowHook init failed with error 12.
- v38 source/CMake/workflow: added to the repository for CI evaluation.
- v38 build artifact: not yet verified.
- v38 on-device test: not yet run.
- NBA 2K20 hook registration: not attempted with Dobby.
- GREEN GIANT visual/audio effects: not implemented.

Do not replace the current app-private `files/libshotmeter.so` until a v38 artifact is available, inspected, and explicitly ready for the phone test. The existing v37 library and backup may be left in place for reference.

## Next steps

1. Confirm the GitHub Actions build compiles the pinned Dobby static target and emits `libshotmeter-v38.so`.
2. Inspect the artifact's ELF architecture, dynamic dependencies, exported JVMTI entry points, and SHA-256.
3. Only after those checks pass, provide the user a safe staging/attach test for the new diagnostic candidate.
4. If the on-device self-hook reports PASS, resolve the three already-audited NBA 2K20 symbols and add read-only capture hooks using Dobby's original-function trampolines.
5. Verify actual in-game callbacks before designing the GREEN GIANT trigger or modifying any rendering/audio state.

## User constraints and safety boundary

- Phone-only workflow; no desktop/laptop required from the user.
- Non-rooted deployment path only.
- Preserve the original APK, original game `.so`, and OBB files.
- Final deliverable remains one custom `libshotmeter.so`; do not require the user to install multiple hook-engine libraries.
- JVMTI attach works here because this particular installed package is debuggable. This is not a permanent injection method for an ordinary non-debuggable production APK.
- Never describe hooks or GREEN GIANT as working until their execution is verified in the running game.


## v38 CI iteration log

The first Dobby CI attempt compiled through configure and then failed because the pinned upstream ARM64 bridge assembly used Mach-O `@PAGE`/`@PAGEOFF` relocations. A follow-up rewrite using explicit `:pg_hi21:` was also rejected by the Android NDK assembler. The current CMake patch now uses an ELF ADRP symbol reference and an ELF `:lo12:` addend instead; this has not yet been confirmed by a successful build.

The next CI attempt then passed the earlier assembly stage but exposed a source/header mismatch in the pinned Dobby revision: Linux/Android `ProcessRuntime.cc` accesses `RuntimeModule::load_address`, while the shared structure declared only `base`. The current CMake patch changes that structure to an anonymous union exposing both names. This is a compatibility shim in the fetched Dobby source only; it does not touch the user's phone or game files.

The latest workflow run is checking that compatibility patch. A successful compile and artifact verification are still required; no v38 binary has been released for device testing.


A subsequent CI pass cleared the earlier `OSMemory` include-cycle and `MemRange::start()` issues after the narrow CMake compatibility edits. It then stopped at Dobby's POSIX code-patching file because the upstream target did not expose its own `source/` include root to that translation unit. The latest CMake change adds this source include path to both Dobby targets. The new workflow run must still confirm whether this gets through the remaining compile and link stages.

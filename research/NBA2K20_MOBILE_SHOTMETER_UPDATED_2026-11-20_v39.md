# NBA 2K20 Mobile Shotmeter — v39 engineering handoff

Date: 2026-11-20

## Confirmed on-device result: v38 Dobby self-hook

The user attached `libshotmeter-v38.so` to the running debuggable game process on the non-rooted Android 15 / API 35 phone. The diagnostic file reported:

```text
engine=Dobby
engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7
baseline_before=105
hook_install_code=0
original_trampoline_after=105
patched_call_after=112
expected_patched_call=112
status=PASS
game_hooks_attempted=0
```

This confirms that Dobby installed a self-hook in the attached process, that its original trampoline returned 105, and that the replacement returned 112. It does **not** prove that any NBA 2K20 function has been hooked.

The v38 artifact and its staged copy remain unchanged. The v38 SHA-256 is:

`f510fce0c0c32a5be1413d98975ea17058d452e7aa6026c45833f92d84a68b62`

## v39 goal

v39 is the first Dobby candidate that attempts the three already-audited NBA 2K20 symbols. It keeps the v38 self-test and then attempts these game hooks:

- `_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR`
- `_Z20EVT_UserShotReleasedP12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGff`
- `_ZNK22ShotIndicatorComponent18SetMaterialEnabledE10string_crc5_bool`

Static analysis previously confirmed these names as exported functions in the original game ELF. That does not guarantee the symbols resolve in this running process, that Dobby can patch them, or that callbacks will execute safely. Those are the v39 runtime questions.

## v39 implementation

New source: `src/shotmeter_dobby_v39_agent.cpp`

New build target: `shotmeter_dobby_v39_agent`, output filename `libshotmeter-v39.so`.

The agent:

1. Repeats the proven Dobby self-hook (expected 105 / 105 / 112).
2. Searches only for an ELF that is already loaded, using `dlopen(..., RTLD_NOLOAD)` with the known SONAME `libnba_clean_opt.so` and then the APK filename `libnba2k20_clean_opt.so`.
3. Uses `dlsym` against that handle for the three exact mangled symbols.
4. Installs the less frequent shot-release and material hooks before the per-draw hook.
5. Calls each original trampoline from its proxy.
6. Records callback counts, shot-release timing enum and two float arguments, and material CRC / enabled value.
7. Refreshes `files/shotmeter.dobby-v39-diagnostic` once per second for up to 180 seconds.

The proxies do not inspect or write game-object fields and do not change shot timing, material state, rendering, audio, or gameplay. Dobby necessarily patches the three target function entry points in the running process's memory so calls can pass through these proxies; this is transient in-memory instrumentation. The original game ELF file on disk is not modified. Callback counts in the diagnostic distinguish successful symbol resolution/registration from actual in-game execution.

The `RTLD_NOLOAD` requirement is deliberate: the agent must not load the protected original game library itself, replace it, rename it, or modify the file on disk.

## Build and device-test state

- v38: CI passed; artifact checksum verified; Dobby self-hook passed on the user's phone.
- v39 source, CMake target, and workflow artifact checks have been committed.
- v39 CI build/artifact: not yet confirmed. Do not download or attach v39 until a successful workflow run and artifact verification are recorded.
- v39 gameplay callbacks: not yet tested.
- GREEN GIANT visual/audio effect: not implemented.

After v39 CI succeeds, download the artifact and verify its checksum. Transfer it to the phone, stage it as the distinct path `files/libshotmeter-v39.so`, and attach using that exact path. Never overwrite the v37 `files/libshotmeter.so`, the v38 file, or any backup.

Read the resulting diagnostic with:

```sh
adb shell run-as com.t2ksports.nba2k20and cat files/shotmeter.dobby-v39-diagnostic
```

A `HOOKS_REGISTERED_WAITING_FOR_CALLBACKS` status means registrations succeeded but no callback was observed yet. `CALLBACKS_OBSERVED` with one or more nonzero callback counters is the evidence of actual invocation. Any failure code must be diagnosed before proceeding.

## Device and user constraints

- Target package: `com.t2ksports.nba2k20and`
- Target ABI: `arm64-v8a`
- Target phone: non-rooted Android 15 / API 35.
- JVMTI attach is possible because this installed package is debuggable; this is not a permanent injection method for a normal non-debuggable release app.
- Phone-only user workflow; no desktop/laptop.
- Final deployment goal remains one custom library, `libshotmeter.so`; Dobby is statically embedded.
- Preserve the original APK, original game native library, and OBB files. Do not create/repackage a replacement game APK.
- Never claim gameplay hooks or GREEN GIANT work until their behavior is demonstrated in-game.

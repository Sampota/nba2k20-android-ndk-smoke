# NBA 2K20 Mobile Shotmeter — v37 diagnostic handoff

Date: 2026-11-20

## Current device state

- Target package: `com.t2ksports.nba2k20and`
- Device: non-rooted Android 12
- v36 single-file JVMTI agent was staged as `files/libshotmeter.so`.
- v36 SHA-256: `a44f40a18e7f399515cfaa744d0b76a869231d6efb969477458aac0f48e66947`
- ART attach markers confirmed: `shotmeter.library-loaded`, `shotmeter.agent-attached`, and `shotmeter.probe-result`.
- Probe result read from device: `probe_status=0x8`.

## Interpretation

In `src/shotmeter_probe.c`, status bit `0x8` is `SHOTMETER_PROBE_INIT_FAILED`, set when `shadowhook_init(SHADOWHOOK_MODE_SHARED, false)` returns a non-zero error. Hook registration is not reached when this initialization fails. No gameplay callbacks have been verified.

Logcat searches did not reveal the ShadowHook error line, so the exact error code/message remains unknown.

## v37 diagnostic source changes pushed

The v37 source changes add:

- `shotmeter_probe_get_init_error_code()`
- `shotmeter_probe_get_init_error_message()`
- A new app-private marker file: `shotmeter.init-diagnostic`
- The marker records the overall probe status, ShadowHook initialization return code, and ShadowHook error message.
- A successful initialization is recorded as `success`, avoiding ambiguous default text.
- CI checks verify the diagnostic marker string is embedded in the agent.

The changes are on `main`. Latest workflow-file commit: `4ca3da1b9e4bfc5c01258add893947b340069480`.

## Build and test status

- v37 source patch: pushed.
- v37 CI build: not yet verified in this handoff.
- v37 artifact: not yet available/verified.
- Do not install v37 until CI succeeds and the artifact hash is checked.
- Keep v36 installed until then.
- Do not modify the original APK, original game native library, or OBB files.

## Next steps

1. Verify the GitHub Actions run triggered by the latest push.
2. If CI passes, download the single-file ARM64 JVMTI agent artifact.
3. Verify its SHA-256 and ELF architecture.
4. Transfer it to the staging path and replace only the app-private `files/libshotmeter.so` after the user is ready.
5. Attach it to the running debuggable game process and read `files/shotmeter.init-diagnostic`.
6. Use the exact ShadowHook code/message to decide the next fix.

## Scope

This remains a single custom `libshotmeter.so` project for a non-rooted phone. The probe is read-only telemetry. GREEN GIANT effects (custom visual feedback and optional sound) are not implemented.

# NBA 2K20 Mobile Shotmeter — v63 engineering handoff

Date: 2026-11-23

## Baseline

- Parent: `v62-claude-flame-hand-clear-meter`
- v63 branch: `v63-diagnostic-coordinate-gl-fix`
- Source: `src/shotmeter_dobby_v63_coordinate_gl_fix.cpp`
- Output: `libshotmeter-v63.so`
- Hook engine: Dobby, pinned commit `5dfc8546954ce3b3198132ab13fddb89ee92cdd7`

## Changes from v62

### Meter placement

The meter's horizontal shift is now expressed once as
`kMeterShiftDesignX = 90.0f` and reused by the feedback-banner position
calculation. This replaces the prior 38-design-unit shift and keeps the
banner's 8-design-unit gap calculated from the same meter origin. The
on-device result is not assumed: verify hand clearance and ensure the meter
and banner remain within the visible viewport.

### Flame alignment

The flame's x-axis now equals the projected head's screen x-coordinate.
The previous `+37.5f * scale` offset is removed. The existing small vertical
offset above the projected head and the 70%-sized flame box are retained.
Actual visual centering must be checked in gameplay.

### GL error attribution

Stage 170 samples the GL error queue immediately before overlay rendering.
That sample is now stored and logged separately as a pre-existing game-side
GL error; it no longer increments the overlay/init GL error count. This
correctly classifies the previously seen `GL_INVALID_ENUM (0x500)` at stage
170 as an error already pending before the overlay pass, not proof that the
overlay generated it. It does not repair the game's own GL error.

Errors from the overlay's shader, draw, and state-restore checkpoints remain
tracked through the normal overlay error counters.

### Diagnostic writer

The diagnostic now writes to a sibling temporary file and:
- retries interrupted writes;
- handles partial writes until the full buffer is persisted;
- checks `fsync` and `close`;
- atomically renames the completed file to the final diagnostic path;
- logs failures and removes an incomplete temporary file.

The report also includes the latest meter origin and flame axis/base coordinates.
Final path:
`files/shotmeter.dobby-v63-coordinate-gl-diagnostic`.

## Preserved behavior

- Dobby self-hook validation and game symbol resolution.
- Original stock meter component reads and original draw call forwarding.
- Existing shot-release timing classification and perfect-shot behavior.
- Existing grade-aware probability hook behavior.
- Green Giant audio and the v62 flame/banner visual design.
- Feedback banner placement relative to the meter.
- Existing GL state save/restore logic.

No speculative broad deletion was made. Code cleanup is conservative so that
existing hook callbacks, shader uniforms, audio state, and diagnostics are not
removed without reference and build evidence.

## CI verification

The workflow now builds the v63 target, checks the AArch64 ELF header, checks
for the v63 diagnostic and positioning markers, and uploads
`shotmeter-dobby-v63-coordinate-gl-diagnostic-arm64`.

A green workflow run establishes successful compilation and artifact checks
only. It does not establish runtime hook success, visual hand clearance,
camera-setting independence, flame alignment, or absence of game-side GL
errors.

## On-device validation checklist

1. Install only the v63 artifact after confirming the CI run succeeded and
   record its SHA-256.
2. Attach it to the running game using the same working JVMTI attach flow.
3. Confirm the v63 attach/self-test and expected game symbols in logcat.
4. Confirm the diagnostic file is non-empty and contains fresh timestamps/
   counters and finite meter/flame coordinates.
5. Test several camera positions and at least one active shooting sequence.
6. Confirm the meter clears the shooting hand and remains visible; confirm the
   flame is centered above the player's head.
7. Confirm the banner still sits to the meter's right with its top edge aligned.
8. Compare `gl_pre_overlay_error_count` / `gl_pre_overlay_error_code` with
   `gl_error_count` and `gl_draw_error_count`. A pre-overlay error is not an
   overlay regression; any new overlay error needs separate investigation.
9. Watch for crashes, missed callbacks, visual artifacts, and audio regression.

Do not treat v63 as runtime-validated until these checks are completed on the
device.
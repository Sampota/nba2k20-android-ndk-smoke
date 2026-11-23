# NBA 2K20 Mobile Shotmeter — v64 engineering handoff

Date: 2026-11-23

## Baseline

- Parent: `v63-diagnostic-coordinate-gl-fix`
- Branch: `v64-halo-flame-tight-feedback`
- Source: `src/shotmeter_dobby_v64_halo_flame_feedback.cpp`
- Output: `libshotmeter-v64.so`
- Hook engine: Dobby, pinned commit `5dfc8546954ce3b3198132ab13fddb89ee92cdd7`

## Changes from v63

- Feedback panel gap reduced from 8.0 to 4.0 design units. The panel remains aligned to the meter's top edge and uses the same meter-right calculation.
- Green flame remains horizontally centered on the projected player head. Its vertical base offset changes from 8.0 to 48.0 design units in the positive GL-Y direction, lifting the flame approximately 20 screen pixels at 720p scale so it should hover above the head like a halo.
- Meter horizontal shift remains 90.0 design units. No meter-placement changes were made.
- Shot timing, release classification, probability mapping, audio, GL state handling, and diagnostic writer are carried forward unchanged.
- **New planned investigation from the v63 live-shot diagnostic:** `last_grade_distance=nan` and `last_grade_index=-1` were observed even though `probability_modified_shots=1` and the probability hook applied a launch chance. Treat this as an unresolved grade-distance/index calculation or input-validity issue, not as a proven root cause.
- Trace the grade-distance calculation inputs and intermediate values for a real release; identify the exact source of the non-finite value (including any invalid vector, denominator, or uninitialized context if applicable). Add finite/range guards before using the distance or grade index, and make invalid-grade handling explicit in diagnostics.
- Add a focused self-test covering valid grade distances and invalid/non-finite inputs. Do not silently substitute a grade or change the chance mapping to hide the failure. Preserve the existing mapping unless investigation proves a narrowly scoped correction is required.
- Keep the probability feature marked as not fully validated until a live shot reports a finite grade distance and valid grade index, with the applied chance consistent with the intended mapping.

## Validation status

The source and CI target are prepared for ARM64 compilation and artifact checks. No v64 CI success, artifact hash, or on-device visual result is claimed until the workflow completes and the new artifact is tested on the phone.

On-device checks:
1. Confirm v64 attach and shader initialization.
2. Capture an Excellent shot and inspect the banner-to-meter gap.
3. Confirm the green flame sits above the hairline without covering the head.
4. Confirm meter placement remains unchanged and no overlay GL draw errors appear.
5. Compare diagnostic flame base and meter coordinates with the v63 baseline.
6. Capture the grade-distance inputs/intermediates for a live shot; verify finite distance, valid grade index, and correct chance mapping. Confirm invalid inputs are safely rejected and diagnosed.

Keep v63 and v62 artifacts untouched as rollback options.

## Additional investigation: pre-overlay GL_INVALID_ENUM (1280)

The v63 live diagnostic reported a pre-overlay OpenGL error code of 1280
(`GL_INVALID_ENUM`). This is evidence that an error was pending when the
overlay sampled the GL error queue; it is **not** evidence that v63 caused it.
The current v64 code samples this queue in `swap_proxy` at stage 170 before
calling `draw_overlay`, and `glGetError()` consumes the sampled error.
The overlay's own shader initialization and draw checkpoints are tracked
separately. Do not relabel the pre-overlay error as an overlay failure.

Investigation plan (diagnostic-first; no speculative game-state or GL changes):

1. Preserve separate counters and codes for errors found before overlay work,
   overlay initialization, and overlay draw/restore checkpoints. Record the
   EGL context identity and frame/sample sequence alongside each observation.
2. Add a controlled A/B capture: same game scene and actions with the overlay
   detached/disabled, then with v64 attached. Compare frequency and timing.
   If 1280 occurs with the overlay absent, that establishes an independent
   game-side source for that test; if it occurs only with v64, investigate
   overlay interaction without assuming causation from timing alone.
3. Check whether `GL_KHR_debug` (or a compatible debug-output extension) is
   advertised by the active GLES context. If available, register a diagnostic
   callback and capture source/type/id/severity/message plus frame/context.
   Do not assume the extension exists on this device.
4. If debug output is unavailable, use a separate opt-in diagnostic build to
   narrow the suspect game GL call sequence. Do not blindly poll
   `glGetError()` after every intercepted game call: that consumes the
   application's error state and changes behavior. Any call-level probe must
   document this observer effect and be isolated from the normal v64 build.
5. Keep the overlay's own GL checkpoints independent. Require zero overlay
   draw/restore errors and successful shader/program status; report pre-overlay
   game errors separately even if they persist.
6. Do not suppress, clear, or "fix" the 1280 by inserting arbitrary GL calls,
   changing game render state, or masking the diagnostic. First identify the
   invalid enum and the exact call/parameter responsible; only then make a
   narrowly scoped correction if it is in our code. If it is in the game,
   leave game code untouched and document the finding.

Acceptance criteria: the diagnostic distinguishes pre-overlay errors from
overlay-owned errors; A/B evidence identifies whether the error reproduces
without the overlay; and, where supported, debug output identifies the
offending API/message. Until then, classify 1280 as an unresolved
pre-overlay/game-side GL error, not a v64 regression.


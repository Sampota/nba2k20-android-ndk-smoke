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
- **Investigation finding:** the v63 sample that showed `last_grade_distance=nan` and `last_grade_index=-1` was an Excellent release (timing class 3). The current probability logic intentionally calculates a grade only for Slight Early/Late (classes 2 and 4); Very Early, Excellent, and Very Late (classes 1, 3, and 5) do not need a grade. Therefore, the class-3 NaN/-1 pair is the unused sentinel state, not proof of a failed distance calculation.
- v64 now reports `last_grade_status` explicitly. A class-3 release should report `NOT_REQUIRED_FOR_TIMING_CLASS`; classes 2/4 must report a validated grade or a specific failure reason. The diagnostic also records player-location XYZ, basket distance, grade index, and timing class.
- v64 validates location coordinates, finite/non-negative basket distance, and grade index range 0..12 before applying a grade-aware chance. If a class-2/4 input is invalid, the original launch chance is passed through unchanged; no substitute grade is invented.
- A separate 10-case grade-validation self-test covers valid/invalid distances (including NaN and infinities) and grade-index bounds. The existing probability mapping remains unchanged; both self-tests must pass before the probability hook is enabled.
- Runtime probability validation remains pending until on-device Slight Early and Slight Late shots show finite location/distance, a valid grade index, and a chance matching the existing mapping. Excellent shots should be checked separately and are expected not to require a grade.

## Validation status

The source and CI target are prepared for ARM64 compilation and artifact checks. No v64 CI success, artifact hash, or on-device visual result is claimed until the workflow completes and the new artifact is tested on the phone.

On-device checks:
1. Confirm v64 attach and shader initialization.
2. Capture an Excellent shot and inspect the banner-to-meter gap.
3. Confirm the green flame sits above the hairline without covering the head.
4. Confirm meter placement remains unchanged and no overlay GL draw errors appear.
5. Compare diagnostic flame base and meter coordinates with the v63 baseline.
6. Capture Excellent, Slight Early, and Slight Late shots separately. Excellent should report `NOT_REQUIRED_FOR_TIMING_CLASS`; classes 2/4 should report finite XYZ and basket distance plus grade index 0..12. Confirm the mapped chance matches the existing table and invalid inputs preserve the original chance with an explicit failure status.

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
   overlay initialization, and overlay draw/restore checkpoints. v64 records
   the pre-overlay error sequence, swap-frame number, EGL context, display,
   and surface when an error is observed.
2. Add a controlled A/B capture: same game scene and actions with the overlay
   detached/disabled, then with v64 attached. Compare frequency and timing.
   The existing `glGetError()` sample is consumed by the observer, so document
   that observer effect and do not treat the sample as a non-invasive trace.
3. v64 checks for `GL_KHR_debug` using the GLES 3 extension list and records
   whether `glDebugMessageCallbackKHR` is available through
   `eglGetProcAddress`. The normal v64 agent does **not** register a callback:
   doing so could replace a callback already owned by the game. Any callback
   capture belongs in a separate opt-in diagnostic build with callback-state
   ownership handled explicitly.
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



## v64 preparation implementation status

- Grade-status diagnostics and the 10-case finite-distance/index validation self-test are implemented in the v64 source.
- GL diagnostics now record pre-overlay error frame/sequence/context/display/surface and identify common GL error enums by name.
- GLES 3 extension detection records `GL_KHR_debug` availability and callback entry-point availability. The regular agent leaves callback registration disabled to avoid taking over game-owned callback state.
- CI must verify the v64 entry points and these diagnostic markers before the ARM64 artifact is accepted.

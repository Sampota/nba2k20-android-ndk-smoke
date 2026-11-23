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

## Validation status

The source and CI target are prepared for ARM64 compilation and artifact checks. No v64 CI success, artifact hash, or on-device visual result is claimed until the workflow completes and the new artifact is tested on the phone.

On-device checks:
1. Confirm v64 attach and shader initialization.
2. Capture an Excellent shot and inspect the banner-to-meter gap.
3. Confirm the green flame sits above the hairline without covering the head.
4. Confirm meter placement remains unchanged and no overlay GL draw errors appear.
5. Compare diagnostic flame base and meter coordinates with the v63 baseline.

Keep v63 and v62 artifacts untouched as rollback options.

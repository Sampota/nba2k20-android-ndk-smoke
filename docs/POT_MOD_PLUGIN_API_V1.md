# POT MOD / shotmeter plugin API v1

## Ownership boundary
- Main game library owns native menu navigation and the single POT MOD row: `SHOTMETER < OFF >`.
- `libshotmeter.so` owns meter rendering, audio/green feedback, customization data, and the separate customization interface.
- The main library must not depend on individual design IDs or internal rendering code.
- The main ELF uses one loader dependency change to bring in the shotmeter plugin. The plugin currently registers a native POT MOD row/page at runtime in the game's native Options menu descriptor; the main game still performs the native page navigation, drawing, and toggle presentation.

## Loading contract
The v71 dependency constructor starts the shotmeter bootstrap when Android loads `libshotmeter.so` through the game's ELF dependency chain. The main library can resolve the exported API by `dlsym` after verifying the loaded module and ABI version.

## API v1
Header: `include/shotmeter_plugin_api.h`

Exports:
- `shotmeter_plugin_get_api_version`
- `shotmeter_plugin_get_capabilities`
- `shotmeter_plugin_set_enabled`
- `shotmeter_plugin_get_enabled`
- `shotmeter_plugin_open_customization`
- `shotmeter_plugin_close_customization`
- `shotmeter_plugin_is_customization_open`
- `shotmeter_plugin_set_selection`
- `shotmeter_plugin_get_selection`
- `shotmeter_plugin_flush_settings`
- `shotmeter_plugin_get_init_state`

Selection categories are DESIGN, AUDIO, and GREEN_ANIMATION. Selection IDs are opaque 0..255 values to the main library. The plugin persists enable state and selection IDs in its app-private settings file.

## Current behavior in this incremental build
- Default state is OFF.
- OFF suppresses plugin-owned meter drawing, result feedback, sound requests, and probability remapping. The original game release handler is still forwarded unchanged.
- ON restores existing v71 plugin behavior.
- The plugin has an initial GLES2/GLES3 customization-screen scaffold with DESIGN, AUDIO, and GREEN ANIMATION tabs, a HORN METER preview, Green Giant audio page, and Claude Flame preview.
- The plugin build now includes a native-menu registration routine that copies the eight current OPTIONS records into a new nine-record array and appends POT MOD as a native page-opening row, using a fresh native toggle-interface object.
- The UI is not yet a complete catalogue: each tab currently shows a single prototype item (selection ID 0). A multi-design/multi-audio/multi-animation list and per-design preview registry remain to be implemented.
- Touch input is intercepted only while customization is open. On this game build, PushEvent emits ACTION_DOWN as type 0, POINTER_DOWN as type 1, MOVE as type 2, CANCEL as type 3; ACTION_UP is not emitted through this queue. The UI activates items on type 0 and forwards touch events to the game whenever the UI is closed.
- The custom UI is a work-in-progress plugin screen, not yet runtime-tested on-device.

## Compatibility and release requirements
- Keep the C ABI versioned. Add future selection categories without changing existing function signatures.
- New meter designs and visual/audio assets belong in the shotmeter plugin, not the main game library.
- The main-menu plugin integration should verify the API version and capability flags before calling exported functions.
- The current native page labels use CRC values and must be verified against the installed game's localized string resources; the external OBB/localization database has not been device-tested.
- Never package a host-only ELF or an unverified build. Validate ARM64 ELF type, SONAME, exported symbols, dependencies, and the untouched clean game-library hash before producing a test APK.

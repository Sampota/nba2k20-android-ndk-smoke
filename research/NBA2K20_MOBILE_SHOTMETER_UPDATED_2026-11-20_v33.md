# NBA 2K20 Mobile Shotmeter — Project Handoff v33
Date: 2026-11-20

## User's deployment decision

The user explicitly chose the **non-rooted Android 15 route**. Do not switch to root-only injection, do not request root again, and do not change the original APK, original game ELF, or original OBB files.

Target remains exactly one custom native library: `libshotmeter.so`, loaded into the original game's process. No separate/repackaged NBA 2K20 installation.

## Step 10 — Direct original APK DEX/bootstrap inspection

The exact original APK was recovered from the user's Library:
- File: `NBA 2K27_98.0.2.apk`
- Size: 218,499,388 bytes
- SHA-256 previously audited: `6f949b86daec9eb76275a7942a0db01afad8747de1fdd89100bb2eab1c5d0d86`
- It was materialized read-only for inspection. No APK bytes were changed.

Direct ZIP/DEX inspection:
- APK contains a single `classes.dex` (21,319,173 bytes).
- DEX 035 string/type/method/class tables were parsed directly.
- This DEX is a small protected bootstrap/shell, not the full game Java implementation: 154 strings, 56 type IDs, 103 method IDs, and 6 class definitions.
- Classes include `sPfSUumNwmBLUIcV.ProxyApplication`, `ProxyComponentFactory`, `JniBridge`, and helper classes.
- The bootstrap has a custom `ProxyComponentFactory.instantiateClassLoader()` path and a custom Application `attachBaseContext()`.
- The bootstrap contains a single Java `System.load(String)` method reference. Direct bytecode analysis shows it is called from `JniBridge.a(String)`.
- `JniBridge.a(String)` constructs a path to the app-private `dpt-libs/lib82bdeb9e9e4f939b.so` file and passes that path to `System.load()`.
- The helper `a.c(String,String)` extracts/verifies the packaged shell library from APK assets under `assets/vwwwwwvwww/`; the APK contains ARM, ARM64, x86, and x86_64 variants.
- `ProxyComponentFactory.instantiateClassLoader()` calls the bootstrap's helper/native bridge methods and reflection to continue loading the protected app classes.
- No `DexClassLoader`, `InMemoryDexClassLoader`, `Runtime.load()`, or additional user-plugin loader API was found in this bootstrap DEX string/method tables.

## Finding

The original APK already has a native-loading mechanism, but the observed mechanism is for its own protected shell library:
`assets/vwwwwwvwww/arm64/lib82bdeb9e9e4f939b.so` -> app-private `dpt-libs/lib82bdeb9e9e4f939b.so` -> `System.load(absolutePath)`.

This is **not evidence of a supported plugin/extension point** for loading an extra user-supplied `libshotmeter.so`. The manifest-selected game library remains `libnba2k20_clean_opt.so` by ELF SONAME and `libnba2k20_clean_opt.so` by APK filename / `android.app.lib_name=nba2k20_clean_opt`; Android NativeActivity does not automatically load a second adjacent library.

## Next investigation — remain non-rooted

1. Inspect the protected shell native library's exports/imports and the full Java bootstrap call graph to determine whether it exposes a documented/legitimate extension callback or a generic native plugin loader. Do not assume it does.
2. Check the original APK's debuggable/instrumentation eligibility and Android 15 constraints for any legitimate non-rooted debugging/agent route. Do not assume Shizuku or ordinary file access grants process injection.
3. If neither app cooperation nor a legitimate non-rooted instrumentation route exists, state the hard constraint clearly: an unchanged non-debuggable app cannot be made to load an arbitrary external native library merely by copying it into storage. Ask before changing the APK or game ELF constraints.
4. Do not build a dummy `libshotmeter.so` and imply it can enter the original game process. Keep shotmeter hook research and any standalone ELF build separate from deployment feasibility.
5. Preserve the original APK, original game library, and OBBs byte-for-byte and in place.

## Status

- Non-rooted target: **confirmed**
- Original APK / game ELF / OBB preservation: **required**
- Existing protected bootstrap native load: **found and analyzed**
- Existing generic plugin loader: **not found**
- Non-rooted external process load path: **not established**
- Single `libshotmeter.so` implementation: **not yet consolidated**
- Runtime injection/hook/gameplay: **not tested**

This handoff supersedes v32's pending "root status" question. It does not supersede earlier technical findings except where explicitly stated above.

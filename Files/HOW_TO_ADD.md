# Shot Feedback and Horn Meter: How to Add Them

The project is split into separate source files, so you can add the shot feedback banner,
the horn meter, the green flame, or any combination.

Only use this on apps you own or have permission to modify.

## 1. What each file does

| File | Role | Needed? |
|---|---|---|
| `overlay_core.cpp` / `overlay_core.h` | Dobby hook on `eglSwapBuffers`, GL state save/restore, shared quad | Always |
| `horn_common.h` | Result names, colors, timing zones, meter placement | Always |
| `horn_meter.cpp` | **Horn meter only** | Optional |
| `shot_feedback.cpp` | **Shot feedback banner only** | Optional |
| `green_flame.cpp` | **Small green flame only** (flashes on an excellent shot) | Optional |
| `horn_font_atlas.h` | Banner text texture (generated) | With `shot_feedback.cpp` |
| `tools/gen_atlas.py` | Regenerates `horn_font_atlas.h` | Only to change text or font |
| `CMakeLists.txt` | Build file with switches for each module | Yes |

`horn_meter.cpp`, `shot_feedback.cpp` and `green_flame.cpp` never include each other. They only meet through the
core: when a shot is released, the core tells every module that is built in.

## 2. Feedback colors

| Release | Fill progress | Color | Banner text |
|---|---|---|---|
| Very early | below 80% | Red | Very Early |
| Slight early | 80% to 90% | White | Slight Early |
| Excellent | 90% to 97.5% | Green | Excellent |
| Slight late | 97.5% to 99.5% | Yellow | Slight Late |
| Very late | above 99.5% | Red | Very Late |

Colors and zones are in `horn_common.h` and apply to both modules.

## 3. Get Dobby

```
cd your-project
git clone https://github.com/jmpews/Dobby
```

## 4. Choose what to build and build it

You need the Android NDK.

```
# both (default)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
# banner only: add   -DHORN_METER=OFF
# meter only:  add   -DSHOT_FEEDBACK=OFF
# no flame:    add   -DGREEN_FLAME=OFF
cmake --build build
```

The result is `build/libhornmeter.so`. To add the shot feedback to your own CMake project
instead, add `overlay_core.cpp` and `shot_feedback.cpp` to your sources, put `horn_common.h`,
`overlay_core.h` and `horn_font_atlas.h` on the include path, and link `dobby EGL GLESv3 log`.

## 5. Load it into the game process

The hook installs itself when the library loads (the constructor in `overlay_core.cpp`). It must
run inside the process that renders the game:

- If you build the game or a mod, put the `.so` in `jniLibs/<abi>/` and call
  `System.loadLibrary("hornmeter")` early (before the first frame), for example in your
  `Application.onCreate()`.
- If you use your own loader or injector, load `libhornmeter.so` into the game process with it.

With the meter built in, you should see its demo loop in the middle of the screen right away:
the meter fills and releases in red, white, green, yellow and red. With the banner only, nothing
shows until your code reports a shot (step 6).

## 6. Connect it to the real shot

Declare these in your own native code (or copy the lines):

```cpp
extern "C" {
    // shot feedback (core API, works with either module)
    int  Shot_Release(float progress);   // classify a release, show the effects, returns 0..4
    void Shot_ReleaseResult(int result); // same, when your game already knows the result
    void Overlay_SetVisible(bool v);     // hide or show everything

    // shot feedback module
    void ShotFeedback_SetEnabled(bool v);

    // green flame module
    void GreenFlame_SetEnabled(bool v);

    // horn meter module
    void HornMeter_Set(float fill01);    // fill 0..1, or -1 for the demo loop
    void HornMeter_SetVisible(bool v);
}
```

Result values: `0` very early (red), `1` slight early (white), `2` excellent (green),
`3` slight late (yellow), `4` very late (red).

```cpp
// when the shot button is pressed
HornMeter_Set(0.0f);                  // meter only: takes control and stops the demo loop

// every frame while it is held
HornMeter_Set(shotProgress);          // meter only

// when the shot is released
int result = Shot_Release(shotProgress);   // meter flashes, banner pops in

// about 1.3 seconds later
HornMeter_Set(0.0f);                  // meter only: empty and ready for the next shot
```

The flame is a wispy, see-through green flame. Its noise texture is generated once at load, in
parallel on up to 8 CPU cores, so nothing heavy runs per frame.

The flame needs no extra calls: an excellent result from `Shot_Release` (or
`Shot_ReleaseResult(2)`) makes it flash and grow, burn for a moment, then fade out smoothly.

Using only the banner: skip every `HornMeter_*` call and just call `Shot_Release(progress)` (or
`Shot_ReleaseResult(result)`) when the shot is released.

From Java or Kotlin, wrap it with JNI:

```cpp
#include <jni.h>
extern "C" JNIEXPORT jint JNICALL
Java_com_example_Horn_release(JNIEnv*, jclass, jfloat p) { return Shot_Release(p); }
```

## 7. Settings

| Where | Setting | What it does |
|---|---|---|
| `horn_common.h` | `kCenterX`, `kCenterY`, `kHeightFrac` | meter position and size |
| `horn_common.h` | `kTint` | color of each result |
| `horn_common.h` | `kTooEarly`, `kWinLo`, `kWinHi`, `kSlightLate` | where each zone starts |
| `shot_feedback.cpp` | `kBannerDX`, `kBannerDY` | banner position relative to the meter |
| `shot_feedback.cpp` | `kBannerHeightFrac` | banner size |
| `shot_feedback.cpp` | `kBannerTime` | seconds the banner stays |
| `green_flame.cpp` | `kFlameSizeFrac` | flame size (side of its square drawing box) |
| `green_flame.cpp` | `FLAME_QUALITY` | 2 = full detail (default), 1 = one noise layer fewer (faster) |
| `green_flame.cpp` | `kAnchorX`, `kAnchorY`, `kOffsetX`, `kOffsetY` | where the flame sits on the meter |
| `green_flame.cpp` | `kIdle` | idle flame size (0 = hidden until an excellent shot, then fades out) |
| `green_flame.cpp` | `kFlashTime`, `kHotTime` | flash length and how long the flame burns at full size |
| `green_flame.cpp` | `kFadeOutTime` | how long the fade out takes |
| `horn_meter.cpp` | `HORN_SHADOW` | 0 removes the drop shadow (slightly cheaper) |

If you change `kWinLo`, change `WIN_LO` in the shader in `horn_meter.cpp` to the same value.
To change the banner text or font, edit `TEXTS` or `FONT` in `tools/gen_atlas.py`, run it, and
replace `horn_font_atlas.h`.

## 8. If something is wrong

- **Nothing shows:** the game may render somewhere other than the surface `eglSwapBuffers`
  presents. Add a log line in `hook_swap` (in `overlay_core.cpp`) to check that the hook runs.
- **Flicker or black frames:** some game GL state is not being restored. Add it to the
  save/restore lists in `drawAll()` (in `overlay_core.cpp`), for example the color mask.
- **Frame drops:** set `HORN_SHADOW` to 0. If it persists, the state queries in `drawAll()` are the
  next place to look.
- **Banner text garbled:** the game changed texture unit 0 or a sampler object. The banner saves
  and restores both while it draws, so check for another overlay hooking the same function.
- **Wrong position:** adjust the position settings in step 7.

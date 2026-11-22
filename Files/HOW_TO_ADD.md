# Shot Feedback: How to Add It

The shot feedback is the horn shot meter plus the result banner at its top right.
It is one native library (`libhornmeter.so`) that draws on top of the game with a Dobby hook.

Only use this on apps you own or have permission to modify.

## 1. Feedback colors

| Release | Fill progress | Color | Banner text |
|---|---|---|---|
| Very early | below 80% | Red | Very Early |
| Slight early | 80% to 90% | White | Slight Early |
| Excellent | 90% to 97.5% | Green | Excellent |
| Slight late | 97.5% to 99.5% | Yellow | Slight Late |
| Very late | above 99.5% | Red | Very Late |

On release, the whole horn (green zone included) flashes the color and the banner pops in
at the top right, stays about 1.3 seconds, then fades.

## 2. Files

```
your-project/
  CMakeLists.txt        build file
  horn_meter.cpp        all the source: hook, shaders, meter logic, banner
  horn_font_atlas.h     generated text texture for the banner (keep next to the .cpp)
  tools/gen_atlas.py    only needed to change banner text or font
  Dobby/                the Dobby hooking library (step 3)
```

## 3. Get Dobby

```
cd your-project
git clone https://github.com/jmpews/Dobby
```

## 4. Build

You need the Android NDK.

```
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
cmake --build build
```

The result is `build/libhornmeter.so`. Use `armeabi-v7a` as well if you target 32-bit devices.

## 5. Load it into the game process

The library installs its hook automatically when it is loaded (the constructor at the bottom of
`horn_meter.cpp`). It must run inside the same process that renders the game:

- If you build the game or a mod, put the `.so` in `jniLibs/<abi>/` and call
  `System.loadLibrary("hornmeter")` early (before the first frame), for example in your
  `Application.onCreate()`.
- If you use your own loader or injector, load `libhornmeter.so` into the game process
  with it.

Right after loading, you should see the demo loop in the middle of the screen: the meter
fills and releases in red, white, green, yellow and red, each with its banner.

## 6. Connect it to the real shot

Declare the functions in your own native code (or copy these lines):

```cpp
extern "C" {
    void HornMeter_Set(float fill01);        // fill progress 0..1, or -1 for the demo loop
    int  HornMeter_Release(float progress);  // returns 0..4 (see below), shows the effect
    void HornMeter_SetVisible(bool v);       // hide or show everything
    void HornMeter_SetBanner(bool v);        // hide or show only the banner
}
```

Then call them from your shot code:

```cpp
// when the shot button is pressed
HornMeter_Set(0.0f);                  // takes control (stops the demo loop)

// every frame while it is held
HornMeter_Set(shotProgress);          // 0.0 empty .. 1.0 full

// when the shot is released
int result = HornMeter_Release(shotProgress);
//   0 = very early (red)      1 = slight early (white)   2 = excellent (green)
//   3 = slight late (yellow)  4 = very late (red)

// about 1.3 seconds later (when the banner is gone)
HornMeter_Set(0.0f);                  // empty meter, ready for the next shot
```

If your game already decides the result itself, call `HornMeter_ReleaseResult(0..4)` instead,
so the colors match your game's own verdict.

If you call it from Java or Kotlin, wrap it with JNI:

```cpp
#include <jni.h>
extern "C" JNIEXPORT void JNICALL
Java_com_example_Horn_set(JNIEnv*, jclass, jfloat v) { HornMeter_Set(v); }
extern "C" JNIEXPORT jint JNICALL
Java_com_example_Horn_release(JNIEnv*, jclass, jfloat p) { return HornMeter_Release(p); }
```

## 7. Settings (top of `horn_meter.cpp`)

| Setting | What it does |
|---|---|
| `kCenterX`, `kCenterY` | meter position (fraction of the screen) |
| `kHeightFrac` | meter size |
| `kBannerDX`, `kBannerDY` | banner position relative to the meter |
| `kBannerHeightFrac` | banner size |
| `kBannerTime` | seconds the banner stays |
| `kTooEarly`, `kWinLo`, `kWinHi`, `kSlightLate` | where each result zone starts |
| `kTint` | the color of each result |
| `HORN_SHADOW` | 0 removes the drop shadow (slightly cheaper) |

If you change `kWinLo`, change `WIN_LO` in the horn shader to the same value.

To change the banner text or font, edit `TEXTS` or `FONT` in `tools/gen_atlas.py`, run it, and
replace `horn_font_atlas.h`.

## 8. If something is wrong

- **Nothing shows:** the game may render somewhere other than the surface `eglSwapBuffers`
  presents. Add a log line in `hook_swap` to check that the hook runs at all.
- **Flicker or black frames:** some game GL state is not being restored. Add it to the
  save/restore lists in `drawOverlay()` (for example the color mask).
- **Frame drops:** set `HORN_SHADOW` to 0. If it persists, the state queries in
  `drawOverlay()` are the next place to look.
- **Banner text garbled:** the game changed texture unit 0 or a sampler object. The code saves and
  restores both while the banner draws, so check for another overlay hooking the same function.
- **Wrong position:** adjust the position settings in step 7.

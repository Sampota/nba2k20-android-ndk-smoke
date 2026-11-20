#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "dobby.h"

#define LOG_TAG "ShotmeterDobbyV46"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAG_NAME "shotmeter.dobby-v46-player-anchored-diagnostic"

static constexpr uintptr_t kManagerOffset = 0x3c700e0ULL;
static constexpr uintptr_t kSavedViewOffset = 0x90ULL;
static constexpr size_t kViewSize = 0x240U;

static const char kSoname[] = "libnba_clean_opt.so";
static const char kFilename[] = "libnba2k20_clean_opt.so";
static const char kDoDrawSymbol[] =
    "_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR";
static const char kHeadSymbol[] =
    "_Z30PlayerIconUtil_GetHeadLocationP12AI_NBA_ACTORRDv4_f";
static const char kSavedGetterSymbol[] =
    "_Z32PlayerIconManager3D_GetSavedViewv";
static const char kProjectSymbol[] =
    "_Z14VCView_ProjectPK6VCVIEWPKDv4_fPS2_";
static const char kComponentSymbol[] = "g_ShotIndicatorComponent";
static const char kSwapSymbol[] = "eglSwapBuffers";

using DoDrawFn = void (*)(void *, void *);
using HeadFn = void (*)(void *, float *);
using ProjectFn = float (*)(const void *, const float *, float *);
using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using TestFn = int (*)(int);

static void *g_game = nullptr;
static const char *g_game_name = "not-found";
static uintptr_t g_game_base = 0;
static uintptr_t g_saved_view = 0;
static volatile uint8_t *g_component = nullptr;
static void *g_do_draw_address = nullptr;
static void *g_head_address = nullptr;
static void *g_saved_getter_address = nullptr;
static void *g_project_address = nullptr;
static void *g_swap_address = nullptr;
static DoDrawFn g_original_do_draw = nullptr;
static HeadFn g_get_head = nullptr;
static ProjectFn g_project = nullptr;
static SwapFn g_original_swap = nullptr;

static int g_self_code = -999;
static int g_self_base = -999;
static int g_self_original = -999;
static int g_self_patched = -999;
static int g_do_draw_code = -999;
static int g_swap_code = -999;

static std::atomic<uint32_t> g_attach_calls{0};
static std::atomic<uint32_t> g_duplicate_attach_calls{0};
static std::atomic<uint32_t> g_init_state{0};
static std::atomic<uint32_t> g_do_draw_calls{0};
static std::atomic<uint32_t> g_projection_calls{0};
static std::atomic<uint32_t> g_projection_valid{0};
static std::atomic<uint32_t> g_swap_calls{0};
static std::atomic<uint32_t> g_active_meter_frames{0};
static std::atomic<uint32_t> g_draw_calls{0};
static std::atomic<uint32_t> g_anchor_valid{0};
static std::atomic<uint32_t> g_anchor_x_bits{0};
static std::atomic<uint32_t> g_anchor_y_bits{0};
static std::atomic<uint32_t> g_anchor_z_bits{0};
static std::atomic<uint32_t> g_clip_w_bits{0};
static std::atomic<uint32_t> g_meter_bits{0};
static std::atomic<uint32_t> g_marker_bits{0};
static std::atomic<int32_t> g_component_state{-1};
static std::atomic<uint32_t> g_tracked_player{0};
static std::atomic<int32_t> g_surface_w{0};
static std::atomic<int32_t> g_surface_h{0};
static std::atomic<int32_t> g_viewport_w{0};
static std::atomic<int32_t> g_viewport_h{0};
static std::atomic<int32_t> g_framebuffer{-1};

static volatile int g_test_bias = 100;
__attribute__((noinline, optnone))
static int test_target(int x) { return x + g_test_bias; }
static TestFn g_test_original = nullptr;
__attribute__((noinline, optnone))
static int test_proxy(int x) {
    return g_test_original ? g_test_original(x) + 7 : -9999;
}
__attribute__((noinline, optnone))
static int invoke_test(int x) {
    TestFn volatile fn = &test_target;
    return fn(x);
}

static void store_float(std::atomic<uint32_t> &dst, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    dst.store(bits, std::memory_order_relaxed);
}
static float load_float(const std::atomic<uint32_t> &src) {
    const uint32_t bits = src.load(std::memory_order_relaxed);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void *open_game(void) {
    void *handle = dlopen(kSoname, RTLD_NOW | RTLD_NOLOAD);
    if (handle) {
        g_game_name = kSoname;
        return handle;
    }
    handle = dlopen(kFilename, RTLD_NOW | RTLD_NOLOAD);
    if (handle) g_game_name = kFilename;
    return handle;
}
static void *resolve(void *handle, const char *name) {
    if (!handle) return nullptr;
    dlerror();
    void *address = dlsym(handle, name);
    const char *error = dlerror();
    if (error) {
        LOGE("dlsym %s failed: %s", name, error);
        return nullptr;
    }
    LOGI("resolved %s at %p", name, address);
    return address;
}

static bool derive_saved_view(void) {
    if (!g_saved_getter_address) return false;
    Dl_info info{};
    if (dladdr(g_saved_getter_address, &info) == 0 || !info.dli_fbase)
        return false;
    g_game_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    g_saved_view = g_game_base + kManagerOffset + kSavedViewOffset;
    return true;
}

static void capture_anchor(void *actor) {
    if (!actor || !g_get_head || !g_project || !g_saved_view) return;
    float head[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_get_head(actor, head);

    alignas(16) uint8_t view[kViewSize] = {};
    memcpy(view, reinterpret_cast<const void *>(g_saved_view), sizeof(view));
    float screen[4] = {NAN, NAN, NAN, NAN};
    const float reciprocal_clip_w =
        g_project(static_cast<const void *>(view), head, screen);

    g_projection_calls.fetch_add(1, std::memory_order_relaxed);
    store_float(g_anchor_x_bits, screen[0]);
    store_float(g_anchor_y_bits, screen[1]);
    store_float(g_anchor_z_bits, screen[2]);
    store_float(g_clip_w_bits, reciprocal_clip_w);

    const bool valid = std::isfinite(screen[0]) &&
        std::isfinite(screen[1]) && std::isfinite(screen[2]) &&
        std::isfinite(reciprocal_clip_w) && reciprocal_clip_w > 1.0e-8f &&
        screen[2] >= 0.0f && screen[2] <= 1.0f;
    if (valid) {
        g_projection_valid.fetch_add(1, std::memory_order_relaxed);
        g_anchor_valid.store(1, std::memory_order_release);
    } else {
        g_anchor_valid.store(0, std::memory_order_release);
    }
}

__attribute__((noinline))
static void do_draw_proxy(void *component, void *actor) {
    g_do_draw_calls.fetch_add(1, std::memory_order_relaxed);
    capture_anchor(actor);
    const DoDrawFn original = g_original_do_draw;
    if (original) original(component, actor);
}

static void clear_rect(GLint x, GLint y, GLsizei w, GLsizei h,
                       GLfloat r, GLfloat g, GLfloat b) {
    if (w <= 0 || h <= 0) return;
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* Small square arc segments form a curved, right-side player-anchored meter. */
static void draw_curved_meter(GLint cx, GLint cy, GLint radius,
                              GLint dot, float progress) {
    constexpr int kSegments =  forty_placeholder;
}

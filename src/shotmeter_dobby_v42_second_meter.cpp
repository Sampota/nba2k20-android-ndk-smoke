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

#define LOG_TAG "ShotmeterDobbyV42"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAG_NAME "shotmeter.dobby-v42-second-meter-diagnostic"

static const char kSoname[] = "libnba_clean_opt.so";
static const char kFilename[] = "libnba2k20_clean_opt.so";
static const char kComponentSymbol[] = "g_ShotIndicatorComponent";
static const char kSwapSymbol[] = "eglSwapBuffers";
static const char kResetName[] = "shotmeter.dobby-v42-reset";

using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
static SwapFn g_original_swap = nullptr;
static void *g_game_handle = nullptr;
static const char *g_game_name = "not-found";
static volatile uint8_t *g_component = nullptr;
static void *g_swap_address = nullptr;
static int g_self_code = -999, g_self_base = -999, g_self_orig = -999, g_self_patch = -999;
static int g_swap_code = -999;

static std::atomic<uint32_t> g_frames{0}, g_active_frames{0}, g_draws{0}, g_resets{0};
static std::atomic<int32_t> g_state{-1}, g_surface_w{0}, g_surface_h{0};
static std::atomic<int32_t> g_view_w{0}, g_view_h{0}, g_fbo{-1};
static std::atomic<uint32_t> g_player{0}, g_meter_bits{0}, g_marker_bits{0};

static volatile int g_bias = 100;
__attribute__((noinline, optnone)) static int test_target(int x) { return x + g_bias; }
using TestFn = int (*)(int);
static TestFn g_test_original = nullptr;
__attribute__((noinline, optnone)) static int test_proxy(int x) {
    return g_test_original ? g_test_original(x) + 7 : -9999;
}
__attribute__((noinline, optnone)) static int invoke_test(int x) {
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
    float value = 0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void *open_game(void) {
    void *h = dlopen(kSoname, RTLD_NOW | RTLD_NOLOAD);
    if (h) { g_game_name = kSoname; return h; }
    h = dlopen(kFilename, RTLD_NOW | RTLD_NOLOAD);
    if (h) g_game_name = kFilename;
    return h;
}
static void *symbol(void *handle, const char *name) {
    if (!handle) return nullptr;
    dlerror();
    void *p = dlsym(handle, name);
    const char *e = dlerror();
    if (e) LOGE("dlsym %s: %s", name, e);
    else LOGI("resolved %s at %p", name, p);
    return e ? nullptr : p;
}

static void reset_counters(void) {
    g_frames.store(0); g_active_frames.store(0); g_draws.store(0);
    g_state.store(-1); g_surface_w.store(0); g_surface_h.store(0);
    g_view_w.store(0); g_view_h.store(0); g_fbo.store(-1);
    g_player.store(0); g_meter_bits.store(0); g_marker_bits.store(0);
    g_resets.fetch_add(1);
}
static void consume_reset(void) {
    char path[256];
    int n = snprintf(path, sizeof(path), "%s/%s", FILES_DIR, kResetName);
    if (n <= 0 || (size_t)n >= sizeof(path)) return;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    close(fd);
    if (unlink(path) == 0) {
        reset_counters();
        LOGI("v42 diagnostic reset consumed");
    } else LOGE("reset unlink failed errno=%d", errno);
}

static void clear_rect(GLint x, GLint y, GLsizei w, GLsizei h,
                       GLfloat r, GLfloat g, GLfloat b) {
    if (w <= 0 || h <= 0) return;
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* Draw an independent HUD meter from the stock timing-position value, read-only. */
static void draw_meter(EGLDisplay display, EGLSurface surface) {
    volatile uint8_t *c = g_component;
    if (!c) return;

    const uintptr_t tracked = *reinterpret_cast<volatile uintptr_t *>(c + 0x08);
    const float raw = *reinterpret_cast<volatile float *>(c + 0x10);
    const int32_t state = *reinterpret_cast<volatile int32_t *>(c + 0x18);
    g_state.store(state, std::memory_order_relaxed);
    g_player.store(tracked ? 1U : 0U, std::memory_order_relaxed);
    store_float(g_meter_bits, raw);

    /* State 2 is the ordinary jumpshot timing path. Do not display in idle/free-throw states. */
    if (!tracked || state != 2 || !std::isfinite(raw)) return;
    /* Update_Game encodes left-to-right timing position in [0.0, 0.5]. */
    const float marker = std::max(0.0f, std::min(1.0f, raw * 2.0f));
    store_float(g_marker_bits, marker);
    g_active_frames.fetch_add(1, std::memory_order_relaxed);

    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return;
    EGLint sw = 0, sh = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &sw) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &sh) != EGL_TRUE || sw <= 0 || sh <= 0) return;
    g_surface_w.store(sw); g_surface_h.store(sh);

    GLint vp[4] = {0, 0, 0, 0}, fbo = -1;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    g_view_w.store(vp[2]); g_view_h.store(vp[3]); g_fbo.store(fbo);
    if (fbo != 0 || vp[2] <= 0 || vp[3] <= 0) return;

    const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint old_scissor[4] = {0, 0, 0, 0};
    GLboolean old_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLfloat old_clear[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_SCISSOR_BOX, old_scissor);
    glGetBooleanv(GL_COLOR_WRITEMASK, old_mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    const GLint width = std::min(420, std::max(120, vp[2] * 44 / 100));
    const GLint height = std::min(20, std::max(12, vp[3] / 100));
    const GLint border = 2;
    const GLint x = vp[0] + (vp[2] - width) / 2;
    const GLint y = vp[1] + vp[3] / 12;
    const GLint inner_w = width - 2 * border;
    const GLint inner_h = height - 2 * border;
    const GLint zone_w = std::max(8, inner_w * 10 / 100);
    const GLint zone_x = x + border + (inner_w - zone_w) / 2;
    const GLint marker_w = 5;
    const GLint marker_x = x + border +
        static_cast<GLint>(std::lround((inner_w - marker_w) * marker));

    clear_rect(x, y, width, height, 0.70f, 0.77f, 0.82f);
    clear_rect(x + border, y + border, inner_w, inner_h, 0.025f, 0.04f, 0.05f);
    clear_rect(zone_x, y + border, zone_w, inner_h, 0.08f, 0.78f, 0.24f);
    clear_rect(marker_x, y + border, marker_w, inner_h, 0.96f, 0.98f, 1.0f);

    glScissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
    if (was_scissor == GL_FALSE) glDisable(GL_SCISSOR_TEST);
    glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    g_draws.fetch_add(1, std::memory_order_relaxed);
}

__attribute__((noinline))
static EGLBoolean swap_proxy(EGLDisplay display, EGLSurface surface) {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    draw_meter(display, surface);
    return g_original_swap ? g_original_swap(display, surface) : EGL_FALSE;
}

static void write_diagnostic(void) {
    char path[256];
    int n = snprintf(path, sizeof(path), "%s/%s", FILES_DIR, DIAG_NAME);
    if (n <= 0 || (size_t)n >= sizeof(path)) return;
    const uint32_t frames = g_frames.load(), active = g_active_frames.load(), draws = g_draws.load();
    const char *status = "HOOK_REGISTRATION_FAILED";
    if (!g_game_handle || !g_component) status = "GAME_COMPONENT_NOT_FOUND";
    else if (g_swap_code != 0 || !g_original_swap) status = "EGL_HOOK_REGISTRATION_FAILED";
    else if (draws) status = "SECOND_METER_DRAWN";
    else if (active) status = "METER_SOURCE_ACTIVE_RENDER_PENDING";
    else if (frames) status = "EGL_CALLBACKS_OBSERVED_WAITING_FOR_JUMPSHOT";
    else status = "HOOKS_REGISTERED_WAITING_FOR_FRAMES";

    char body[2048];
    snprintf(body, sizeof(body),
        "agent=v42\nengine=Dobby\n"
        "engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7\n"
        "diagnostic_reset_command=touch files/shotmeter.dobby-v42-reset\n"
        "diagnostic_reset_count=%u\n"
        "self_test_status=%s\nself_test_hook_code=%d\n"
        "self_test_baseline=%d\nself_test_original=%d\nself_test_patched=%d\n"
        "self_test_expected_patched=112\n"
        "target_library=%s\ncomponent_symbol=%s\ncomponent_address=%p\n"
        "egl_swap_buffers_address=%p\negl_swap_buffers_hook_code=%d\n"
        "frame_callbacks=%u\nactive_meter_frames=%u\noverlay_draw_calls=%u\n"
        "last_component_state=%d\nlast_tracked_player_present=%u\n"
        "last_meter_value=%.7g\nlast_meter_position=%.7g\n"
        "last_surface_width=%d\nlast_surface_height=%d\n"
        "last_viewport_width=%d\nlast_viewport_height=%d\nlast_framebuffer_binding=%d\n"
        "component_memory_modified=0\noriginal_meter_resource_modified=0\n"
        "shot_timing_modified=0\noriginal_library_file_modified=0\nstatus=%s\n",
        g_resets.load(),
        (g_self_code == 0 && g_self_base == 105 && g_self_orig == 105 && g_self_patch == 112) ? "PASS" : "FAIL",
        g_self_code, g_self_base, g_self_orig, g_self_patch,
        g_game_name, kComponentSymbol, (const void *)g_component,
        g_swap_address, g_swap_code, frames, active, draws,
        g_state.load(), g_player.load(), load_float(g_meter_bits), load_float(g_marker_bits),
        g_surface_w.load(), g_surface_h.load(), g_view_w.load(), g_view_h.load(), g_fbo.load(), status);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { LOGE("cannot write diagnostic errno=%d", errno); return; }
    const size_t len = strlen(body);
    const ssize_t wrote = write(fd, body, len);
    if (wrote < 0 || static_cast<size_t>(wrote) != len)
        LOGE("incomplete diagnostic write %ld/%lu", (long)wrote, (unsigned long)len);
    close(fd);
}

static void *reporter(void *) {
    for (int i = 0; i < 600; ++i) {
        consume_reset();
        write_diagnostic();
        sleep(1);
    }
    write_diagnostic();
    return nullptr;
}

extern "C" JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *, void *) {
    if (!vm) return JNI_ERR;
    g_self_base = invoke_test(5);
    void *test_original = nullptr;
    g_self_code = DobbyHook((void *)&test_target, (void *)&test_proxy, &test_original);
    g_test_original = reinterpret_cast<TestFn>(test_original);
    g_self_orig = g_test_original ? g_test_original(5) : -1;
    g_self_patch = invoke_test(5);
    if (g_self_code != 0 || g_self_base != 105 || g_self_orig != 105 || g_self_patch != 112) {
        write_diagnostic();
        return JNI_OK;
    }

    g_game_handle = open_game();
    if (g_game_handle) {
        g_component = reinterpret_cast<volatile uint8_t *>(symbol(g_game_handle, kComponentSymbol));
        g_swap_address = symbol(g_game_handle, kSwapSymbol);
        if (g_swap_address) {
            void *original = nullptr;
            g_swap_code = DobbyHook(g_swap_address, (void *)&swap_proxy, &original);
            g_original_swap = reinterpret_cast<SwapFn>(original);
            LOGI("eglSwapBuffers hook code=%d trampoline=%p", g_swap_code, original);
        } else g_swap_code = -1002;
    } else g_swap_code = -1001;

    write_diagnostic();
    pthread_t thread;
    if (pthread_create(&thread, nullptr, reporter, nullptr) == 0) pthread_detach(thread);
    else LOGE("reporter thread creation failed");
    LOGI("v42 second shotmeter attached; original component is read-only");
    return JNI_OK;
}

extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *) {
    LOGI("v42 second shotmeter agent unloading");
}

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
static const char kDoDrawSymbol[] = "_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR";
static const char kHeadSymbol[] = "_Z30PlayerIconUtil_GetHeadLocationP12AI_NBA_ACTORRDv4_f";
static const char kSavedGetterSymbol[] = "_Z32PlayerIconManager3D_GetSavedViewv";
static const char kProjectSymbol[] = "_Z14VCView_ProjectPK6VCVIEWPKDv4_fPS2_";
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

static int g_self_code = -999, g_self_base = -999;
static int g_self_original = -999, g_self_patched = -999;
static int g_do_draw_code = -999, g_swap_code = -999;
static std::atomic<uint32_t> g_attach_calls{0}, g_duplicate_attach_calls{0};
static std::atomic<uint32_t> g_init_state{0};
static std::atomic<uint32_t> g_do_draw_calls{0}, g_projection_calls{0};
static std::atomic<uint32_t> g_projection_valid{0}, g_swap_calls{0};
static std::atomic<uint32_t> g_active_meter_frames{0}, g_draw_calls{0};
static std::atomic<uint32_t> g_anchor_valid{0};
static std::atomic<uint32_t> g_anchor_x_bits{0}, g_anchor_y_bits{0};
static std::atomic<uint32_t> g_anchor_z_bits{0}, g_clip_w_bits{0};
static std::atomic<uint32_t> g_meter_bits{0}, g_marker_bits{0};
static std::atomic<int32_t> g_component_state{-1};
static std::atomic<uint32_t> g_tracked_player{0};
static std::atomic<int32_t> g_surface_w{0}, g_surface_h{0};
static std::atomic<int32_t> g_viewport_w{0}, g_viewport_h{0};
static std::atomic<int32_t> g_framebuffer{-1};

static volatile int g_test_bias = 100;
__attribute__((noinline, optnone)) static int test_target(int x) { return x + g_test_bias; }
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
    float value = 0.0f;
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
static void *resolve(void *handle, const char *name) {
    if (!handle) return nullptr;
    dlerror();
    void *p = dlsym(handle, name);
    const char *e = dlerror();
    if (e) { LOGE("dlsym %s failed: %s", name, e); return nullptr; }
    LOGI("resolved %s at %p", name, p);
    return p;
}
static bool derive_saved_view(void) {
    if (!g_saved_getter_address) return false;
    Dl_info info{};
    if (dladdr(g_saved_getter_address, &info) == 0 || !info.dli_fbase) return false;
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
    const float reciprocal_clip_w = g_project(view, head, screen);
    g_projection_calls.fetch_add(1, std::memory_order_relaxed);
    store_float(g_anchor_x_bits, screen[0]);
    store_float(g_anchor_y_bits, screen[1]);
    store_float(g_anchor_z_bits, screen[2]);
    store_float(g_clip_w_bits, reciprocal_clip_w);
    const bool valid = std::isfinite(screen[0]) && std::isfinite(screen[1]) &&
        std::isfinite(screen[2]) && std::isfinite(reciprocal_clip_w) &&
        reciprocal_clip_w > 1.0e-8f && screen[2] >= 0.0f && screen[2] <= 1.0f;
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

/* Arc opens toward the player and sits to the right of the projected head. */
static void draw_curved_meter(GLint cx, GLint cy, GLint radius,
                              GLint dot, float progress) {
    constexpr int kSegments = 40;
    const float start = -110.0f * 0.017453292519943295f;
    const float sweep = 220.0f * 0.017453292519943295f;
    for (int i = 0; i <= kSegments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSegments);
        const float a = start + sweep * t;
        const GLint x = cx + static_cast<GLint>(std::lround(std::cos(a) * radius));
        const GLint y = cy - static_cast<GLint>(std::lround(std::sin(a) * radius));
        const bool target = (t >= 0.84f && t <= 0.94f);
        clear_rect(x - dot / 2, y - dot / 2, dot, dot,
                   target ? 0.08f : 0.52f,
                   target ? 0.82f : 0.61f,
                   target ? 0.27f : 0.69f);
        if (std::fabs(t - progress) <= (0.5f / static_cast<float>(kSegments))) {
            const GLint marker = dot + std::max(2, dot / 2);
            clear_rect(x - marker / 2, y - marker / 2, marker, marker,
                       0.98f, 0.99f, 1.0f);
        }
    }
}

static void draw_overlay(EGLDisplay display, EGLSurface surface) {
    if (!g_component || g_anchor_valid.load(std::memory_order_acquire) == 0) return;
    const uintptr_t tracked = *reinterpret_cast<volatile uintptr_t *>(g_component + 0x08);
    const float raw = *reinterpret_cast<volatile float *>(g_component + 0x10);
    const int32_t state = *reinterpret_cast<volatile int32_t *>(g_component + 0x18);
    g_component_state.store(state, std::memory_order_relaxed);
    g_tracked_player.store(tracked ? 1U : 0U, std::memory_order_relaxed);
    store_float(g_meter_bits, raw);
    /* Same stock jumpshot-only gate and normalized source used by v42. */
    if (!tracked || state != 2 || !std::isfinite(raw)) return;
    const float progress = std::max(0.0f, std::min(1.0f, raw * 2.0f));
    store_float(g_marker_bits, progress);
    g_active_meter_frames.fetch_add(1, std::memory_order_relaxed);
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return;

    EGLint sw = 0, sh = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &sw) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &sh) != EGL_TRUE ||
        sw <= 0 || sh <= 0) return;
    GLint vp[4] = {0, 0, 0, 0}, fbo = -1;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    g_surface_w.store(sw); g_surface_h.store(sh);
    g_viewport_w.store(vp[2]); g_viewport_h.store(vp[3]);
    g_framebuffer.store(fbo);
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

    const float sx = static_cast<float>(vp[2]) / static_cast<float>(sw);
    const float sy = static_cast<float>(vp[3]) / static_cast<float>(sh);
    const float anchor_x = load_float(g_anchor_x_bits);
    const float anchor_y = load_float(g_anchor_y_bits);
    const GLint ax = vp[0] + static_cast<GLint>(std::lround(anchor_x * sx));
    const GLint top_y = static_cast<GLint>(std::lround(anchor_y * sy));
    const GLint ay = vp[1] + vp[3] - top_y;
    const float scale = std::max(0.65f, std::min(1.35f, sy * 1.0f));
    const GLint radius = std::max(18, static_cast<GLint>(std::lround(31.0f * scale)));
    const GLint dot = std::max(3, static_cast<GLint>(std::lround(5.0f * scale)));
    const GLint cx = ax + radius + static_cast<GLint>(std::lround(14.0f * scale));
    draw_curved_meter(cx, ay, radius, dot, progress);

    glScissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
    if (was_scissor == GL_FALSE) glDisable(GL_SCISSOR_TEST);
    glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
}

__attribute__((noinline))
static EGLBoolean swap_proxy(EGLDisplay display, EGLSurface surface) {
    g_swap_calls.fetch_add(1, std::memory_order_relaxed);
    draw_overlay(display, surface);
    return g_original_swap ? g_original_swap(display, surface) : EGL_FALSE;
}

static void write_diagnostic(void) {
    char path[256];
    const int n = snprintf(path, sizeof(path), "%s/%s", FILES_DIR, DIAG_NAME);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) return;
    const bool self_ok = g_self_code == 0 && g_self_base == 105 &&
        g_self_original == 105 && g_self_patched == 112;
    const char *status = "HOOK_REGISTRATION_FAILED";
    if (!self_ok) status = "DOBBY_SELF_TEST_FAILED";
    else if (!g_game || !g_component) status = "GAME_COMPONENT_NOT_FOUND";
    else if (g_do_draw_code != 0 || !g_original_do_draw)
        status = "PLAYER_ANCHOR_HOOK_FAILED";
    else if (g_swap_code != 0 || !g_original_swap)
        status = "EGL_HOOK_FAILED";
    else if (g_draw_calls.load()) status = "PLAYER_ANCHORED_CURVED_METER_DRAWN";
    else if (g_active_meter_frames.load()) status = "METER_ACTIVE_WAITING_FOR_RENDER";
    else if (g_anchor_valid.load()) status = "PLAYER_ANCHOR_READY_WAITING_FOR_SHOT";
    else if (g_swap_calls.load()) status = "WAITING_FOR_VALID_PLAYER_PROJECTION";
    else status = "HOOKS_READY_WAITING_FOR_FRAMES";

    char body[2400];
    (void)snprintf(body, sizeof(body),
        "agent=v46\npurpose=player_anchored_curved_second_meter\nengine=Dobby\n"
        "engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7\n"
        "target_library=%s\ncomponent_symbol=%s\ncomponent_address=%p\n"
        "do_draw_address=%p\ndo_draw_hook_code=%d\nhead_location_address=%p\n"
        "saved_view_address=%p\nproject_address=%p\negl_swap_buffers_address=%p\n"
        "egl_swap_buffers_hook_code=%d\nself_test_status=%s\nself_test_hook_code=%d\n"
        "self_test_baseline=%d\nself_test_original=%d\nself_test_patched=%d\n"
        "attach_calls=%u\nduplicate_attach_calls=%u\ninitialization_state=%u\n"
        "do_draw_callbacks=%u\nprojection_calls=%u\nprojection_valid=%u\n"
        "last_anchor_x=%.8g\nlast_anchor_y=%.8g\nlast_anchor_z=%.8g\n"
        "last_reciprocal_clip_w=%.8g\nanchor_valid=%u\negl_swap_callbacks=%u\n"
        "active_meter_frames=%u\noverlay_draw_calls=%u\nlast_component_state=%d\n"
        "last_tracked_player_present=%u\nlast_meter_value=%.7g\nlast_meter_position=%.7g\n"
        "surface_width=%d\nsurface_height=%d\nviewport_width=%d\nviewport_height=%d\n"
        "framebuffer_binding=%d\nsaved_view_copy_only=1\ngame_owned_view_bytes_written=0\n"
        "game_object_fields_modified=0\ncomponent_memory_modified=0\n"
        "original_meter_resource_modified=0\nshot_timing_modified=0\n"
        "original_library_file_modified=0\nstatus=%s\n",
        g_game_name, kComponentSymbol, (const void *)g_component,
        g_do_draw_address, g_do_draw_code, g_head_address, (void *)g_saved_view,
        g_project_address, g_swap_address, g_swap_code,
        self_ok ? "PASS" : "FAIL", g_self_code, g_self_base,
        g_self_original, g_self_patched, g_attach_calls.load(),
        g_duplicate_attach_calls.load(), g_init_state.load(), g_do_draw_calls.load(),
        g_projection_calls.load(), g_projection_valid.load(),
        load_float(g_anchor_x_bits), load_float(g_anchor_y_bits),
        load_float(g_anchor_z_bits), load_float(g_clip_w_bits), g_anchor_valid.load(),
        g_swap_calls.load(), g_active_meter_frames.load(), g_draw_calls.load(),
        g_component_state.load(), g_tracked_player.load(), load_float(g_meter_bits),
        load_float(g_marker_bits), g_surface_w.load(), g_surface_h.load(),
        g_viewport_w.load(), g_viewport_h.load(), g_framebuffer.load(), status);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { LOGE("diagnostic open failed errno=%d", errno); return; }
    const size_t len = strlen(body);
    const ssize_t wrote = write(fd, body, len);
    if (wrote < 0 || static_cast<size_t>(wrote) != len)
        LOGE("diagnostic write incomplete %ld/%lu", (long)wrote, (unsigned long)len);
    close(fd);
}
static void *reporter(void *) {
    for (;;) { write_diagnostic(); sleep(1); }
    return nullptr;
}

extern "C" JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM *vm, char *, void *) {
    if (!vm) return JNI_ERR;
    g_attach_calls.fetch_add(1, std::memory_order_relaxed);
    uint32_t expected = 0;
    if (!g_init_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        g_duplicate_attach_calls.fetch_add(1, std::memory_order_relaxed);
        return JNI_OK;
    }
    g_self_base = invoke_test(5);
    void *test_original = nullptr;
    g_self_code = DobbyHook((void *)&test_target, (void *)&test_proxy, &test_original);
    g_test_original = reinterpret_cast<TestFn>(test_original);
    g_self_original = g_test_original ? g_test_original(5) : -1;
    g_self_patched = invoke_test(5);
    const bool self_ok = g_self_code == 0 && g_self_base == 105 &&
        g_self_original == 105 && g_self_patched == 112;
    if (!self_ok) {
        g_init_state.store(3, std::memory_order_release);
        write_diagnostic();
        return JNI_OK;
    }

    g_game = open_game();
    if (g_game) {
        g_component = reinterpret_cast<volatile uint8_t *>(resolve(g_game, kComponentSymbol));
        g_do_draw_address = resolve(g_game, kDoDrawSymbol);
        g_head_address = resolve(g_game, kHeadSymbol);
        g_saved_getter_address = resolve(g_game, kSavedGetterSymbol);
        g_project_address = resolve(g_game, kProjectSymbol);
        g_swap_address = resolve(g_game, kSwapSymbol);
        g_get_head = reinterpret_cast<HeadFn>(g_head_address);
        g_project = reinterpret_cast<ProjectFn>(g_project_address);
        const bool view_ok = derive_saved_view();
        if (g_do_draw_address && g_get_head && g_project && view_ok) {
            void *original = nullptr;
            g_do_draw_code = DobbyHook(g_do_draw_address, (void *)&do_draw_proxy, &original);
            g_original_do_draw = reinterpret_cast<DoDrawFn>(original);
        } else g_do_draw_code = -1002;
        if (g_swap_address) {
            void *original = nullptr;
            g_swap_code = DobbyHook(g_swap_address, (void *)&swap_proxy, &original);
            g_original_swap = reinterpret_cast<SwapFn>(original);
        } else g_swap_code = -1003;
    } else {
        g_do_draw_code = -1001;
        g_swap_code = -1001;
    }

    g_init_state.store(2, std::memory_order_release);
    write_diagnostic();
    pthread_t thread;
    if (pthread_create(&thread, nullptr, reporter, nullptr) == 0) pthread_detach(thread);
    else LOGE("diagnostic reporter thread creation failed");
    LOGI("v46 player-anchored curved meter attached; saved view copied privately; stock game state remains read-only");
    return JNI_OK;
}
extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *) {
    LOGI("v46 player-anchored curved meter unloading");
}

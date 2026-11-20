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
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <time.h>

#include "dobby.h"

#define LOG_TAG "ShotmeterDobbyV47"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAG_NAME "shotmeter.dobby-v47-crescent-perfect-diagnostic"

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
static const char kReleaseEventSymbol[] = "_Z20EVT_UserShotReleasedP12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGff";

using DoDrawFn = void (*)(void *, void *);
using HeadFn = void (*)(void *, float *);
using ProjectFn = float (*)(const void *, const float *, float *);
using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using TestFn = int (*)(int);
using ReleaseFn = void (*)(void *, int32_t, float, float);

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
static void *g_release_event_address = nullptr;
static DoDrawFn g_original_do_draw = nullptr;
static HeadFn g_get_head = nullptr;
static ProjectFn g_project = nullptr;
static SwapFn g_original_swap = nullptr;
static ReleaseFn g_original_release_event = nullptr;

static int g_self_code = -999, g_self_base = -999;
static int g_self_original = -999, g_self_patched = -999;
static int g_do_draw_code = -999, g_swap_code = -999, g_release_event_code = -999;
static std::atomic<uint32_t> g_attach_calls{0}, g_duplicate_attach_calls{0};
static std::atomic<uint32_t> g_init_state{0};
static std::atomic<uint32_t> g_do_draw_calls{0}, g_projection_calls{0};
static std::atomic<uint32_t> g_projection_valid{0}, g_swap_calls{0};
static std::atomic<uint32_t> g_active_meter_frames{0}, g_draw_calls{0};
static std::atomic<uint32_t> g_release_event_calls{0}, g_perfect_release_events{0}, g_perfect_flash_frames{0};
static std::atomic<int32_t> g_last_release_timing{-1};
static std::atomic<uint64_t> g_perfect_until_ns{0};
static std::atomic<uint32_t> g_anchor_valid{0};
static std::atomic<uint32_t> g_anchor_x_bits{0}, g_anchor_y_bits{0};
static std::atomic<uint32_t> g_anchor_z_bits{0}, g_clip_w_bits{0};
static std::atomic<uint32_t> g_meter_bits{0}, g_marker_bits{0};
static std::atomic<int32_t> g_component_state{-1};
static std::atomic<uint32_t> g_tracked_player{0};
static std::atomic<int32_t> g_surface_w{0}, g_surface_h{0};
static std::atomic<int32_t> g_viewport_w{0}, g_viewport_h{0};
static std::atomic<int32_t> g_framebuffer{-1};
static std::atomic<uint32_t> g_sound_play_count{0};
static std::atomic<int32_t> g_sound_init_code{-999};
static std::atomic<uint64_t> g_last_sound_ns{0};
static pthread_mutex_t g_sound_mutex = PTHREAD_MUTEX_INITIALIZER;

static SLObjectItf g_sl_engine_object = nullptr;
static SLEngineItf g_sl_engine = nullptr;
static SLObjectItf g_sl_output_mix = nullptr;
static SLObjectItf g_sl_player_object = nullptr;
static SLPlayItf g_sl_play = nullptr;
static SLAndroidSimpleBufferQueueItf g_sl_queue = nullptr;
static constexpr uint32_t kToneSamples = 5292;
static int16_t g_tone_samples[kToneSamples]{};

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

static uint64_t monotonic_ns(void) {
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static bool initialize_sound(void) {
    SLresult result = slCreateEngine(&g_sl_engine_object, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_engine_object)->Realize(g_sl_engine_object, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_engine_object)->GetInterface(g_sl_engine_object, SL_IID_ENGINE, &g_sl_engine);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_engine)->CreateOutputMix(g_sl_engine, &g_sl_output_mix, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_output_mix)->Realize(g_sl_output_mix, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }

    SLDataLocator_AndroidSimpleBufferQueue queue_locator = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2
    };
    SLDataFormat_PCM pcm_format = {
        SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_44_1,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource source = {&queue_locator, &pcm_format};
    SLDataLocator_OutputMix output_locator = {
        SL_DATALOCATOR_OUTPUTMIX, g_sl_output_mix
    };
    SLDataSink sink = {&output_locator, nullptr};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean required[] = {SL_BOOLEAN_TRUE};
    result = (*g_sl_engine)->CreateAudioPlayer(
        g_sl_engine, &g_sl_player_object, &source, &sink, 1, ids, required);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_player_object)->Realize(g_sl_player_object, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_player_object)->GetInterface(g_sl_player_object, SL_IID_PLAY, &g_sl_play);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_player_object)->GetInterface(
        g_sl_player_object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_sl_queue);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }

    for (uint32_t i = 0; i < kToneSamples; ++i) {
        const float t = static_cast<float>(i) / 44100.0f;
        const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(kToneSamples);
        const float sample = std::sin(6.28318530718f * 880.0f * t) * envelope * 6200.0f;
        g_tone_samples[i] = static_cast<int16_t>(sample);
    }
    g_sound_init_code.store(0);
    return true;
}

static void play_perfect_sound(void) {
    if (!g_sl_play || !g_sl_queue) return;
    const uint64_t now = monotonic_ns();
    pthread_mutex_lock(&g_sound_mutex);
    const uint64_t previous = g_last_sound_ns.load(std::memory_order_relaxed);
    if (previous == 0 || now < previous || now - previous >= 450000000ULL) {
        const SLresult clear_result = (*g_sl_queue)->Clear(g_sl_queue);
        const SLresult enqueue_result = clear_result == SL_RESULT_SUCCESS
            ? (*g_sl_queue)->Enqueue(g_sl_queue, g_tone_samples, sizeof(g_tone_samples))
            : clear_result;
        if (enqueue_result == SL_RESULT_SUCCESS &&
            (*g_sl_play)->SetPlayState(g_sl_play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS) {
            g_last_sound_ns.store(now, std::memory_order_relaxed);
            g_sound_play_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    pthread_mutex_unlock(&g_sound_mutex);
}

__attribute__((noinline))
static void release_event_proxy(void *actor, int32_t timing, float arg0, float arg1) {
    (void)arg0;
    (void)arg1;
    const ReleaseFn original = g_original_release_event;
    if (original) original(actor, timing, arg0, arg1);
    g_release_event_calls.fetch_add(1, std::memory_order_relaxed);
    g_last_release_timing.store(timing, std::memory_order_relaxed);
    /* The release event gets the HUR classifier's class directly; class 3 is
       the standard jumpshot's stock Excellent/special-feedback class. */
    if (timing != 3 || !g_component || !actor) return;
    const uintptr_t tracked = *reinterpret_cast<volatile uintptr_t *>(g_component + 0x08);
    if (tracked == 0 || reinterpret_cast<uintptr_t>(actor) != tracked) return;
    g_perfect_release_events.fetch_add(1, std::memory_order_relaxed);
    g_perfect_until_ns.store(monotonic_ns() + 650000000ULL, std::memory_order_release);
    play_perfect_sound();
}

static void draw_disc(GLint cx, GLint cy, GLint radius,
                      GLfloat r, GLfloat g, GLfloat b) {
    for (GLint dy = -radius; dy <= radius; ++dy) {
        const float inside = static_cast<float>(radius * radius - dy * dy);
        const GLint half = static_cast<GLint>(std::floor(std::sqrt(std::max(0.0f, inside))));
        clear_rect(cx - half, cy + dy, half * 2 + 1, 1, r, g, b);
    }
}

static float crescent_half_width(float t, float maximum) {
    const float taper = std::pow(std::max(0.0f, 1.0f - t), 0.78f);
    const float shoulder = 0.76f + 0.24f * std::sin(3.14159265359f * t);
    return maximum * taper * shoulder;
}

static void draw_crescent_pass(GLint center_x, GLint top_y, GLint height,
                               GLint bend, float maximum_half_width,
                               GLint expand, GLfloat r, GLfloat g, GLfloat b) {
    const GLint segments = std::max(48, std::min(96, height / 2));
    for (GLint i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = 3.14159265359f * t;
        const GLint x = center_x - static_cast<GLint>(std::lround(std::sin(angle) * bend));
        const GLint y = top_y - static_cast<GLint>(std::lround(t * height));
        const float half_width = crescent_half_width(t, maximum_half_width) +
                                 static_cast<float>(expand);
        if (half_width < 0.65f) continue;
        const GLint half = std::max(1, static_cast<GLint>(std::lround(half_width)));
        clear_rect(x - half, y - 2, half * 2 + 1, 4, r, g, b);
    }
}

static void draw_spark_line(GLint x0, GLint y0, GLint x1, GLint y1,
                            GLint thickness, GLfloat r, GLfloat g, GLfloat b) {
    const GLint dx = x1 - x0, dy = y1 - y0;
    const GLint steps = std::max(1, std::max(std::abs(dx), std::abs(dy)));
    for (GLint i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const GLint x = x0 + static_cast<GLint>(std::lround(dx * t));
        const GLint y = y0 + static_cast<GLint>(std::lround(dy * t));
        clear_rect(x - thickness / 2, y - thickness / 2,
                   thickness, thickness, r, g, b);
    }
}

/* Full-height tapered crescent with a green Excellent tip and a traveling
   white marker. The perfect-release flash recolors the entire inner crescent. */
static void draw_crescent_meter(GLint anchor_x, GLint anchor_y, GLint viewport_h,
                                float progress, bool perfect, uint64_t now) {
    const float scale = std::max(0.72f, std::min(1.35f, viewport_h / 720.0f));
    const GLint height = static_cast<GLint>(std::lround(160.0f * scale));
    const GLint top_y = anchor_y + static_cast<GLint>(std::lround(48.0f * scale));
    const GLint center_x = anchor_x + static_cast<GLint>(std::lround(43.0f * scale));
    const GLint bend = static_cast<GLint>(std::lround(29.0f * scale));
    const float half_width = 15.0f * scale;

    GLfloat green = 0.94f;
    if (perfect) {
        const float pulse = 0.5f + 0.5f * std::sin(
            static_cast<float>(now % 300000000ULL) * (6.28318530718f / 300000000.0f));
        green = 0.78f + 0.20f * pulse;
        draw_crescent_pass(center_x, top_y, height, bend, half_width,
                           10, 0.10f, 0.58f, 0.18f);
        draw_crescent_pass(center_x, top_y, height, bend, half_width,
                           6, 0.26f, 0.96f, 0.28f);
    }

    draw_crescent_pass(center_x, top_y, height, bend, half_width,
                       3, 0.015f, 0.02f, 0.025f);
    const GLint segments = std::max(48, std::min(96, height / 2));
    for (GLint i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = 3.14159265359f * t;
        const GLint x = center_x - static_cast<GLint>(std::lround(std::sin(angle) * bend));
        const GLint y = top_y - static_cast<GLint>(std::lround(t * height));
        const float inner_half = crescent_half_width(t, half_width) - 1.5f;
        if (inner_half < 0.65f) continue;
        const GLint half = std::max(1, static_cast<GLint>(std::lround(inner_half)));
        if (perfect) {
            clear_rect(x - half, y - 2, half * 2 + 1, 4, 0.04f, green, 0.12f);
        } else if (t <= 0.13f) {
            clear_rect(x - half, y - 2, half * 2 + 1, 4, 0.05f, 0.92f, 0.22f);
        } else {
            clear_rect(x - half, y - 2, half * 2 + 1, 4, 0.965f, 0.975f, 0.99f);
        }
    }

    const float marker_t = 1.0f - std::max(0.0f, std::min(1.0f, progress));
    const float marker_angle = 3.14159265359f * marker_t;
    const GLint marker_x = center_x -
        static_cast<GLint>(std::lround(std::sin(marker_angle) * bend));
    const GLint marker_y = top_y - static_cast<GLint>(std::lround(marker_t * height));
    draw_disc(marker_x, marker_y, static_cast<GLint>(std::lround(7.0f * scale)),
              0.015f, 0.02f, 0.025f);
    draw_disc(marker_x, marker_y, static_cast<GLint>(std::lround(4.0f * scale)),
              0.99f, 1.0f, 1.0f);

    if (perfect) {
        const GLint spark_y = top_y + static_cast<GLint>(std::lround(2.0f * scale));
        const GLint spark_x = center_x;
        const GLint ray = static_cast<GLint>(std::lround(11.0f * scale));
        const GLint gap = static_cast<GLint>(std::lround(18.0f * scale));
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.78539816339f;
            const GLint x1 = spark_x + static_cast<GLint>(std::lround(std::cos(a) * gap));
            const GLint y1 = spark_y + static_cast<GLint>(std::lround(std::sin(a) * gap));
            const GLint x2 = spark_x + static_cast<GLint>(std::lround(std::cos(a) * (gap + ray)));
            const GLint y2 = spark_y + static_cast<GLint>(std::lround(std::sin(a) * (gap + ray)));
            draw_spark_line(x1, y1, x2, y2, std::max(2, static_cast<GLint>(2.0f * scale)),
                            0.78f, 1.0f, 0.82f);
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

    const uint64_t now = monotonic_ns();
    const bool perfect = tracked != 0 &&
        now <= g_perfect_until_ns.load(std::memory_order_acquire);
    const bool meter_active = tracked && state == 2 && std::isfinite(raw);
    if (!tracked || (!meter_active && !perfect)) return;
    const float progress = std::isfinite(raw)
        ? std::max(0.0f, std::min(1.0f, raw * 2.0f)) : 0.0f;
    store_float(g_marker_bits, progress);
    if (meter_active) g_active_meter_frames.fetch_add(1, std::memory_order_relaxed);
    if (perfect) g_perfect_flash_frames.fetch_add(1, std::memory_order_relaxed);
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
    draw_crescent_meter(ax, ay, vp[3], progress, perfect, now);

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
    else if (g_release_event_code != 0 || !g_original_release_event)
        status = "RELEASE_EVENT_HOOK_FAILED";
    else if (g_draw_calls.load()) status = "CRESCENT_METER_DRAWN";
    else if (g_active_meter_frames.load()) status = "METER_ACTIVE_WAITING_FOR_RENDER";
    else if (g_anchor_valid.load()) status = "PLAYER_ANCHOR_READY_WAITING_FOR_SHOT";
    else if (g_swap_calls.load()) status = "WAITING_FOR_VALID_PLAYER_PROJECTION";
    else status = "HOOKS_READY_WAITING_FOR_FRAMES";

    const bool perfect_active = monotonic_ns() <= g_perfect_until_ns.load();
    char body[3072];
    (void)snprintf(body, sizeof(body),
        "agent=v47\npurpose=player_anchored_crescent_meter_perfect_feedback\nengine=Dobby\n"
        "engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7\n"
        "target_library=%s\ncomponent_symbol=%s\ncomponent_address=%p\n"
        "do_draw_address=%p\ndo_draw_hook_code=%d\nhead_location_address=%p\n"
        "saved_view_address=%p\nproject_address=%p\negl_swap_buffers_address=%p\n"
        "egl_swap_buffers_hook_code=%d\nrelease_event_address=%p\nrelease_event_hook_code=%d\n"
        "self_test_status=%s\nself_test_hook_code=%d\nself_test_baseline=%d\n"
        "self_test_original=%d\nself_test_patched=%d\nattach_calls=%u\n"
        "duplicate_attach_calls=%u\ninitialization_state=%u\ndo_draw_callbacks=%u\n"
        "projection_calls=%u\nprojection_valid=%u\nlast_anchor_x=%.8g\n"
        "last_anchor_y=%.8g\nlast_anchor_z=%.8g\nlast_reciprocal_clip_w=%.8g\n"
        "anchor_valid=%u\negl_swap_callbacks=%u\nactive_meter_frames=%u\n"
        "overlay_draw_calls=%u\nlast_component_state=%d\nlast_tracked_player_present=%u\n"
        "last_meter_value=%.7g\nlast_meter_position=%.7g\nrelease_event_callbacks=%u\n"
        "last_release_timing_class=%d\nperfect_class3_release_events=%u\n"
        "perfect_flash_active=%u\nperfect_flash_frames=%u\nsound_init_code=%d\n"
        "perfect_sound_play_count=%u\nsurface_width=%d\nsurface_height=%d\n"
        "viewport_width=%d\nviewport_height=%d\nframebuffer_binding=%d\n"
        "saved_view_copy_only=1\ngame_owned_view_bytes_written=0\n"
        "game_object_fields_modified=0\ncomponent_memory_modified=0\n"
        "original_meter_resource_modified=0\nshot_timing_modified=0\n"
        "original_library_file_modified=0\nstatus=%s\n",
        g_game_name, kComponentSymbol, (const void *)g_component,
        g_do_draw_address, g_do_draw_code, g_head_address, (void *)g_saved_view,
        g_project_address, g_swap_address, g_swap_code, g_release_event_address,
        g_release_event_code, self_ok ? "PASS" : "FAIL", g_self_code, g_self_base,
        g_self_original, g_self_patched, g_attach_calls.load(),
        g_duplicate_attach_calls.load(), g_init_state.load(), g_do_draw_calls.load(),
        g_projection_calls.load(), g_projection_valid.load(),
        load_float(g_anchor_x_bits), load_float(g_anchor_y_bits),
        load_float(g_anchor_z_bits), load_float(g_clip_w_bits), g_anchor_valid.load(),
        g_swap_calls.load(), g_active_meter_frames.load(), g_draw_calls.load(),
        g_component_state.load(), g_tracked_player.load(), load_float(g_meter_bits),
        load_float(g_marker_bits), g_release_event_calls.load(),
        g_last_release_timing.load(), g_perfect_release_events.load(),
        perfect_active ? 1U : 0U, g_perfect_flash_frames.load(),
        g_sound_init_code.load(), g_sound_play_count.load(),
        g_surface_w.load(), g_surface_h.load(), g_viewport_w.load(),
        g_viewport_h.load(), g_framebuffer.load(), status);
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
        g_release_event_address = resolve(g_game, kReleaseEventSymbol);
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
        if (g_release_event_address) {
            void *original_release = nullptr;
            g_release_event_code = DobbyHook(g_release_event_address,
                (void *)&release_event_proxy, &original_release);
            g_original_release_event = reinterpret_cast<ReleaseFn>(original_release);
        } else g_release_event_code = -1004;
    } else {
        g_do_draw_code = -1001;
        g_swap_code = -1001;
    }

    const bool sound_ready = initialize_sound();
    LOGI("v47 perfect-shot sound init=%s code=%d", sound_ready ? "ready" : "unavailable", g_sound_init_code.load());
    g_init_state.store(2, std::memory_order_release);
    write_diagnostic();
    pthread_t thread;
    if (pthread_create(&thread, nullptr, reporter, nullptr) == 0) pthread_detach(thread);
    else LOGE("diagnostic reporter thread creation failed");
    LOGI("v47 crescent meter attached; class-3 player release triggers green flash and optional tone");
    return JNI_OK;
}
extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *) {
    LOGI("v47 crescent meter unloading");
}

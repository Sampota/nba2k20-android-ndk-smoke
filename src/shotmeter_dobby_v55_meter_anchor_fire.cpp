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
#include <string>
#include <time.h>

#include "dobby.h"
#include "greengiant_ima_b64.h"
#include "../Files/horn_font_atlas.h"

#define LOG_TAG "ShotmeterDobbyV54"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAG_NAME "shotmeter.dobby-v55-meter-anchor-fire-fix"

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
static const char kBalancedDiceSymbol[] = "_Z30PHY_LaunchShotBalancedDiceRollP8AI_ACTORP7AI_BALLif5_bool19PHY_SHOT_TRAJECTORY23MVS_SHOT_RELEASE_TIMING";
static const char kGetLocationSymbol[] = "_ZNK8AI_ACTOR11GetLocationEPDv4_f";
static const char kDistanceSymbol[] = "_Z32MTH_GetDistanceFromPointToBasketPKDv4_f";
static const char kGradeSymbol[] = "_Z40JumpshotFeedback_TranslatePercentToGradeff";

using DoDrawFn = void (*)(void *, void *);
using HeadFn = void (*)(void *, float *);
using ProjectFn = float (*)(const void *, const float *, float *);
using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using TestFn = int (*)(int);
using ReleaseFn = void (*)(void *, int32_t, float, float);
using BalancedDiceFn = void (*)(void *, void *, int32_t, float, bool, int32_t, int32_t);
using GetLocationFn = void (*)(const void *, float *);
using DistanceFn = float (*)(const float *);
using GradeFn = int32_t (*)(float, float);

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
static void *g_balanced_dice_address = nullptr;
static void *g_get_location_address = nullptr;
static void *g_distance_address = nullptr;
static void *g_grade_address = nullptr;
static BalancedDiceFn g_original_balanced_dice = nullptr;
static GetLocationFn g_get_location = nullptr;
static DistanceFn g_get_distance = nullptr;
static GradeFn g_translate_grade = nullptr;
static int g_probability_hook_code = -999;
static std::atomic<uint32_t> g_probability_enabled{0};
static std::atomic<uint32_t> g_probability_hook_calls{0}, g_probability_modified_shots{0};
static std::atomic<uint32_t> g_probability_invalid_context{0};
static std::atomic<uint32_t> g_probability_last_original_bits{0}, g_probability_last_applied_bits{0};
static std::atomic<uint32_t> g_probability_last_distance_bits{0};
static std::atomic<int32_t> g_probability_last_grade{-1}, g_probability_last_timing{-1};

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
static std::atomic<uint64_t> g_result_until_ns{0};
static std::atomic<uint64_t> g_result_started_ns{0};
static std::atomic<uint32_t> g_banner_draw_calls{0};
static std::atomic<uint32_t> g_result_sequence{0};
static std::atomic<int32_t> g_result_timing{-1};
static std::atomic<uint32_t> g_result_raw_bits{0};
static std::atomic<uint32_t> g_anchor_valid{0};
static std::atomic<uint32_t> g_anchor_x_bits{0}, g_anchor_y_bits{0};
static std::atomic<uint32_t> g_anchor_z_bits{0}, g_clip_w_bits{0};
static std::atomic<uint32_t> g_meter_bits{0}, g_marker_bits{0};
static std::atomic<uint32_t> g_component_animation_bits{0}, g_component_release_value_bits{0};
static std::atomic<int32_t> g_component_timing_enum{-1};
static std::atomic<uint32_t> g_release_raw_bits{0}, g_release_animation_bits{0};
static std::atomic<uint32_t> g_release_value_bits{0}, g_release_arg0_bits{0}, g_release_arg1_bits{0};
static std::atomic<uint32_t> g_release_position_bits{0}, g_perfect_raw_reference_bits{0};
static std::atomic<int32_t> g_release_pre_state{-1}, g_release_pre_timing_enum{-1};
static std::atomic<uint32_t> g_eligible_local_release_events{0};
static std::atomic<int32_t> g_component_state{-1};
static std::atomic<uint32_t> g_tracked_player{0};
static std::atomic<int32_t> g_surface_w{0}, g_surface_h{0};
static std::atomic<int32_t> g_viewport_w{0}, g_viewport_h{0};
static std::atomic<int32_t> g_framebuffer{-1};
static std::atomic<uint32_t> g_sound_play_count{0};
static std::atomic<int32_t> g_sound_init_code{-999};
static std::atomic<int32_t> g_shader_init_code{-999};
static std::atomic<int32_t> g_gl_init_stage{0}, g_gl_error_code{-1};
static std::atomic<int32_t> g_gl_last_error_stage{-1};
static std::atomic<uint32_t> g_gl_error_count{0}, g_gl_draw_error_count{0};
static std::atomic<uint32_t> g_gl_pre_overlay_error_count{0};
static std::atomic<uint32_t> g_perfect_fire_trigger_count{0}, g_perfect_fire_draw_calls{0};
static std::atomic<uint32_t> g_perfect_fire_frames{0}, g_perfect_fire_active{0};
static std::atomic<int32_t> g_gl_vertex_status{-1}, g_gl_fragment_status{-1}, g_gl_link_status{-1};
static std::atomic<int32_t> g_gl_draw_error_code{-1};
static std::atomic<uint32_t> g_gl_init_attempts{0};
static std::atomic<uintptr_t> g_egl_context_value{0}, g_egl_display_value{0}, g_egl_surface_value{0};
static std::atomic<uint32_t> g_gl_identity_captured{0};
static pthread_mutex_t g_gl_diag_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_gl_version[160] = {}, g_gl_renderer[160] = {}, g_gl_vendor[160] = {};
static char g_glsl_version[160] = {}, g_gl_shader_log[768] = {};
static std::atomic<uint64_t> g_last_sound_ns{0};
static pthread_mutex_t g_sound_mutex = PTHREAD_MUTEX_INITIALIZER;

static SLObjectItf g_sl_engine_object = nullptr;
static SLEngineItf g_sl_engine = nullptr;
static SLObjectItf g_sl_output_mix = nullptr;
static SLObjectItf g_sl_player_object = nullptr;
static SLPlayItf g_sl_play = nullptr;
static SLAndroidSimpleBufferQueueItf g_sl_queue = nullptr;
static int16_t g_green_giant_pcm[kGreenGiantSamples]{};
static std::atomic<uint32_t> g_sound_requests{0};

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

/* JUMPSHOT_GRADE is an ascending 0..12 enum: F through A+. The chosen
   values stay inside the requested probability bands for each letter grade. */
static constexpr float kChanceByGrade[13] = {
    0.000f, 0.005f, 0.010f, 0.020f,
    0.025f, 0.035f, 0.050f,
    0.100f, 0.175f, 0.250f,
    0.350f, 0.510f, 0.670f
};
static constexpr float kExcellentChance = 0.970f;

static bool map_timing_probability(int32_t timing, int32_t grade, float *chance) {
    if (!chance) return false;
    switch (timing) {
        case 1: // Very Early
        case 5: // Very Late
            *chance = 0.0f;
            return true;
        case 2: // Slight Early
        case 4: // Slight Late
            if (grade < 0 || grade >= 13) return false;
            *chance = kChanceByGrade[grade];
            return true;
        case 3: // Excellent
            *chance = kExcellentChance;
            return true;
        default:
            return false;
    }
}

static bool probability_mapping_self_test(uint32_t *case_count) {
    uint32_t checked = 0;
    for (int32_t grade = 0; grade < 13; ++grade) {
        for (int32_t timing = 1; timing <= 5; ++timing) {
            float actual = -1.0f;
            if (!map_timing_probability(timing, grade, &actual)) return false;
            const float expected = (timing == 1 || timing == 5) ? 0.0f :
                (timing == 3 ? kExcellentChance : kChanceByGrade[grade]);
            if (std::fabs(actual - expected) > 0.000001f) return false;
            ++checked;
        }
    }
    float ignored = -1.0f;
    if (map_timing_probability(2, -1, &ignored)) return false;
    ++checked;
    if (map_timing_probability(4, 13, &ignored)) return false;
    ++checked;
    if (map_timing_probability(0, 0, &ignored)) return false;
    ++checked;
    if (map_timing_probability(6, 12, &ignored)) return false;
    ++checked;
    if (case_count) *case_count = checked;
    return checked == 69;
}

static std::atomic<uint32_t> g_probability_map_self_test_pass{0};
static std::atomic<uint32_t> g_probability_map_self_test_cases{0};

__attribute__((noinline))
static void balanced_dice_proxy(void *actor, void *ball, int32_t shot_type,
                                float original_chance, bool bank_shot,
                                int32_t trajectory, int32_t timing) {
    float applied_chance = original_chance;
    float distance = NAN;
    int32_t grade = -1;
    bool modified = false;

    if (g_probability_enabled.load(std::memory_order_acquire) != 0 &&
        timing >= 1 && timing <= 5) {
        /* At PHY_LaunchShotBalancedDiceRoll, s0 is the exact chance entering
           simulation and w5 is the native release-timing enum. The chance
           stored by the game for HUD feedback is never written here. */
        if ((timing == 2 || timing == 4) && actor && g_get_location &&
            g_get_distance && g_translate_grade && std::isfinite(original_chance) &&
            original_chance >= 0.0f && original_chance <= 1.0f) {
            alignas(16) float location[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            g_get_location(actor, location);
            distance = g_get_distance(location);
            if (std::isfinite(distance) && distance >= 0.0f)
                grade = g_translate_grade(original_chance, distance);
        }

        modified = map_timing_probability(timing, grade, &applied_chance);
        g_probability_hook_calls.fetch_add(1, std::memory_order_relaxed);
        if (modified)
            g_probability_modified_shots.fetch_add(1, std::memory_order_relaxed);
        else
            g_probability_invalid_context.fetch_add(1, std::memory_order_relaxed);

        store_float(g_probability_last_original_bits, original_chance);
        store_float(g_probability_last_applied_bits, applied_chance);
        store_float(g_probability_last_distance_bits, distance);
        g_probability_last_grade.store(grade, std::memory_order_relaxed);
        g_probability_last_timing.store(timing, std::memory_order_relaxed);
    }

    const BalancedDiceFn original = g_original_balanced_dice;
    if (original)
        original(actor, ball, shot_type, applied_chance, bank_shot, trajectory, timing);
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

static constexpr float kHornAngBot = 1.1135f;
static constexpr float kHornAngTop = -0.9931f;
static constexpr float kHornWinLo = 0.900f;
static constexpr float kPerfectRawInitial = 0.25f; // measured v48 class-3 release baseline; refined from subsequent verified class-3 shots.
static constexpr float kPerfectFillTarget = 0.945f; // inside the reduced Excellent cap zone.
static constexpr uint64_t kBannerDurationNs = 1300000000ULL;
static constexpr float kBannerDurationSec = 1.30f;
static const float kBannerTint[5][3] = {
    {0.90f, 0.15f, 0.15f},
    {1.00f, 1.00f, 1.00f},
    {0.21f, 0.82f, 0.23f},
    {1.00f, 0.85f, 0.10f},
    {0.90f, 0.15f, 0.15f}
};

static const char kHornVS[] = R"(
attribute vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

static const char kHornFS[] = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform vec2 uOrigin;
uniform vec2 uDirFill, uDirCap;
uniform vec3 uTint, uLabelTint;
uniform float uScale, uFill, uFlash, uAlpha, uFire, uTime;
uniform vec2 uLabelSize;
uniform sampler2D uLabelTex;

const vec2 OUT_C = vec2(50.0, 143.0); const float OUT_R = 136.0;
const vec2 IN_C = vec2(-3.0, 170.0); const float IN_R = 147.0;
const vec2 CUT_P = vec2(98.0, 67.0);
const vec2 CUT_N = vec2(-0.823, -0.57);
float W;
float cov(float x) { return 1.0 - smoothstep(-W, W, x); }
float dOuter(vec2 p) { return length(p - OUT_C) - OUT_R; }
float dInner(vec2 p) { return IN_R - length(p - IN_C); }
float dCut(vec2 p) { return dot(p - CUT_P, CUT_N); }
float horn(vec2 p) { return max(max(dOuter(p), dInner(p)), dCut(p)); }
float roundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - (b - vec2(r));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    W = 0.8 / uScale;
    vec2 p = (gl_FragCoord.xy - uOrigin) / uScale;
    p.y = 319.0 - p.y;

    float labelW = uLabelSize.x + 12.0;
    float labelLeft = 247.0 - labelW - 5.0;
    vec2 labelCenter = vec2(labelLeft + labelW * 0.5, 18.0);
    float labelD = roundBox(p - labelCenter, vec2(labelW * 0.5, 10.0), 2.5);
    float chip = cov(labelD) * step(0.5, uLabelSize.x);
    float d = horn(p);
    if (d > 20.0 && chip < 0.01) { gl_FragColor = vec4(0.0); return; }

    float a = cov(d);
    float body = cov(d + 3.0);
    float sh = 0.0;
#if HORN_SHADOW
    sh = (1.0 - smoothstep(-4.0, 12.0, horn(p - vec2(4.0, 6.0)))) * 0.35;
#endif
    vec3 ink = vec3(0.04);
    vec3 col = mix(ink, mix(vec3(0.20), uTint, uFlash), body);

    vec2 v = p - OUT_C;
    float fm = cov(-(uDirFill.x * v.y - uDirFill.y * v.x));
    col = mix(col, mix(vec3(1.0), uTint, uFlash), fm * body * step(0.001, uFill));

    float cap = cov(uDirCap.x * v.y - uDirCap.y * v.x) * body;
    col = mix(col, mix(vec3(0.25, 0.90, 0.20), uTint, uFlash), cap);
    float whiteRim = max(0.0, a - cov(d + 1.6));
    col = mix(col, vec3(0.96, 0.98, 1.0), whiteRim * 0.92);

    float baseA = (a + sh * (1.0 - a)) * uAlpha;
    vec3 rgb = col * baseA;
    float alpha = baseA;

    float border = max(0.0, chip - cov(labelD + 1.25) * step(0.5, uLabelSize.x));
    vec3 bg = vec3(0.014, 0.018, 0.022);
    rgb = mix(rgb, bg * chip, chip);
    alpha = alpha + chip * 0.96 * (1.0 - alpha);
    rgb = mix(rgb, uLabelTint * alpha, border);

    vec2 tp = p - vec2(labelLeft + 6.0, 10.0);
    float inText = step(0.0, tp.x) * step(tp.x, uLabelSize.x) *
                   step(0.0, tp.y) * step(tp.y, 16.0);
    vec2 tuv = vec2(tp.x / max(1.0, uLabelSize.x), tp.y / 16.0);
    float textA = texture2D(uLabelTex, tuv).a * inText * step(0.5, uLabelSize.x);
    rgb = mix(rgb, uLabelTint * alpha, textA);
    alpha = alpha + textA * (1.0 - alpha);

    // Procedural green flame anchored just above the projected player head.
    // The layered sine fields create upward flicker and independently moving tongues.
    vec2 fq = vec2((p.x - 80.5) / 24.0, (48.0 - p.y) / 80.0);
    float fy = clamp(fq.y, 0.0, 1.0);
    float flicker = 0.10 * sin(uTime * 17.0 + fq.y * 11.0)
                  + 0.06 * sin(uTime * 23.0 - fq.y * 18.0)
                  + 0.035 * sin(uTime * 31.0 + fq.x * 13.0);
    float flameWidth = (0.40 * (1.0 - fy) + 0.045) * (1.0 + flicker);
    float mainFlame = 1.0 - smoothstep(flameWidth - 0.07, flameWidth + 0.07, abs(fq.x));
    float tongueA = 1.0 - smoothstep(0.12, 0.25, abs(fq.x - 0.22 * sin(uTime * 13.0 + fq.y * 8.0)));
    float tongueB = 1.0 - smoothstep(0.10, 0.22, abs(fq.x + 0.24 * sin(uTime * 11.0 - fq.y * 9.0)));
    float flameHeight = smoothstep(0.0, 0.07, fq.y) *
        (1.0 - smoothstep(0.88 + 0.06 * sin(uTime * 9.0), 1.08, fq.y));
    float flame = clamp(max(mainFlame, max(tongueA * 0.48, tongueB * 0.42)) *
                        flameHeight * uFire, 0.0, 1.0);
    float fireCore = clamp(1.0 - abs(fq.x) / max(0.08, flameWidth * 0.55), 0.0, 1.0);
    vec3 fireCol = mix(vec3(0.02, 0.38, 0.035), vec3(0.32, 1.0, 0.055), fireCore);
    fireCol = mix(fireCol, vec3(0.78, 1.0, 0.28), fireCore * 0.45);
    float headFlash = uFire * exp(-uTime * 11.0) *
        exp(-length(vec2(fq.x * 1.5, (fq.y - 0.08) * 1.8)) * 5.0) * 0.85;
    float fireAlpha = clamp(flame * 0.98 + headFlash * 0.70, 0.0, 1.0);
    vec3 fireRgb = fireCol * flame * 0.98 + vec3(0.25, 1.0, 0.12) * headFlash * 0.70;
    rgb = fireRgb + rgb * (1.0 - fireAlpha);
    alpha = fireAlpha + alpha * (1.0 - fireAlpha);

    gl_FragColor = vec4(rgb, alpha);
}
)";

static const char kBannerFS[] = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform vec2  uOrigin;            // bottom-left pixel of the banner box
uniform float uScale;             // pixels per design unit
uniform float uAlpha;             // overall fade 0..1
uniform float uPop;               // pop-in scale (about 0.6 .. 1.04 .. 1.0)
uniform float uSdfK;              // converts atlas alpha to design-unit distance
uniform vec3  uTint;              // result color
uniform sampler2D uAtlas;         // signed-distance text atlas (alpha channel)
uniform vec2  uTextOrigin;        // design position of the text cell's top-left corner
uniform vec2  uCellSize;          // design size of the text cell
uniform vec2  uTexV;              // v start and v height of the text row in the atlas

const vec2  C    = vec2(196.0, 54.0);    // panel center
const vec2  HS   = vec2(150.0, 36.0);    // panel half size before slanting
const float SK   = 0.36;                 // slant: sideways shift per unit of height
const float INV  = 0.9409;               // 1 / sqrt(1 + SK*SK), makes slanted edges true distances
const vec2  NEON = vec2(14.0, 14.0);     // neon outline offset (down-right of the panel)

float W;
float cov(float x) { return 1.0 - smoothstep(-W, W, x); }
// signed distance to the slanted panel centered at c (negative inside)
float para(vec2 p, vec2 c) {
    vec2 q = p - c;
    q.x += SK * q.y;                      // un-slant
    return max((abs(q.x) - HS.x) * INV, abs(q.y) - HS.y);
}

void main() {
    W = 0.8 / uScale;
    vec2 p = (gl_FragCoord.xy - uOrigin) / uScale;
    p.y = 130.0 - p.y;
    p = C + (p - C) / uPop;               // pop-in scale around the panel center

    float dP = para(p, C);                // panel
    float dN = para(p, C + NEON);         // neon outline shape
    if (dP > 24.0 && dN > 24.0) { gl_FragColor = vec4(0.0); return; }   // early exit

    // 1) neon outline with glow (behind the panel)
    vec3  neonCol = mix(uTint, vec3(1.0), 0.18);
    float edge = abs(dN) - 1.7;
    float line = cov(edge);
    float glow = exp(-max(edge, 0.0) / 5.0) * 0.55;
    float na   = clamp(line + glow * (1.0 - line), 0.0, 1.0);
    vec4  res  = vec4(neonCol * na, na);

    // 2) dark translucent panel with a darker, more opaque border
    float pc     = cov(dP);
    float border = pc * (1.0 - cov(dP + 3.5));
    vec3  pcol   = mix(uTint * 0.16, uTint * 0.48, border);
    float pa     = pc * mix(0.88, 1.0, border);
    res *= (1.0 - pc);                    // the neon never shows through the panel
    res  = vec4(pcol * pa, pa) + res * (1.0 - pa);

    // 3) text from the signed-distance atlas: glow first, then the letters
    vec2  n = (p - uTextOrigin) / uCellSize;
    float inCell = step(0.0, n.x) * step(n.x, 1.0) * step(0.0, n.y) * step(n.y, 1.0);
    float a = texture2D(uAtlas, vec2(n.x, uTexV.x + n.y * uTexV.y)).a;
    float dist = mix(-100.0, (a - 0.50196) * uSdfK, inCell);   // + inside a letter, - outside
    float tc    = smoothstep(-W, W, dist);                    // letter coverage
    float glowT = exp(min(dist, 0.0) / 4.5) * 0.7 * (1.0 - tc) * pc;
    vec3  gcol  = mix(uTint, vec3(1.0), 0.15);
    vec3  tcol  = mix(uTint, vec3(1.0), 0.40);
    res = vec4(gcol * glowT, glowT) + res * (1.0 - glowT);
    res = vec4(tcol * tc, tc) + res * (1.0 - tc);

    gl_FragColor = res * uAlpha;          // premultiplied alpha output
}
)";
static struct {
    EGLContext ctx = EGL_NO_CONTEXT;
    GLuint prog = 0, bannerProg = 0, vbo = 0, vao = 0, labelTex = 0, bannerTex = 0;
    GLint uOrigin = -1, uScale = -1, uFill = -1, uFlash = -1, uAlpha = -1;
    GLint uDirFill = -1, uDirCap = -1, uTint = -1, uLabelTint = -1;
    GLint uLabelSize = -1, uLabelTex = -1, uFire = -1, uTime = -1;
    GLint bOrigin = -1, bScale = -1, bAlpha = -1, bPop = -1, bSdfK = -1;
    GLint bTint = -1, bAtlas = -1, bTextOrigin = -1, bCellSize = -1, bTexV = -1;
    bool ok = false, es3 = false, bannerOk = false;
    int labelTiming = -99;
    float labelWidth = 0.0f;
} G;

static void set_gl_string(char *dst, size_t cap, const GLubyte *value) {
    pthread_mutex_lock(&g_gl_diag_mutex);
    snprintf(dst, cap, "%s", value ? reinterpret_cast<const char *>(value) : "(null)");
    pthread_mutex_unlock(&g_gl_diag_mutex);
}
static void set_shader_log(const char *value) {
    pthread_mutex_lock(&g_gl_diag_mutex);
    snprintf(g_gl_shader_log, sizeof(g_gl_shader_log), "%s", value ? value : "");
    for (char *p = g_gl_shader_log; *p; ++p) if (*p == '\n' || *p == '\r') *p = '|';
    pthread_mutex_unlock(&g_gl_diag_mutex);
}
static GLenum capture_gl_error(int stage) {
    const GLenum error = glGetError();
    if (stage == 170 && error != GL_NO_ERROR)
        g_gl_pre_overlay_error_count.fetch_add(1, std::memory_order_relaxed);
    if (error != GL_NO_ERROR) {
        g_gl_error_code.store(static_cast<int32_t>(error), std::memory_order_relaxed);
        g_gl_last_error_stage.store(stage, std::memory_order_relaxed);
        g_gl_init_stage.store(stage, std::memory_order_relaxed);
        g_gl_error_count.fetch_add(1, std::memory_order_relaxed);
        LOGE("OpenGL error 0x%x at stage %d", static_cast<unsigned>(error), stage);
    }
    return error;
}
static void replace_all(std::string &value, const char *from, const char *to) {
    size_t pos = 0;
    const size_t fromLen = strlen(from);
    const size_t toLen = strlen(to);
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, fromLen, to);
        pos += toLen;
    }
}
static GLuint compile_horn_shader(GLenum type, const char *source, int statusCode) {
    g_gl_init_stage.store(statusCode, std::memory_order_relaxed);
    GLuint shader = glCreateShader(type);
    (void)capture_gl_error(statusCode);
    if (!shader) {
        set_shader_log("glCreateShader returned 0");
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    (void)capture_gl_error(statusCode + 1);
    if (type == GL_VERTEX_SHADER) g_gl_vertex_status.store(ok == GL_TRUE ? 1 : 0);
    else g_gl_fragment_status.store(ok == GL_TRUE ? 1 : 0);
    if (ok != GL_TRUE) {
        char log[1024] = {};
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(log), &len, log);
        set_shader_log(log[0] ? log : "shader compile failed without an info log");
        LOGE("horn shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}


static bool init_banner_gl(void) {
    GLint oldProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);

    std::string vsSource;
    std::string fsSource;
    if (G.es3) {
        vsSource =
            "#version 300 es\n"
            "precision highp float;\n"
            "in vec2 aPos;\n"
            "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
        fsSource = "#version 300 es\n";
        fsSource += kBannerFS;
        replace_all(fsSource, "texture2D(", "texture(");
        replace_all(fsSource, "gl_FragColor", "fragColor");
        const size_t precisionEnd = fsSource.find("\n#endif\n");
        if (precisionEnd == std::string::npos) {
            set_shader_log("banner shader precision block conversion failed");
            return false;
        }
        fsSource.insert(precisionEnd + 8, "\nout vec4 fragColor;\n");
    } else {
        vsSource = kHornVS;
        fsSource = kBannerFS;
    }

    const char *vsPtr = vsSource.c_str();
    const char *fsPtr = fsSource.c_str();
    GLuint vs = compile_horn_shader(GL_VERTEX_SHADER, vsPtr, 50);
    GLuint fs = compile_horn_shader(GL_FRAGMENT_SHADER, fsPtr, 60);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        glUseProgram(oldProgram);
        return false;
    }

    GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        glUseProgram(oldProgram);
        set_shader_log("banner glCreateProgram returned 0");
        return false;
    }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "aPos");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024] = {};
        GLsizei len = 0;
        glGetProgramInfoLog(program, sizeof(log), &len, log);
        set_shader_log(log[0] ? log : "banner program link failed");
        glDeleteProgram(program);
        glUseProgram(oldProgram);
        return false;
    }

    G.bannerProg = program;
    G.bOrigin = glGetUniformLocation(program, "uOrigin");
    G.bScale = glGetUniformLocation(program, "uScale");
    G.bAlpha = glGetUniformLocation(program, "uAlpha");
    G.bPop = glGetUniformLocation(program, "uPop");
    G.bSdfK = glGetUniformLocation(program, "uSdfK");
    G.bTint = glGetUniformLocation(program, "uTint");
    G.bAtlas = glGetUniformLocation(program, "uAtlas");
    G.bTextOrigin = glGetUniformLocation(program, "uTextOrigin");
    G.bCellSize = glGetUniformLocation(program, "uCellSize");
    G.bTexV = glGetUniformLocation(program, "uTexV");
    if (G.bOrigin < 0 || G.bScale < 0 || G.bAlpha < 0 || G.bPop < 0 ||
        G.bSdfK < 0 || G.bTint < 0 || G.bAtlas < 0 || G.bTextOrigin < 0 ||
        G.bCellSize < 0 || G.bTexV < 0) {
        set_shader_log("one or more banner uniforms were unavailable");
        glDeleteProgram(G.bannerProg);
        G.bannerProg = 0;
        glUseProgram(oldProgram);
        return false;
    }

    GLint oldActive = GL_TEXTURE0, oldTexture = 0, oldUnpack = 4;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpack);
    static uint8_t rgbaAtlas[HORN_ATLAS_W * HORN_ATLAS_H * 4] = {};
    for (size_t i = 0; i < static_cast<size_t>(HORN_ATLAS_W * HORN_ATLAS_H); ++i) {
        rgbaAtlas[i * 4 + 0] = 255;
        rgbaAtlas[i * 4 + 1] = 255;
        rgbaAtlas[i * 4 + 2] = 255;
        rgbaAtlas[i * 4 + 3] = kHornAtlas[i];
    }
    glGenTextures(1, &G.bannerTex);
    if (G.bannerTex) {
        glBindTexture(GL_TEXTURE_2D, G.bannerTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, HORN_ATLAS_W, HORN_ATLAS_H,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaAtlas);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldUnpack);
    glBindTexture(GL_TEXTURE_2D, oldTexture);
    glActiveTexture(oldActive);
    if (!G.bannerTex || glGetError() != GL_NO_ERROR) {
        if (G.bannerTex) glDeleteTextures(1, &G.bannerTex);
        G.bannerTex = 0;
        glDeleteProgram(G.bannerProg);
        G.bannerProg = 0;
        glUseProgram(oldProgram);
        set_shader_log("banner atlas texture initialization failed");
        return false;
    }

    glUseProgram(G.bannerProg);
    glUniform1i(G.bAtlas, 0);
    glUseProgram(oldProgram);
    G.bannerOk = true;
    return true;
}

static int init_horn_gl(void) {
    g_gl_init_stage.store(1, std::memory_order_relaxed);
    const EGLContext current = eglGetCurrentContext();
    if (current == EGL_NO_CONTEXT) {
        set_shader_log("no current EGL context");
        return -100;
    }
    g_egl_context_value.store(reinterpret_cast<uintptr_t>(current), std::memory_order_relaxed);
    const GLubyte *version = glGetString(GL_VERSION);
    set_gl_string(g_gl_version, sizeof(g_gl_version), version);
    set_gl_string(g_gl_vendor, sizeof(g_gl_vendor), glGetString(GL_VENDOR));
    set_gl_string(g_gl_renderer, sizeof(g_gl_renderer), glGetString(GL_RENDERER));
    set_gl_string(g_glsl_version, sizeof(g_glsl_version), glGetString(GL_SHADING_LANGUAGE_VERSION));
    if (!version) {
        (void)capture_gl_error(2);
        set_shader_log("glGetString(GL_VERSION) returned null");
        return -101;
    }
    G.es3 = strstr(reinterpret_cast<const char *>(version), "OpenGL ES 3") != nullptr;
    (void)capture_gl_error(3);

    GLint oldProgram = 0, oldBuffer = 0, oldVao = 0, oldActive = GL_TEXTURE0, oldTexture = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    if (G.es3) glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    (void)capture_gl_error(4);

    std::string vsSource;
    std::string fsSource;
    if (G.es3) {
        vsSource =
            "#version 300 es\n"
            "precision highp float;\n"
            "in vec2 aPos;\n"
            "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
        fsSource = "#version 300 es\n#define HORN_SHADOW 0\n";
        fsSource += kHornFS;
        replace_all(fsSource, "texture2D(", "texture(");
        replace_all(fsSource, "gl_FragColor", "fragColor");
        const size_t precisionEnd = fsSource.find("\n#endif\n");
        if (precisionEnd == std::string::npos) {
            set_shader_log("could not locate fragment precision block for ES3 conversion");
            return -102;
        }
        fsSource.insert(precisionEnd + 8, "\nout vec4 fragColor;\n");
    } else {
        vsSource = kHornVS;
        fsSource = "#define HORN_SHADOW 0\n";
        fsSource += kHornFS;
    }

    const char *vsPtr = vsSource.c_str();
    const char *fsPtr = fsSource.c_str();
    GLuint vs = compile_horn_shader(GL_VERTEX_SHADER, vsPtr, 10);
    GLuint fs = compile_horn_shader(GL_FRAGMENT_SHADER, fsPtr, 20);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        glBindTexture(GL_TEXTURE_2D, oldTexture);
        glActiveTexture(oldActive);
        glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
        glUseProgram(oldProgram);
        (void)capture_gl_error(29);
        return !vs ? -110 : -120;
    }

    g_gl_init_stage.store(30, std::memory_order_relaxed);
    G.prog = glCreateProgram();
    (void)capture_gl_error(30);
    if (!G.prog) {
        glDeleteShader(vs); glDeleteShader(fs);
        set_shader_log("glCreateProgram returned 0");
        glBindTexture(GL_TEXTURE_2D, oldTexture);
        glActiveTexture(oldActive);
        glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
        glUseProgram(oldProgram);
        return -130;
    }
    glAttachShader(G.prog, vs);
    glAttachShader(G.prog, fs);
    glBindAttribLocation(G.prog, 0, "aPos");
    glLinkProgram(G.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(G.prog, GL_LINK_STATUS, &linked);
    g_gl_link_status.store(linked == GL_TRUE ? 1 : 0);
    (void)capture_gl_error(31);
    if (linked != GL_TRUE) {
        char log[1024] = {};
        GLsizei len = 0;
        glGetProgramInfoLog(G.prog, sizeof(log), &len, log);
        set_shader_log(log[0] ? log : "program link failed without an info log");
        LOGE("horn program link failed: %s", log);
        glDeleteProgram(G.prog); G.prog = 0;
        glBindTexture(GL_TEXTURE_2D, oldTexture);
        glActiveTexture(oldActive);
        glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
        glUseProgram(oldProgram);
        (void)capture_gl_error(32);
        return -131;
    }

    G.uOrigin = glGetUniformLocation(G.prog, "uOrigin");
    G.uScale = glGetUniformLocation(G.prog, "uScale");
    G.uFill = glGetUniformLocation(G.prog, "uFill");
    G.uFlash = glGetUniformLocation(G.prog, "uFlash");
    G.uAlpha = glGetUniformLocation(G.prog, "uAlpha");
    G.uDirFill = glGetUniformLocation(G.prog, "uDirFill");
    G.uDirCap = glGetUniformLocation(G.prog, "uDirCap");
    G.uTint = glGetUniformLocation(G.prog, "uTint");
    G.uLabelTint = glGetUniformLocation(G.prog, "uLabelTint");
    G.uLabelSize = glGetUniformLocation(G.prog, "uLabelSize");
    G.uLabelTex = glGetUniformLocation(G.prog, "uLabelTex");
    G.uFire = glGetUniformLocation(G.prog, "uFire");
    G.uTime = glGetUniformLocation(G.prog, "uTime");
    if (G.uOrigin < 0 || G.uScale < 0 || G.uFill < 0 || G.uFlash < 0 ||
        G.uAlpha < 0 || G.uDirFill < 0 || G.uDirCap < 0 || G.uTint < 0 ||
        G.uLabelTint < 0 || G.uLabelSize < 0 || G.uLabelTex < 0 ||
        G.uFire < 0 || G.uTime < 0) {
        set_shader_log("one or more required shader uniforms were unavailable");
        glDeleteProgram(G.prog); G.prog = 0;
        glBindTexture(GL_TEXTURE_2D, oldTexture);
        glActiveTexture(oldActive);
        glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
        glUseProgram(oldProgram);
        return -132;
    }

    static const float quad[8] = {-1.0f,-1.0f, 1.0f,-1.0f, -1.0f,1.0f, 1.0f,1.0f};
    g_gl_init_stage.store(40, std::memory_order_relaxed);
    glGenBuffers(1, &G.vbo);
    if (G.vbo) {
        glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    }
    if (G.es3 && G.vbo) {
        glGenVertexArrays(1, &G.vao);
        if (G.vao) {
            glBindVertexArray(G.vao);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glBindVertexArray(oldVao);
        }
    }
    glGenTextures(1, &G.labelTex);
    if (G.labelTex) {
        glBindTexture(GL_TEXTURE_2D, G.labelTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        static const uint8_t blank[84 * 16 * 4] = {};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 84, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, blank);
    }
    const GLenum resourceError = capture_gl_error(41);
    if (!G.vbo || !G.labelTex || (G.es3 && !G.vao) || resourceError != GL_NO_ERROR) {
        set_shader_log("GL resource setup failed; inspect gl_error_code and gl_init_stage");
        if (G.vbo) glDeleteBuffers(1, &G.vbo);
        if (G.vao) glDeleteVertexArrays(1, &G.vao);
        if (G.labelTex) glDeleteTextures(1, &G.labelTex);
        if (G.prog) glDeleteProgram(G.prog);
        G.vbo = G.vao = G.labelTex = G.prog = 0;
        glBindTexture(GL_TEXTURE_2D, oldTexture);
        glActiveTexture(oldActive);
        glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
        glUseProgram(oldProgram);
        return -140;
    }

    glUseProgram(G.prog);
    const float capAngle = kHornAngBot + (kHornAngTop - kHornAngBot) * kHornWinLo;
    glUniform2f(G.uDirCap, cosf(capAngle), sinf(capAngle));
    glUniform1f(G.uAlpha, 1.0f);
    glUniform1i(G.uLabelTex, 1);
    G.bannerOk = init_banner_gl();
    if (!G.bannerOk) LOGE("Claude banner renderer unavailable; retaining existing horn meter");
    (void)capture_gl_error(42);

    glBindTexture(GL_TEXTURE_2D, oldTexture);
    glActiveTexture(oldActive);
    glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
    glUseProgram(oldProgram);
    (void)capture_gl_error(43);
    G.ok = true;
    g_gl_init_stage.store(100, std::memory_order_relaxed);
    set_shader_log("shader/program/resources initialized");
    return 0;
}

static int glyph_index(char c) {
    switch (c) {
        case 'P': return 0; case 'E': return 1; case 'R': return 2;
        case 'F': return 3; case 'C': return 4; case 'T': return 5;
        case 'A': return 6; case 'L': return 7; case 'Y': return 8;
        default: return -1;
    }
}

static void update_horn_label(int timing) {
    GLint previousActive = GL_TEXTURE0, previousTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActive);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    static const uint8_t glyphs[9][7] = {
        {30,17,17,30,16,16,16}, // P
        {31,16,16,30,16,16,31}, // E
        {30,17,17,30,20,18,17}, // R
        {31,16,16,30,16,16,16}, // F
        {14,17,16,16,16,17,14}, // C
        {31,4,4,4,4,4,4},       // T
        {14,17,17,31,17,17,17}, // A
        {16,16,16,16,16,16,31}, // L
        {17,17,10,4,4,4,4}      // Y
    };
    const char *label = "";
    float tint[3] = {1.0f, 1.0f, 1.0f};
    if (timing == 3) { label = "PERFECT"; tint[0]=0.21f; tint[1]=0.82f; tint[2]=0.23f; }
    else if (timing == 2) { label = "EARLY"; tint[0]=1.0f; tint[1]=0.60f; tint[2]=0.10f; }
    else if (timing == 4) { label = "LATE"; tint[0]=1.0f; tint[1]=0.60f; tint[2]=0.10f; }
    else if (timing == 1) { label = "EARLY"; tint[0]=0.90f; tint[1]=0.15f; tint[2]=0.15f; }
    else if (timing == 5) { label = "LATE"; tint[0]=0.90f; tint[1]=0.15f; tint[2]=0.15f; }
    const size_t chars = strlen(label);
    uint8_t pixels[84 * 16 * 4] = {};
    for (size_t x=0; x<chars; ++x) {
        const int gi = glyph_index(label[x]);
        if (gi < 0) continue;
        for (int y=0; y<7; ++y) {
            for (int col=0; col<5; ++col) {
                if ((glyphs[gi][y] & (1u << (4-col))) == 0) continue;
                for (int yy=0; yy<2; ++yy) for (int xx=0; xx<2; ++xx) {
                    const size_t at = (static_cast<size_t>(y*2+yy) * 84 + x*12 + col*2 + xx) * 4;
                    pixels[at+0]=255; pixels[at+1]=255; pixels[at+2]=255; pixels[at+3]=255;
                }
            }
        }
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, G.labelTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 84, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    G.labelWidth = static_cast<float>(chars * 12);
    G.labelTiming = timing;
    glBindTexture(GL_TEXTURE_2D, previousTexture);
    glActiveTexture(previousActive);
}

static void draw_horn(int surfaceW, int surfaceH, int anchorX, int anchorY,
                      float progress, bool perfect, int resultTiming,
                      bool fireActive, float fireElapsed) {
    (void)surfaceW;
    if (!G.ok) return;
    (void)capture_gl_error(190); // pending error before any meter GL work
    if (G.labelTiming != -1) update_horn_label(-1);

    GLint oldProgram=0, oldViewport[4]={0,0,0,0}, oldBuffer=0, oldVao=0;
    GLint srcRgb=GL_ONE, dstRgb=GL_ZERO, srcA=GL_ONE, dstA=GL_ZERO;
    GLint eqRgb=GL_FUNC_ADD, eqA=GL_FUNC_ADD, oldActive=GL_TEXTURE0, oldTexture=0;
    GLint a0Enabled=0, a0Size=4, a0Type=GL_FLOAT, a0Normalized=0, a0Stride=0, a0Buffer=0;
    void *a0Pointer=nullptr;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb); glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA); glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRgb); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqA);
    GLboolean wasBlend=glIsEnabled(GL_BLEND), wasDepth=glIsEnabled(GL_DEPTH_TEST);
    GLboolean wasScissor=glIsEnabled(GL_SCISSOR_TEST), wasCull=glIsEnabled(GL_CULL_FACE);
    GLboolean wasStencil=glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    if (G.es3) glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    else {
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a0Enabled);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a0Size);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a0Type);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a0Normalized);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a0Stride);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a0Buffer);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a0Pointer);
    }
    (void)capture_gl_error(191); // GL state-query checkpoint

    const float scale = (160.0f * std::max(0.72f, std::min(1.35f, surfaceH / 720.0f))) / 319.0f;
    const float bx = anchorX + 43.0f*scale - 123.5f*scale;
    const float topY = anchorY + 48.0f*scale;
    const float by = topY - 319.0f*scale;
    const GLint vx = static_cast<GLint>(floorf(bx + 60.0f*scale));
    const GLint vy = static_cast<GLint>(floorf(by + 19.0f*scale));
    const GLsizei vw = static_cast<GLsizei>(ceilf(187.0f*scale)+1.0f);
    /* Extend the top of the render viewport by 32 design units so the
       independent flame can rise above the projected head without clipping. */
    const GLsizei vh = static_cast<GLsizei>(ceilf(332.0f*scale)+1.0f);

    if (wasDepth) glDisable(GL_DEPTH_TEST);
    if (wasScissor) glDisable(GL_SCISSOR_TEST);
    if (wasCull) glDisable(GL_CULL_FACE);
    if (wasStencil) glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(vx, vy, vw, vh);
    glUseProgram(G.prog);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, G.labelTex);
    glUniform1i(G.uLabelTex, 1);
    glUniform2f(G.uOrigin, bx, by);
    glUniform1f(G.uScale, scale);
    glUniform1f(G.uFill, progress);
    glUniform1f(G.uFlash, perfect ? 1.0f : 0.0f);
    glUniform1f(G.uFire, fireActive ? 1.0f : 0.0f);
    glUniform1f(G.uTime, fireElapsed);
    glUniform2f(G.uDirFill,
        cosf(kHornAngBot + (kHornAngTop-kHornAngBot)*progress),
        sinf(kHornAngBot + (kHornAngTop-kHornAngBot)*progress));
    glUniform3f(G.uTint, 0.21f, 0.82f, 0.23f);
    if (resultTiming == 3) glUniform3f(G.uLabelTint, 0.21f, 0.82f, 0.23f);
    else if (resultTiming == 2 || resultTiming == 4) glUniform3f(G.uLabelTint, 1.0f, 0.60f, 0.10f);
    else glUniform3f(G.uLabelTint, 0.90f, 0.15f, 0.15f);
    glUniform2f(G.uLabelSize, G.labelWidth, G.labelWidth > 0.0f ? 7.0f : 0.0f);
    (void)capture_gl_error(192); // uniforms/state setup checkpoint

    if (G.es3) glBindVertexArray(G.vao);
    else {
        glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const GLenum drawError = capture_gl_error(200);
    if (drawError != GL_NO_ERROR) {
        g_gl_draw_error_code.store(static_cast<int32_t>(drawError), std::memory_order_relaxed);
        g_gl_draw_error_count.fetch_add(1, std::memory_order_relaxed);
    }

    if (G.es3) glBindVertexArray(oldVao);
    else {
        glBindBuffer(GL_ARRAY_BUFFER, a0Buffer);
        glVertexAttribPointer(0, a0Size, a0Type, a0Normalized, a0Stride, a0Pointer);
        if (!a0Enabled) glDisableVertexAttribArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
    glBindTexture(GL_TEXTURE_2D, oldTexture);
    glActiveTexture(oldActive);
    glUseProgram(oldProgram);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glBlendEquationSeparate(eqRgb, eqA);
    glBlendFuncSeparate(srcRgb, dstRgb, srcA, dstA);
    if (!wasBlend) glDisable(GL_BLEND);
    if (wasDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (wasScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (wasCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (wasStencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    (void)capture_gl_error(203); // meter GL-state restore checkpoint
}


static float banner_clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}
static float banner_ease_out_back(float value) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float u = banner_clamp01(value) - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

/* Claude's SDF neon banner is drawn by the existing EGL hook. Its label index
   is derived only from the real game release enum (1..5), never from fill. */
static void draw_banner(int viewportX, int viewportY, int surfaceW, int surfaceH,
                        int anchorX, int anchorY, int resultTiming, float elapsed) {
    if (!G.ok || !G.bannerOk || resultTiming < 1 || resultTiming > 5) return;
    const int resultIndex = resultTiming - 1;
    const float scale = (160.0f * std::max(0.72f, std::min(1.35f, surfaceH / 720.0f))) / 319.0f;
    const float hornBx = anchorX + 43.0f * scale - 123.5f * scale;
    const float hornTopY = anchorY + 48.0f * scale;
    const float hornBy = hornTopY - 319.0f * scale;
    const float hornCenterX = hornBx + 123.5f * scale;
    const float hornCenterY = hornBy + 159.5f * scale;

    const float bannerScale = scale;
    const float bannerW = 420.0f * bannerScale;
    const float bannerH = 130.0f * bannerScale;
    const float bannerCenterX = hornCenterX + 0.75f * 319.0f * scale;
    const float bannerCenterY = hornCenterY + 0.35f * 319.0f * scale;
    float bx0 = bannerCenterX - bannerW * 0.5f;
    float by0 = bannerCenterY - bannerH * 0.5f;
    const float minX = static_cast<float>(viewportX);
    const float minY = static_cast<float>(viewportY);
    const float maxX = static_cast<float>(viewportX + surfaceW) - bannerW;
    const float maxY = static_cast<float>(viewportY + surfaceH) - bannerH;
    if (maxX < minX || maxY < minY) return;
    bx0 = std::max(minX, std::min(maxX, bx0));
    by0 = std::max(minY, std::min(maxY, by0));

    const float t = std::max(0.0f, elapsed);
    const float wipeLinear = banner_clamp01(t / 0.26f);
    const float wipe = 1.0f - (1.0f - wipeLinear) * (1.0f - wipeLinear) * (1.0f - wipeLinear);
    const float fade = banner_clamp01(t / 0.10f) *
        (1.0f - banner_clamp01((t - (kBannerDurationSec - 0.25f)) / 0.25f));
    const float pop = 0.60f + 0.40f * banner_ease_out_back(t / 0.20f);
    const GLint clipH = static_cast<GLint>(ceilf(bannerH * wipe));
    if (clipH <= 0 || fade <= 0.0f) return;

    (void)capture_gl_error(210); // pending error before banner GL work
    const HornTextRect& tr = kHornText[resultIndex];
    const float textScale = fminf(2.1f, 270.0f / tr.tw);
    const float cellW = HORN_ATLAS_W * textScale;
    const float cellH = tr.h * textScale;
    const float textOx = 196.0f - (HORN_ATLAS_PAD + tr.tw * 0.5f) * textScale;
    const float textOy = 54.0f - (HORN_ATLAS_PAD + tr.th * 0.5f) * textScale;
    const float sdfK = (255.0f / 127.0f) * HORN_ATLAS_SPREAD * textScale;

    GLint oldProgram = 0, oldViewport[4] = {0,0,0,0}, oldBuffer = 0, oldVao = 0;
    GLint srcRgb = GL_ONE, dstRgb = GL_ZERO, srcA = GL_ONE, dstA = GL_ZERO;
    GLint eqRgb = GL_FUNC_ADD, eqA = GL_FUNC_ADD;
    GLint oldActive = GL_TEXTURE0, oldTexture = 0, oldSampler = 0, oldScissor[4] = {0,0,0,0};
    GLint a0Enabled = 0, a0Size = 4, a0Type = GL_FLOAT, a0Normalized = 0;
    GLint a0Stride = 0, a0Buffer = 0;
    void *a0Pointer = nullptr;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqA);
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    const GLboolean wasBlend = glIsEnabled(GL_BLEND);
    const GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean wasScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean wasCull = glIsEnabled(GL_CULL_FACE);
    const GLboolean wasStencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    if (G.es3) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
        glGetIntegerv(GL_SAMPLER_BINDING, &oldSampler);
    } else {
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a0Enabled);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a0Size);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a0Type);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a0Normalized);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a0Stride);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a0Buffer);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a0Pointer);
    }

    if (wasDepth) glDisable(GL_DEPTH_TEST);
    if (wasCull) glDisable(GL_CULL_FACE);
    if (wasStencil) glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<GLint>(floorf(bx0)), static_cast<GLint>(floorf(by0)),
              static_cast<GLsizei>(ceilf(bannerW)),
              std::max<GLsizei>(1, clipH));
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(static_cast<GLint>(floorf(bx0)), static_cast<GLint>(floorf(by0)),
               static_cast<GLsizei>(ceilf(bannerW)) + 1,
               static_cast<GLsizei>(ceilf(bannerH)) + 1);
    glUseProgram(G.bannerProg);
    glBindTexture(GL_TEXTURE_2D, G.bannerTex);
    if (G.es3) glBindSampler(0, 0);
    glUniform2f(G.bOrigin, bx0, by0);
    glUniform1f(G.bScale, bannerScale);
    glUniform1f(G.bAlpha, fade);
    glUniform1f(G.bPop, pop);
    glUniform1f(G.bSdfK, sdfK);
    glUniform3f(G.bTint, kBannerTint[resultIndex][0],
                kBannerTint[resultIndex][1], kBannerTint[resultIndex][2]);
    glUniform2f(G.bTextOrigin, textOx, textOy);
    glUniform2f(G.bCellSize, cellW, cellH);
    glUniform2f(G.bTexV, static_cast<float>(tr.y) / HORN_ATLAS_H,
                static_cast<float>(tr.h) / HORN_ATLAS_H);
    (void)capture_gl_error(211); // banner uniform/state setup checkpoint
    if (G.es3) {
        glBindVertexArray(G.vao);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const GLenum drawError = capture_gl_error(201);
    if (drawError == GL_NO_ERROR)
        g_banner_draw_calls.fetch_add(1, std::memory_order_relaxed);

    if (G.es3) {
        glBindVertexArray(oldVao);
        glBindSampler(0, oldSampler);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, a0Buffer);
        glVertexAttribPointer(0, a0Size, a0Type, a0Normalized, a0Stride, a0Pointer);
        if (!a0Enabled) glDisableVertexAttribArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
    glBindTexture(GL_TEXTURE_2D, oldTexture);
    glActiveTexture(oldActive);
    glUseProgram(oldProgram);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
    glBlendEquationSeparate(eqRgb, eqA);
    glBlendFuncSeparate(srcRgb, dstRgb, srcA, dstA);
    if (wasBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (wasDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (wasScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (wasCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (wasStencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    (void)capture_gl_error(213); // banner GL-state restore checkpoint
}

static uint64_t monotonic_ns(void) {
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}


static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static size_t decode_base64(const char *src, uint8_t *dst, size_t cap) {
    uint32_t acc=0; int bits=-8; size_t out=0;
    for (const char *p=src; *p && *p!='='; ++p) {
        const int v=b64_value(*p); if (v<0) continue;
        acc=(acc<<6)|static_cast<uint32_t>(v); bits+=6;
        if (bits>=0) {
            if (out>=cap) return 0;
            dst[out++]=static_cast<uint8_t>((acc>>bits)&0xffu);
            bits-=8;
        }
    }
    return out;
}
static bool decode_green_giant_ima(int16_t *out, size_t expectedSamples) {
    static uint8_t packed[22400];
    const size_t bytes=decode_base64(kGreenGiantImaB64, packed, sizeof(packed));
    if (bytes < 16 || memcmp(packed, "GGI1", 4)!=0) return false;
    const uint32_t samples=static_cast<uint32_t>(packed[4]) |
        (static_cast<uint32_t>(packed[5])<<8) |
        (static_cast<uint32_t>(packed[6])<<16) |
        (static_cast<uint32_t>(packed[7])<<24);
    const uint32_t blockSize=static_cast<uint32_t>(packed[8]) |
        (static_cast<uint32_t>(packed[9])<<8) |
        (static_cast<uint32_t>(packed[10])<<16) |
        (static_cast<uint32_t>(packed[11])<<24);
    const uint32_t payloadBytes=static_cast<uint32_t>(packed[12]) |
        (static_cast<uint32_t>(packed[13])<<8) |
        (static_cast<uint32_t>(packed[14])<<16) |
        (static_cast<uint32_t>(packed[15])<<24);
    if (samples!=expectedSamples || blockSize!=256 || payloadBytes+16!=bytes) return false;
    static const int stepTable[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
    static const int indexTable[16]={-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
    size_t pos=16, written=0;
    while (written<samples) {
        const size_t blockSamples=std::min<size_t>(blockSize, samples-written);
        if (pos+4>bytes) return false;
        int predictor=static_cast<int16_t>(static_cast<uint16_t>(packed[pos]) |
            (static_cast<uint16_t>(packed[pos+1])<<8));
        int index=std::min(88, static_cast<int>(packed[pos+2]));
        const size_t nibbleBytes=(blockSamples-1+1)/2;
        if (pos+4+nibbleBytes>bytes) return false;
        out[written++]=static_cast<int16_t>(predictor);
        for (size_t i=0;i<blockSamples-1;++i) {
            const uint8_t byte=packed[pos+4+i/2];
            const int code=(i&1u) ? (byte>>4) : (byte&15);
            const int step=stepTable[index];
            int delta=step>>3;
            if (code&1) delta+=step>>2;
            if (code&2) delta+=step>>1;
            if (code&4) delta+=step;
            predictor=(code&8) ? predictor-delta : predictor+delta;
            predictor=std::max(-32768,std::min(32767,predictor));
            index=std::max(0,std::min(88,index+indexTable[code]));
            out[written++]=static_cast<int16_t>(predictor);
        }
        pos+=4+nibbleBytes;
    }
    return written==samples && pos==bytes;
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
        SL_DATAFORMAT_PCM, 1, kGreenGiantRate * 1000,
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

    if (!decode_green_giant_ima(g_green_giant_pcm, kGreenGiantSamples)) {
        g_sound_init_code.store(-2001); return false;
    }
    g_sound_init_code.store(0);
    return true;
}

static void play_perfect_sound_now(void) {
    if (!g_sl_play || !g_sl_queue) return;
    const uint64_t now = monotonic_ns();
    pthread_mutex_lock(&g_sound_mutex);
    const uint64_t previous = g_last_sound_ns.load(std::memory_order_relaxed);
    if (previous == 0 || now < previous || now - previous >= 450000000ULL) {
        const SLresult clear_result = (*g_sl_queue)->Clear(g_sl_queue);
        const SLresult enqueue_result = clear_result == SL_RESULT_SUCCESS
            ? (*g_sl_queue)->Enqueue(g_sl_queue, g_green_giant_pcm, sizeof(g_green_giant_pcm))
            : clear_result;
        if (enqueue_result == SL_RESULT_SUCCESS &&
            (*g_sl_play)->SetPlayState(g_sl_play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS) {
            g_last_sound_ns.store(now, std::memory_order_relaxed);
            g_sound_play_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    pthread_mutex_unlock(&g_sound_mutex);
}

/* Release-event callbacks only enqueue a request. OpenSL calls run on this
   worker, never on the game's render/release callback. */
static void request_perfect_sound(void) {
    g_sound_requests.fetch_add(1, std::memory_order_release);
}
static void *sound_worker(void *) {
    for (;;) {
        const uint32_t requests = g_sound_requests.exchange(0, std::memory_order_acq_rel);
        if (requests) play_perfect_sound_now();
        struct timespec pause{0, 10000000L};
        nanosleep(&pause, nullptr);
    }
    return nullptr;
}

static float clamp_progress(float raw, float reference) {
    if (!std::isfinite(raw)) return 0.0f;
    if (!std::isfinite(reference) || reference <= 0.001f)
        reference = kPerfectRawInitial;
    return std::max(0.0f, std::min(1.0f,
        (raw / reference) * kPerfectFillTarget));
}

__attribute__((noinline))
static void release_event_proxy(void *actor, int32_t timing, float arg0, float arg1) {
    /* Snapshot the live component BEFORE forwarding. The stock event dispatcher can
       synchronously transition state 2 into released/feedback states. */
    const uintptr_t tracked_before = g_component
        ? *reinterpret_cast<volatile uintptr_t *>(g_component + 0x08) : 0;
    const int32_t state_before = g_component
        ? *reinterpret_cast<volatile int32_t *>(g_component + 0x18) : -1;
    const float raw_before = g_component
        ? *reinterpret_cast<volatile float *>(g_component + 0x10) : NAN;
    const float animation_before = g_component
        ? *reinterpret_cast<volatile float *>(g_component + 0x14) : NAN;
    const int32_t component_timing_before = g_component
        ? *reinterpret_cast<volatile int32_t *>(g_component + 0x1c) : -1;
    const float release_value_before = g_component
        ? *reinterpret_cast<volatile float *>(g_component + 0x20) : NAN;

    const ReleaseFn original = g_original_release_event;
    if (original) original(actor, timing, arg0, arg1);

    g_release_event_calls.fetch_add(1, std::memory_order_relaxed);
    g_last_release_timing.store(timing, std::memory_order_relaxed);
    g_release_pre_state.store(state_before, std::memory_order_relaxed);
    g_release_pre_timing_enum.store(component_timing_before, std::memory_order_relaxed);
    store_float(g_release_arg0_bits, arg0);
    store_float(g_release_arg1_bits, arg1);

    /* Only the tracked player's live normal jumpshot state is eligible. This
       rejects scripted/special/other-player event paths without guessing timing. */
    if (!actor || !tracked_before ||
        reinterpret_cast<uintptr_t>(actor) != tracked_before ||
        state_before != 2 || timing < 1 || timing > 5) return;

    g_eligible_local_release_events.fetch_add(1, std::memory_order_relaxed);
    const float reference = load_float(g_perfect_raw_reference_bits);
    const float result_position = clamp_progress(raw_before, reference);
    store_float(g_release_raw_bits, raw_before);
    store_float(g_release_animation_bits, animation_before);
    store_float(g_release_value_bits, release_value_before);
    store_float(g_release_position_bits, result_position);
    store_float(g_result_raw_bits, result_position);
    g_result_timing.store(timing, std::memory_order_release);
    const uint64_t resultStartNs = monotonic_ns();
    g_result_started_ns.store(resultStartNs, std::memory_order_release);
    g_result_until_ns.store(resultStartNs + kBannerDurationNs, std::memory_order_release);
    g_result_sequence.fetch_add(1, std::memory_order_acq_rel);

    if (timing == 3) {
        g_perfect_release_events.fetch_add(1, std::memory_order_relaxed);
        g_perfect_until_ns.store(monotonic_ns() + kBannerDurationNs, std::memory_order_release);
        g_perfect_fire_trigger_count.fetch_add(1, std::memory_order_relaxed);
        /* Calibrate subsequent shots only. This release stays frozen at the
           position computed with the previous reference; no perfect-fill snap. */
        if (std::isfinite(raw_before) && raw_before > 0.001f && raw_before < 1.0f) {
            const float old_reference = reference > 0.001f ? reference : kPerfectRawInitial;
            store_float(g_perfect_raw_reference_bits,
                old_reference * 0.80f + raw_before * 0.20f);
        }
        request_perfect_sound();
    }
}


static void draw_overlay(EGLDisplay display, EGLSurface surface) {
    if (!g_component || g_anchor_valid.load(std::memory_order_acquire) == 0) return;
    const uintptr_t tracked = *reinterpret_cast<volatile uintptr_t *>(g_component + 0x08);
    const float raw = *reinterpret_cast<volatile float *>(g_component + 0x10);
    const float animation = *reinterpret_cast<volatile float *>(g_component + 0x14);
    const int32_t state = *reinterpret_cast<volatile int32_t *>(g_component + 0x18);
    const int32_t component_timing = *reinterpret_cast<volatile int32_t *>(g_component + 0x1c);
    const float release_value = *reinterpret_cast<volatile float *>(g_component + 0x20);
    g_component_state.store(state, std::memory_order_relaxed);
    g_tracked_player.store(tracked ? 1U : 0U, std::memory_order_relaxed);
    store_float(g_meter_bits, raw);
    store_float(g_component_animation_bits, animation);
    store_float(g_component_release_value_bits, release_value);
    g_component_timing_enum.store(component_timing, std::memory_order_relaxed);

    const uint64_t now = monotonic_ns();
    const bool resultActive = tracked != 0 &&
        now <= g_result_until_ns.load(std::memory_order_acquire);
    const int32_t resultTiming = resultActive ? g_result_timing.load(std::memory_order_acquire) : -1;
    // Excellent meter tint persists for the complete shot-result fade.
    const bool perfect = resultActive && resultTiming == 3;
    const bool fireActive = resultActive && resultTiming == 3;
    g_perfect_fire_active.store(fireActive ? 1U : 0U, std::memory_order_relaxed);
    const uint64_t resultStartForFire = g_result_started_ns.load(std::memory_order_acquire);
    const float fireElapsed = (resultStartForFire > 0 && now >= resultStartForFire)
        ? static_cast<float>(now - resultStartForFire) / 1000000000.0f : 0.0f;
    const bool meterActive = tracked && state == 2 && std::isfinite(raw);
    if (!tracked || (!meterActive && !resultActive)) return;

    float progress = resultActive
        ? load_float(g_result_raw_bits)
        : clamp_progress(raw, load_float(g_perfect_raw_reference_bits));
    store_float(g_marker_bits, progress);
    if (meterActive) g_active_meter_frames.fetch_add(1, std::memory_order_relaxed);
    if (perfect) g_perfect_flash_frames.fetch_add(1, std::memory_order_relaxed);
    if (fireActive) g_perfect_fire_frames.fetch_add(1, std::memory_order_relaxed);

    const EGLContext currentContext = eglGetCurrentContext();
    g_egl_context_value.store(reinterpret_cast<uintptr_t>(currentContext), std::memory_order_relaxed);
    g_egl_display_value.store(reinterpret_cast<uintptr_t>(display), std::memory_order_relaxed);
    g_egl_surface_value.store(reinterpret_cast<uintptr_t>(surface), std::memory_order_relaxed);
    if (currentContext == EGL_NO_CONTEXT) {
        g_shader_init_code.store(-100, std::memory_order_relaxed);
        return;
    }
    EGLint sw=0, sh=0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &sw) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &sh) != EGL_TRUE || sw <= 0 || sh <= 0) return;
    (void)capture_gl_error(180); // pending GL error before overlay state queries
    GLint vp[4]={0,0,0,0}, fbo=-1;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    (void)capture_gl_error(181); // overlay state-query checkpoint
    g_surface_w.store(sw); g_surface_h.store(sh);
    g_viewport_w.store(vp[2]); g_viewport_h.store(vp[3]); g_framebuffer.store(fbo);
    if (fbo != 0 || vp[2] <= 0 || vp[3] <= 0) return;

    if (G.ctx != currentContext) {
        G = {};
        G.ctx = currentContext;
        g_gl_init_attempts.store(0, std::memory_order_relaxed);
    }
    const uint64_t initNow = monotonic_ns();
    static uint64_t lastInitAttemptNs = 0;
    const uint32_t attempts = g_gl_init_attempts.load(std::memory_order_relaxed);
    if (!G.ok && (attempts < 3 || initNow < lastInitAttemptNs ||
                  initNow - lastInitAttemptNs >= 2000000000ULL)) {
        lastInitAttemptNs = initNow;
        g_gl_init_attempts.fetch_add(1, std::memory_order_relaxed);
        const int initCode = init_horn_gl();
        G.ok = (initCode == 0);
        g_shader_init_code.store(initCode, std::memory_order_relaxed);
    }
    if (!G.ok) return;

    const float sx = static_cast<float>(vp[2]) / static_cast<float>(sw);
    const float sy = static_cast<float>(vp[3]) / static_cast<float>(sh);
    const float target_x = load_float(g_anchor_x_bits);
    const float target_y = load_float(g_anchor_y_bits);
    /* Use the exact, unsmoothed projected head anchor from the v53 baseline.
       Meter placement must track the current camera projection immediately. */
    const GLint ax = vp[0] + static_cast<GLint>(std::lround(target_x * sx));
    const GLint top_y = static_cast<GLint>(std::lround(target_y * sy));
    const GLint ay = vp[1] + vp[3] - top_y;
    draw_horn(vp[2], vp[3], ax, ay, progress, perfect, resultTiming,
              fireActive, fireElapsed);
    if (fireActive) g_perfect_fire_draw_calls.fetch_add(1, std::memory_order_relaxed);
    if (resultActive && resultTiming >= 1 && resultTiming <= 5) {
        const uint64_t resultStartNs = g_result_started_ns.load(std::memory_order_acquire);
        const float elapsed = (resultStartNs > 0 && now >= resultStartNs)
            ? static_cast<float>(now - resultStartNs) / 1000000000.0f : 0.0f;
        draw_banner(vp[0], vp[1], vp[2], vp[3], ax, ay, resultTiming, elapsed);
    }
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
}

/* Capture EGL/GL state on ordinary frames too. Previously the report
   sampled these fields only while a shot meter was active, which made an idle
   player frame look like GL had never initialized. */
static void capture_swap_diagnostics(EGLDisplay display, EGLSurface surface) {
    const EGLContext context = eglGetCurrentContext();
    g_egl_context_value.store(reinterpret_cast<uintptr_t>(context), std::memory_order_relaxed);
    g_egl_display_value.store(reinterpret_cast<uintptr_t>(display), std::memory_order_relaxed);
    g_egl_surface_value.store(reinterpret_cast<uintptr_t>(surface), std::memory_order_relaxed);
    if (context == EGL_NO_CONTEXT) return;

    EGLint sw = 0, sh = 0;
    if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE &&
        eglQuerySurface(display, surface, EGL_WIDTH, &sw) == EGL_TRUE &&
        eglQuerySurface(display, surface, EGL_HEIGHT, &sh) == EGL_TRUE) {
        g_surface_w.store(sw, std::memory_order_relaxed);
        g_surface_h.store(sh, std::memory_order_relaxed);
    }
    GLint viewport[4] = {0, 0, 0, 0};
    GLint framebuffer = -1;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    g_viewport_w.store(viewport[2], std::memory_order_relaxed);
    g_viewport_h.store(viewport[3], std::memory_order_relaxed);
    g_framebuffer.store(framebuffer, std::memory_order_relaxed);

    uint32_t expected = 0;
    if (g_gl_identity_captured.compare_exchange_strong(expected, 1,
            std::memory_order_acq_rel)) {
        const GLubyte *version = glGetString(GL_VERSION);
        if (version) {
            set_gl_string(g_gl_version, sizeof(g_gl_version), version);
            set_gl_string(g_gl_renderer, sizeof(g_gl_renderer), glGetString(GL_RENDERER));
            set_gl_string(g_gl_vendor, sizeof(g_gl_vendor), glGetString(GL_VENDOR));
            set_gl_string(g_glsl_version, sizeof(g_glsl_version),
                          glGetString(GL_SHADING_LANGUAGE_VERSION));
        } else {
            g_gl_identity_captured.store(0, std::memory_order_release);
        }
    }
}

__attribute__((noinline))
static EGLBoolean swap_proxy(EGLDisplay display, EGLSurface surface) {
    const uint32_t frame = g_swap_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    /* One sample immediately, then about once per 60 rendered frames. */
    if (frame == 1 || (frame % 60u) == 0u)
        capture_swap_diagnostics(display, surface);
    // Sample the GL error state before our overlay touches GL. This separates
    // errors already pending from the game from errors introduced by overlay work.
    if (g_component && g_anchor_valid.load(std::memory_order_acquire) != 0 &&
        (g_tracked_player.load(std::memory_order_relaxed) != 0 ||
         g_result_sequence.load(std::memory_order_relaxed) != 0))
        (void)capture_gl_error(170);
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
    else if (g_banner_draw_calls.load()) status = "CLAUDE_BANNER_DRAWN";
    else if (g_draw_calls.load()) status = "HORN_SHADER_DRAWN";
    else if (g_active_meter_frames.load()) status = "METER_ACTIVE_WAITING_FOR_RENDER";
    else if (g_anchor_valid.load()) status = "PLAYER_ANCHOR_READY_WAITING_FOR_SHOT";
    else if (g_swap_calls.load()) status = "WAITING_FOR_VALID_PLAYER_PROJECTION";
    else status = "HOOKS_READY_WAITING_FOR_FRAMES";

    const bool perfect_active = monotonic_ns() <= g_perfect_until_ns.load();
    char body[8192];
    char glVersion[160], glRenderer[160], glVendor[160], glslVersion[160], glShaderLog[768];
    pthread_mutex_lock(&g_gl_diag_mutex);
    snprintf(glVersion, sizeof(glVersion), "%s", g_gl_version);
    snprintf(glRenderer, sizeof(glRenderer), "%s", g_gl_renderer);
    snprintf(glVendor, sizeof(glVendor), "%s", g_gl_vendor);
    snprintf(glslVersion, sizeof(glslVersion), "%s", g_glsl_version);
    snprintf(glShaderLog, sizeof(glShaderLog), "%s", g_gl_shader_log);
    pthread_mutex_unlock(&g_gl_diag_mutex);
    (void)snprintf(body, sizeof(body),
        "agent=v55\npurpose=grade_aware_probability_with_perfect_fire_meter_and_pre_overlay_gl_diagnostics\n"
        "banner_renderer=CLAUDE_SDF_NEON_PANEL\nbanner_animation=WIPE_UP_BOTTOM_TO_TOP\nperfect_effect=PROCEDURAL_GREEN_FLAME_ABOVE_PLAYER_HEAD\nperfect_meter_color=FULL_METER_GREEN_UNTIL_RESULT_FADE\nperfect_fire_animation=UPWARD_FLICKER_AND_PARTICLE_LIKE_TONGUES\n"
        "banner_timing_source=GAME_RELEASE_ENUM\nengine=Dobby\n"
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
        "last_meter_value=%.7g\nlast_meter_position=%.7g\n"
        "component_animation_progress=%.7g\ncomponent_timing_enum=%d\n"
        "component_release_value=%.7g\nperfect_raw_reference=%.7g\n"
        "last_release_raw=%.7g\nlast_release_animation=%.7g\n"
        "last_release_value=%.7g\nlast_release_position=%.7g\n"
        "release_pre_state=%d\nrelease_pre_component_timing=%d\n"
        "release_arg0=%.7g\nrelease_arg1=%.7g\neligible_local_release_events=%u\n"
        "shader_init_code=%d\nresult_timing_enum=%d\nresult_sequence=%u\n"
        "release_event_callbacks=%u\nlast_release_timing_class=%d\nperfect_class3_release_events=%u\n"
        "perfect_flash_active=%u\nperfect_flash_frames=%u\nsound_init_code=%d\n"
        "perfect_sound_play_count=%u\nperfect_fire_trigger_count=%u\nperfect_fire_draw_calls=%u\nperfect_fire_frames=%u\nperfect_fire_active=%u\nsurface_width=%d\nsurface_height=%d\n"
        "viewport_width=%d\nviewport_height=%d\nframebuffer_binding=%d\n"
        "saved_view_copy_only=1\ngame_owned_view_bytes_written=0\n"
        "game_object_fields_modified=0\ncomponent_memory_modified=0\n"
        "original_meter_resource_modified=0\nshot_timing_modified=0\n"
        "original_library_file_modified=0\n"
        "egl_current_context=%p\negl_current_display=%p\negl_current_surface=%p\n"
        "gl_version=%s\ngl_renderer=%s\ngl_vendor=%s\nglsl_version=%s\n"
        "gl_init_stage=%d\ngl_error_code=%d\ngl_error_count=%u\n"
        "gl_last_error_stage=%d\ngl_pre_overlay_error_count=%u\n"
        "vertex_shader_status=%d\nfragment_shader_status=%d\nprogram_link_status=%d\n"
        "shader_log=%s\ngl_draw_error_code=%d\ngl_draw_error_count=%u\n"
        "gl_init_attempts=%u\n"
        "probability_feature=GRADE_AWARE_CHANCE_REMAP_V2\n"
        "probability_mapping=1:0,2:grade,3:0.97,4:grade,5:0\n"
        "probability_map_self_test_status=%s\nprobability_map_self_test_cases=%u\n"
        "probability_grade_order=F,D-,D,D+,C-,C,C+,B-,B,B+,A-,A,A+\n"
        "probability_hooks_enabled=%u\nprobability_status=%s\n"
        "probability_hook_address=%p\nprobability_hook_code=%d\n"
        "probability_hook_calls=%u\nprobability_modified_shots=%u\n"
        "probability_invalid_context=%u\nlast_original_launch_chance=%.7g\n"
        "last_applied_launch_chance=%.7g\nlast_grade_distance=%.7g\n"
        "last_grade_index=%d\nlast_grade_timing_enum=%d\n"
        "shot_feedback_probability_storage_modified=0\n"
        "status=%s\n",
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
        load_float(g_marker_bits), load_float(g_component_animation_bits),
        g_component_timing_enum.load(), load_float(g_component_release_value_bits),
        load_float(g_perfect_raw_reference_bits), load_float(g_release_raw_bits),
        load_float(g_release_animation_bits), load_float(g_release_value_bits),
        load_float(g_release_position_bits), g_release_pre_state.load(),
        g_release_pre_timing_enum.load(), load_float(g_release_arg0_bits),
        load_float(g_release_arg1_bits), g_eligible_local_release_events.load(),
        g_shader_init_code.load(), g_result_timing.load(),
        g_result_sequence.load(), g_release_event_calls.load(),
        g_last_release_timing.load(), g_perfect_release_events.load(),
        perfect_active ? 1U : 0U, g_perfect_flash_frames.load(),
        g_sound_init_code.load(), g_sound_play_count.load(),
        g_perfect_fire_trigger_count.load(), g_perfect_fire_draw_calls.load(),
        g_perfect_fire_frames.load(), g_perfect_fire_active.load(),
        g_surface_w.load(), g_surface_h.load(), g_viewport_w.load(),
        g_viewport_h.load(), g_framebuffer.load(),
        reinterpret_cast<void *>(g_egl_context_value.load()),
        reinterpret_cast<void *>(g_egl_display_value.load()),
        reinterpret_cast<void *>(g_egl_surface_value.load()),
        glVersion, glRenderer, glVendor, glslVersion,
        g_gl_init_stage.load(), g_gl_error_code.load(), g_gl_error_count.load(),
        g_gl_last_error_stage.load(), g_gl_pre_overlay_error_count.load(),
        g_gl_vertex_status.load(), g_gl_fragment_status.load(), g_gl_link_status.load(),
        glShaderLog, g_gl_draw_error_code.load(), g_gl_draw_error_count.load(),
        g_gl_init_attempts.load(),
        g_probability_map_self_test_pass.load() ? "PASS" : "FAIL",
        g_probability_map_self_test_cases.load(),
        g_probability_enabled.load(std::memory_order_acquire),
        g_probability_enabled.load(std::memory_order_acquire)
            ? "GRADE_AWARE_OVERRIDE_ACTIVE" : "PROBABILITY_HOOK_NOT_INSTALLED",
        g_balanced_dice_address, g_probability_hook_code,
        g_probability_hook_calls.load(), g_probability_modified_shots.load(),
        g_probability_invalid_context.load(),
        load_float(g_probability_last_original_bits),
        load_float(g_probability_last_applied_bits),
        load_float(g_probability_last_distance_bits),
        g_probability_last_grade.load(), g_probability_last_timing.load(),
        status);
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
    uint32_t probability_cases = 0;
    const bool probability_map_ok = probability_mapping_self_test(&probability_cases);
    g_probability_map_self_test_pass.store(probability_map_ok ? 1U : 0U, std::memory_order_release);
    g_probability_map_self_test_cases.store(probability_cases, std::memory_order_release);
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
        g_balanced_dice_address = resolve(g_game, kBalancedDiceSymbol);
        g_get_location_address = resolve(g_game, kGetLocationSymbol);
        g_distance_address = resolve(g_game, kDistanceSymbol);
        g_grade_address = resolve(g_game, kGradeSymbol);
        g_get_head = reinterpret_cast<HeadFn>(g_head_address);
        g_project = reinterpret_cast<ProjectFn>(g_project_address);
        g_get_location = reinterpret_cast<GetLocationFn>(g_get_location_address);
        g_get_distance = reinterpret_cast<DistanceFn>(g_distance_address);
        g_translate_grade = reinterpret_cast<GradeFn>(g_grade_address);
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
        if (g_balanced_dice_address && g_get_location && g_get_distance && g_translate_grade) {
            void *original_balanced = nullptr;
            g_probability_hook_code = DobbyHook(g_balanced_dice_address,
                (void *)&balanced_dice_proxy, &original_balanced);
            g_original_balanced_dice = reinterpret_cast<BalancedDiceFn>(original_balanced);
            if (g_probability_hook_code == 0 && g_original_balanced_dice && probability_map_ok) {
                g_probability_enabled.store(1, std::memory_order_release);
            } else {
                g_probability_enabled.store(0, std::memory_order_release);
            }
        } else {
            g_probability_hook_code = -1005;
            g_probability_enabled.store(0, std::memory_order_release);
        }
    } else {
        g_do_draw_code = -1001;
        g_swap_code = -1001;
    }

    store_float(g_perfect_raw_reference_bits, kPerfectRawInitial);
    const bool sound_ready = initialize_sound();
    if (sound_ready) {
        pthread_t sound_thread;
        if (pthread_create(&sound_thread, nullptr, sound_worker, nullptr) == 0)
            pthread_detach(sound_thread);
        else g_sound_init_code.store(-2002, std::memory_order_release);
    }
    LOGI("v55 embedded Green Giant audio init=%s code=%d", sound_ready ? "ready" : "unavailable", g_sound_init_code.load());
    g_init_state.store(2, std::memory_order_release);
    write_diagnostic();
    pthread_t thread;
    if (pthread_create(&thread, nullptr, reporter, nullptr) == 0) pthread_detach(thread);
    else LOGE("diagnostic reporter thread creation failed");
    LOGI("v54 grade-aware probability agent attached; probability_hook_code=%d enabled=%u",
         g_probability_hook_code, g_probability_enabled.load());
    return JNI_OK;
}
extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *) {
    LOGI("v52 grade-aware probability agent unloading");
}

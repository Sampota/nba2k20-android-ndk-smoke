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
#include <string>
#include <stdlib.h>
#include <time.h>
#include <vector>

#include "dobby.h"
#include "shotmeter_plugin_api.h"
#include "greengiant_ima_b64.h"
#include "../Files/horn_font_atlas.h"

// v67 CI output-path verification was corrected in the companion workflow.
#define LOG_TAG "ShotmeterDobbyV71"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAG_NAME "shotmeter.dobby-v71-green-fps"

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
using PushTouchEventFn = void (*)(void *, int32_t, int32_t, float, float, int32_t);
using TestFn = int (*)(int);
using ReleaseFn = void (*)(void *, int32_t, float, float);
using BalancedDiceFn = void (*)(void *, void *, int32_t, float, bool, int32_t, int32_t);
using GetLocationFn = void (*)(const void *, float *);
using DistanceFn = float (*)(const float *);
using GradeFn = int32_t (*)(float, float);

static void *g_game = nullptr;
static const char *g_game_name = "not-found";
static uintptr_t g_saved_view = 0;
static volatile uint8_t *g_component = nullptr;
static void *g_do_draw_address = nullptr;
static void *g_head_address = nullptr;
static void *g_saved_getter_address = nullptr;
static void *g_project_address = nullptr;
static void *g_swap_address = nullptr;
static void *g_push_touch_event_address = nullptr;
static void *g_release_event_address = nullptr;
static DoDrawFn g_original_do_draw = nullptr;
static HeadFn g_get_head = nullptr;
static ProjectFn g_project = nullptr;
static SwapFn g_original_swap = nullptr;
static PushTouchEventFn g_original_push_touch_event = nullptr;
static ReleaseFn g_original_release_event = nullptr;
static int g_touch_event_hook_code = -999;
static std::atomic<uint32_t> g_touch_event_counts[4]{};
static std::atomic<uint32_t> g_touch_event_invalid{0};
static std::atomic<int32_t> g_ui_touch_pointer_id{-1};
static std::atomic<uint32_t> g_ui_touch_down{0};
static std::atomic<uint32_t> g_ui_tap_sequence{0};
static std::atomic<uint32_t> g_ui_tap_consumed{0};
static std::atomic<uint32_t> g_ui_tap_x_bits{0}, g_ui_tap_y_bits{0};
static std::atomic<uint32_t> g_ui_touch_x_bits{0}, g_ui_touch_y_bits{0};
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
static std::atomic<uint32_t> g_probability_timing_calls[6]{};
static std::atomic<uint32_t> g_probability_timing_invalid_context[6]{};
static std::atomic<uint32_t> g_probability_grade_index_counts[2][13]{};
static std::atomic<uint32_t> g_probability_timing_last_original_bits[6]{};
static std::atomic<uint32_t> g_probability_timing_last_applied_bits[6]{};
static std::atomic<uint32_t> g_probability_timing_last_distance_bits[6]{};
static std::atomic<int32_t> g_probability_timing_last_grade[6]{};
static std::atomic<int32_t> g_probability_timing_last_status[6]{};
static std::atomic<uint32_t> g_probability_last_original_bits{0}, g_probability_last_applied_bits{0};
static std::atomic<uint32_t> g_probability_last_distance_bits{0};
static std::atomic<uint32_t> g_probability_last_location_x_bits{0};
static std::atomic<uint32_t> g_probability_last_location_y_bits{0};
static std::atomic<uint32_t> g_probability_last_location_z_bits{0};
static std::atomic<int32_t> g_probability_last_grade{-1}, g_probability_last_timing{-1};
static std::atomic<int32_t> g_probability_last_grade_status{0};
static std::atomic<uint32_t> g_probability_grade_validation_self_test_pass{0};
static std::atomic<uint32_t> g_probability_grade_validation_self_test_cases{0};

static int g_self_code = -999, g_self_base = -999;
static int g_self_original = -999, g_self_patched = -999;
static int g_do_draw_code = -999, g_swap_code = -999, g_release_event_code = -999;
static std::atomic<uint32_t> g_attach_calls{0}, g_duplicate_attach_calls{0};
static std::atomic<uint32_t> g_init_state{0};
static uintptr_t g_game_base = 0;
static void *g_pot_options_rows = nullptr;
static void *g_pot_page_descriptor = nullptr;
static void *g_pot_page_rows = nullptr;
static void *g_pot_toggle_interface = nullptr;
static std::atomic<int32_t> g_pot_menu_install_state{0};
static std::atomic<uint32_t> g_plugin_enabled{0};
static std::atomic<uint32_t> g_customization_open{0};
static std::atomic<uint32_t> g_plugin_settings_loaded{0};
static std::atomic<uint32_t> g_plugin_settings_modified{0};
static std::atomic<uint32_t> g_customization_tab{0};
static std::atomic<uint32_t> g_selected_design{0};
static std::atomic<uint32_t> g_selected_audio{0};
static std::atomic<uint32_t> g_selected_green_animation{0};
static pthread_mutex_t g_plugin_settings_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<uint32_t> g_do_draw_calls{0}, g_projection_calls{0};
static std::atomic<uint32_t> g_projection_invalid{0};
static std::atomic<uint32_t> g_projection_valid{0};
static std::atomic<uint32_t> g_active_meter_frames{0}, g_draw_calls{0};
static std::atomic<uint32_t> g_release_event_calls{0}, g_perfect_release_events{0}, g_perfect_flash_frames{0};
static std::atomic<uint32_t> g_release_timing_counts[6]{};
static std::atomic<uint32_t> g_suspected_duplicate_release_events{0};
static std::atomic<uintptr_t> g_last_release_actor{0};
static std::atomic<int32_t> g_last_release_signature_timing{-1};
static std::atomic<uint32_t> g_last_release_signature_arg0{0}, g_last_release_signature_arg1{0};
static std::atomic<uint64_t> g_last_release_signature_ns{0};
static std::atomic<int32_t> g_last_release_timing{-1};
static std::atomic<uint64_t> g_perfect_until_ns{0};
static std::atomic<uint64_t> g_result_until_ns{0};
static std::atomic<uint64_t> g_result_started_ns{0};

static std::atomic<uint32_t> g_result_sequence{0};
static std::atomic<int32_t> g_result_timing{-1};
static std::atomic<uint32_t> g_result_raw_bits{0};
static std::atomic<uint32_t> g_anchor_valid{0};
static std::atomic<uint32_t> g_anchor_x_bits{0}, g_anchor_y_bits{0};
static std::atomic<uint32_t> g_meter_origin_x_bits{0}, g_meter_origin_y_bits{0};
static std::atomic<uint32_t> g_flame_axis_x_bits{0}, g_flame_base_y_bits{0};

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
static std::atomic<int32_t> g_gl_init_stage{0};











static std::atomic<uint32_t> g_perfect_fire_trigger_count{0}, g_perfect_fire_draw_calls{0};
static std::atomic<uint32_t> g_perfect_fire_frames{0}, g_perfect_fire_active{0};
static std::atomic<uint32_t> g_perfect_fire_draw_failures{0};
static std::atomic<int32_t> g_gl_vertex_status{-1}, g_gl_fragment_status{-1}, g_gl_link_status{-1};

static std::atomic<uint32_t> g_gl_init_attempts{0};
static std::atomic<uintptr_t> g_egl_context_value{0}, g_egl_display_value{0}, g_egl_surface_value{0};

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

enum ProbabilityGradeStatus : int32_t {
    kGradeNotRequired = 0,
    kGradeValid = 1,
    kGradeMissingActor = 2,
    kGradeMissingFunction = 3,
    kGradeInvalidLaunchChance = 4,
    kGradeNonfiniteLocation = 5,
    kGradeNonfiniteDistance = 6,
    kGradeNegativeDistance = 7,
    kGradeIndexOutOfRange = 8
};

static bool valid_grade_distance(float distance) {
    return std::isfinite(distance) && distance >= 0.0f;
}
static bool valid_grade_index(int32_t grade) {
    return grade >= 0 && grade < 13;
}
static const char *probability_grade_status_name(int32_t status) {
    switch (status) {
        case kGradeNotRequired: return "NOT_REQUIRED_FOR_TIMING_CLASS";
        case kGradeValid: return "VALID_GRADE";
        case kGradeMissingActor: return "MISSING_ACTOR";
        case kGradeMissingFunction: return "MISSING_GRADE_FUNCTION";
        case kGradeInvalidLaunchChance: return "INVALID_LAUNCH_CHANCE";
        case kGradeNonfiniteLocation: return "NONFINITE_PLAYER_LOCATION";
        case kGradeNonfiniteDistance: return "NONFINITE_BASKET_DISTANCE";
        case kGradeNegativeDistance: return "NEGATIVE_BASKET_DISTANCE";
        case kGradeIndexOutOfRange: return "GRADE_INDEX_OUT_OF_RANGE";
        default: return "UNKNOWN_GRADE_STATUS";
    }
}
static bool grade_validation_self_test(uint32_t *case_count) {
    uint32_t checked = 0;
    const float distances[] = {0.0f, 12.5f, NAN, INFINITY, -INFINITY, -0.01f};
    const bool expected_distance[] = {true, true, false, false, false, false};
    for (size_t i = 0; i < sizeof(distances) / sizeof(distances[0]); ++i) {
        if (valid_grade_distance(distances[i]) != expected_distance[i]) return false;
        ++checked;
    }
    const int32_t grades[] = {0, 12, -1, 13};
    const bool expected_grade[] = {true, true, false, false};
    for (size_t i = 0; i < sizeof(grades) / sizeof(grades[0]); ++i) {
        if (valid_grade_index(grades[i]) != expected_grade[i]) return false;
        ++checked;
    }
    const float expected_chances[13] = {
        0.000f, 0.005f, 0.010f, 0.020f, 0.025f,
        0.035f, 0.050f, 0.100f, 0.175f, 0.250f,
        0.350f, 0.510f, 0.670f
    };
    for (int32_t grade = 0; grade < 13; ++grade) {
        if (!std::isfinite(kChanceByGrade[grade]) ||
            kChanceByGrade[grade] < 0.0f || kChanceByGrade[grade] > 1.0f ||
            std::fabs(kChanceByGrade[grade] - expected_chances[grade]) > 0.000001f)
            return false;
        if (grade > 0 && kChanceByGrade[grade] < kChanceByGrade[grade - 1])
            return false;
        ++checked;
    }
    if (case_count) *case_count = checked;
    return checked == 23;
}

__attribute__((noinline))
static void balanced_dice_proxy(void *actor, void *ball, int32_t shot_type,
                                float original_chance, bool bank_shot,
                                int32_t trajectory, int32_t timing) {
    float applied_chance = original_chance;
    float distance = NAN;
    float location_x = NAN, location_y = NAN, location_z = NAN;
    int32_t grade = -1;
    int32_t grade_status = kGradeNotRequired;
    bool modified = false;

    if (g_plugin_enabled.load(std::memory_order_acquire) != 0 &&
        g_probability_enabled.load(std::memory_order_acquire) != 0 &&
        timing >= 1 && timing <= 5) {
        /* At PHY_LaunchShotBalancedDiceRoll, s0 is the exact chance entering
           simulation and w5 is the native release-timing enum. A grade is
           required only for Slight Early/Late (classes 2 and 4); Very
           Early/Late and Excellent do not consume a grade. The chance stored
           by the game for HUD feedback is never written here. */
        if (timing == 2 || timing == 4) {
            if (!actor) {
                grade_status = kGradeMissingActor;
            } else if (!g_get_location || !g_get_distance || !g_translate_grade) {
                grade_status = kGradeMissingFunction;
            } else if (!std::isfinite(original_chance) ||
                       original_chance < 0.0f || original_chance > 1.0f) {
                grade_status = kGradeInvalidLaunchChance;
            } else {
                alignas(16) float location[4] = {NAN, NAN, NAN, 1.0f};
                g_get_location(actor, location);
                location_x = location[0];
                location_y = location[1];
                location_z = location[2];
                if (!std::isfinite(location_x) || !std::isfinite(location_y) ||
                    !std::isfinite(location_z)) {
                    grade_status = kGradeNonfiniteLocation;
                } else {
                    distance = g_get_distance(location);
                    if (!std::isfinite(distance)) {
                        grade_status = kGradeNonfiniteDistance;
                    } else if (distance < 0.0f) {
                        grade_status = kGradeNegativeDistance;
                    } else {
                        grade = g_translate_grade(original_chance, distance);
                        grade_status = valid_grade_index(grade)
                            ? kGradeValid : kGradeIndexOutOfRange;
                    }
                }
            }
        }

        modified = map_timing_probability(timing, grade, &applied_chance);
        g_probability_hook_calls.fetch_add(1, std::memory_order_relaxed);
        g_probability_timing_calls[timing].fetch_add(1, std::memory_order_relaxed);
        if (modified) {
            g_probability_modified_shots.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_probability_invalid_context.fetch_add(1, std::memory_order_relaxed);
            g_probability_timing_invalid_context[timing].fetch_add(1, std::memory_order_relaxed);
        }
        store_float(g_probability_timing_last_original_bits[timing], original_chance);
        store_float(g_probability_timing_last_applied_bits[timing], applied_chance);
        store_float(g_probability_timing_last_distance_bits[timing], distance);
        g_probability_timing_last_grade[timing].store(grade, std::memory_order_relaxed);
        g_probability_timing_last_status[timing].store(grade_status, std::memory_order_relaxed);
        if ((timing == 2 || timing == 4) && grade_status == kGradeValid)
            g_probability_grade_index_counts[timing == 2 ? 0 : 1][grade].fetch_add(
                1, std::memory_order_relaxed);

        store_float(g_probability_last_original_bits, original_chance);
        store_float(g_probability_last_applied_bits, applied_chance);
        store_float(g_probability_last_distance_bits, distance);
        store_float(g_probability_last_location_x_bits, location_x);
        store_float(g_probability_last_location_y_bits, location_y);
        store_float(g_probability_last_location_z_bits, location_z);
        g_probability_last_grade.store(grade, std::memory_order_relaxed);
        g_probability_last_timing.store(timing, std::memory_order_relaxed);
        g_probability_last_grade_status.store(grade_status, std::memory_order_relaxed);
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
    const uintptr_t gameBase = reinterpret_cast<uintptr_t>(info.dli_fbase);
    g_game_base = gameBase;
    g_saved_view = gameBase + kManagerOffset + kSavedViewOffset;
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
        g_projection_invalid.fetch_add(1, std::memory_order_relaxed);
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
static constexpr float kMeterShiftDesignX = 90.0f;
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
uniform float uScale, uFill, uFlash, uAlpha;
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
    if (d > 20.0 && chip < 0.01) {
        gl_FragColor = vec4(0.0); return;
    }

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
static const char* kClaudeFlameFS = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uNoise;                       // tileable noise (alpha channel), wraps
uniform vec2  uOrigin;                          // pixels: x of the flame axis, y of the box bottom
uniform float uPx;                              // pixels per flame unit
uniform float uTime, uPower, uFlash, uAlpha;    // clock, size 0..1.35, flash 0..1, fade 0..1

float tx(vec2 p) { return texture2D(uNoise, p).a; }

// only used for the four sparks
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

void main() {
    const float BASE = 0.20;
    vec2  p = (gl_FragCoord.xy - uOrigin) / uPx;
    float HF = 0.40 + 0.16 * uPower;            // flame height (flame units): wide, not stretched
    float h  = (p.y - BASE) / HF;               // 0 at the base .. 1 at the tip
    float t  = uTime;
    float strobe = uFlash * (0.65 + 0.35 * sin(t * 40.0));   // flashing while the flash lasts
    float flick  = 0.94 + 0.06 * sin(t * 13.0) + 0.04 * sin(t * 23.0 + 1.3);
    float wmax   = 0.20 * (0.85 + 0.15 * uPower) * flick;   // half width at the base

    // soft green glow around the flame. It is faded to exactly zero toward the box edges
    // (win), so the box edge never shows, even during the bright flash.
    float edgeD = min(min(0.5 - abs(p.x), p.y), 1.0 - p.y);   // distance to the nearest box edge
    float win   = smoothstep(0.0, 0.25, edgeD);
    float g = exp(-length(vec2(p.x * 2.0, (p.y - BASE - 0.12) * 1.3)) * 4.2)
              * (0.16 + 0.28 * uPower + 0.9 * strobe) * win;
    vec4 res = vec4(vec3(0.10, 0.90, 0.25) * g, g);

    // the noise reads only happen inside the flame's envelope
    if (abs(p.x) < wmax * 1.9 && h > -0.15 && h < 1.1) {
        // slow sway of the whole flame, growing toward the tip
        float sway = sin(t * 1.7 + h * 3.0) * 0.04 * h + (tx(vec2(t * 0.05, h * 0.7)) - 0.5) * 0.10 * h;
        float x = p.x - sway;

        // envelope: wide at the base, narrowing to a thin wispy tip, soft edges
        float w  = wmax * pow(max(1.0 - h, 0.0), 0.9) * (0.55 + 0.45 * (1.0 - h));
        float m  = 1.0 - smoothstep(0.35, 1.0, abs(x) / max(w, 0.005));
        float vm = smoothstep(-0.08, 0.12, h) * (1.0 - smoothstep(0.7, 1.0, h));

        // coordinates scroll upward (faster during the flash) ...
        vec2 q = vec2(x * 2.6, h * 1.5 - t * 0.5 * (1.0 + 0.35 * uFlash));
        // ... and are bent by two noise lookups, which makes the tongues curl and flow
        vec2 wv = vec2(tx(q * 0.55 + vec2(0.0, -t * 0.18)),
                       tx(q * 0.55 + vec2(0.37, 0.61 - t * 0.21))) - 0.5;
        vec2 qq = q + wv * (0.55 + 1.1 * h);

        // ridged noise: thin bright lines where the noise crosses its middle value = filaments
        float n1 = tx(qq * 1.15);
        float r1 = pow(1.0 - abs(n1 * 2.0 - 1.0), 3.0);
#if FLAME_QUALITY >= 2
        float n2  = tx(qq * 2.3 + vec2(3.1, 1.7));
        float fil = r1 * 0.75 + pow(1.0 - abs(n2 * 2.0 - 1.0), 4.0) * 0.5;
#else
        float fil = r1 * 0.95;
#endif
        float body = tx(qq * 0.8 + vec2(1.3, 4.1));            // soft volume under the filaments
        float d = m * vm * (0.30 * body * (1.0 - 0.5 * h) + 0.95 * fil);

        // bright hot column in the lower middle
        float cx   = x / (wmax * 0.45);
        float core = exp(-cx * cx) * (1.0 - smoothstep(0.05, 0.7, h)) * (0.6 + 0.4 * body) * vm;

        float dens = clamp(d + core * 0.55, 0.0, 1.0);
        float a    = smoothstep(0.04, 0.55, dens);

        // color: dark green edge -> bright green body -> pale green hot parts
        vec3 col = mix(vec3(0.00, 0.42, 0.10), vec3(0.12, 0.88, 0.28), smoothstep(0.05, 0.45, dens));
        col = mix(col, vec3(0.78, 1.00, 0.80), smoothstep(0.45, 1.0, dens) * (0.7 + 0.3 * strobe));
        col = mix(col, vec3(0.60, 0.95, 0.12), 0.35 * (1.0 - smoothstep(0.0, 0.22, h)));   // yellow-green at the base
        col *= 1.0 + 0.8 * strobe;                              // brighter during the flash
        res = vec4(col * a, a) + res * (1.0 - a);
    }

    // four small sparks rising from the flame (only when the flame is big or flashing)
    float spk = clamp(uPower - 0.45, 0.0, 1.0) + uFlash;
    for (int i = 0; i < 4; i++) {
        float fi   = float(i);
        float seed = hash(vec2(fi, 3.7));
        float life = fract(t * (0.45 + 0.25 * seed) + seed * 5.0);   // 0 at birth .. 1 at death
        vec2  sp   = vec2((seed - 0.5) * 0.30 + sin(t * 3.0 + fi * 2.0) * 0.03 * life,
                          BASE + 0.08 + life * (0.30 + 0.25 * uPower));
        float s = (1.0 - smoothstep(0.0, 0.02, length(p - sp))) * (1.0 - life) * spk;
        res += vec4(vec3(0.60, 1.00, 0.60) * s, s);
    }

    res *= smoothstep(0.0, 0.05, edgeD);                    // safety: nothing is ever cut by the box edge
    res.a   = min(res.a, 1.0);
    res.rgb = min(res.rgb, vec3(1.0));
    gl_FragColor = res * uAlpha;                            // premultiplied alpha output
}
)";

static struct {
    EGLContext ctx = EGL_NO_CONTEXT;
    GLuint prog = 0, bannerProg = 0, vbo = 0, vao = 0, labelTex = 0, bannerTex = 0;
    GLuint flameProg = 0, flameTex = 0;
    GLint fOrigin = -1, fPx = -1, fTime = -1, fPower = -1, fFlash = -1, fAlpha = -1;
    bool flameOk = false;
    GLint uOrigin = -1, uScale = -1, uFill = -1, uFlash = -1, uAlpha = -1;
    GLint uDirFill = -1, uDirCap = -1, uTint = -1, uLabelTint = -1;
    GLint uLabelSize = -1, uLabelTex = -1;
    GLint bOrigin = -1, bScale = -1, bAlpha = -1, bPop = -1, bSdfK = -1;
    GLint bTint = -1, bAtlas = -1, bTextOrigin = -1, bCellSize = -1, bTexV = -1;
    bool ok = false, es3 = false, bannerOk = false;
    int labelTiming = -99;
    float labelWidth = 0.0f;
} G;

/* Clear stale GL errors left by the game before our own resource checks. v65's
   diagnostic checkpoints incidentally consumed these errors; v66 removed those
   checkpoints, allowing an unrelated game error to make our resource setup fail. */
static void discard_stale_gl_errors(void) {
    for (int i = 0; i < 16; ++i) {
        if (glGetError() == GL_NO_ERROR) break;
    }
}

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
    
    if (!shader) {
        set_shader_log("glCreateShader returned 0");
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    
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



static constexpr int kClaudeNoiseSize = 256;
static constexpr float kClaudeFlameSizeFrac = 0.12f;
static uint8_t g_claudeNoiseRGBA[kClaudeNoiseSize * kClaudeNoiseSize * 4]{};
static bool g_claudeNoiseReady = false;
static unsigned claude_hash2(unsigned x, unsigned y, unsigned seed) {
    unsigned h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float claude_perlin(float x, float y, int grid, unsigned seed) {
    const float fx=x*grid, fy=y*grid;
    const int ix=static_cast<int>(floorf(fx)), iy=static_cast<int>(floorf(fy));
    const float tx=fx-ix, ty=fy-iy;
    auto grad=[&](int cx,int cy,float dx,float dy) {
        const unsigned h=claude_hash2(static_cast<unsigned>(cx%grid),static_cast<unsigned>(cy%grid),seed);
        const float a=static_cast<float>(h&0xFFFFu)*(6.2831853f/65536.0f);
        return cosf(a)*dx+sinf(a)*dy;
    };
    const float u=tx*tx*tx*(tx*(tx*6.0f-15.0f)+10.0f);
    const float v=ty*ty*ty*(ty*(ty*6.0f-15.0f)+10.0f);
    const float n00=grad(ix,iy,tx,ty), n10=grad(ix+1,iy,tx-1.0f,ty);
    const float n01=grad(ix,iy+1,tx,ty-1.0f), n11=grad(ix+1,iy+1,tx-1.0f,ty-1.0f);
    const float a=n00+(n10-n00)*u, b=n01+(n11-n01)*u;
    return a+(b-a)*v;
}
static void claude_bake_noise() {
    if(g_claudeNoiseReady)return;
    for(int y=0;y<kClaudeNoiseSize;++y)for(int x=0;x<kClaudeNoiseSize;++x){
        const float fx=static_cast<float>(x)/kClaudeNoiseSize, fy=static_cast<float>(y)/kClaudeNoiseSize;
        float v=0.50f*claude_perlin(fx,fy,4,1)+0.30f*claude_perlin(fx,fy,8,2)
               +0.15f*claude_perlin(fx,fy,16,3)+0.08f*claude_perlin(fx,fy,32,4);
        v=std::max(0.0f,std::min(1.0f,0.5f+v*1.1f));
        const uint8_t q=static_cast<uint8_t>(std::lround(v*255.0f));
        const size_t i=(static_cast<size_t>(y)*kClaudeNoiseSize+static_cast<size_t>(x))*4U;
        g_claudeNoiseRGBA[i]=255;g_claudeNoiseRGBA[i+1]=255;g_claudeNoiseRGBA[i+2]=255;g_claudeNoiseRGBA[i+3]=q;
    }
    g_claudeNoiseReady=true;
}
static bool init_claude_flame_gl() {
    discard_stale_gl_errors();
    GLint oldProgram=0,oldActive=GL_TEXTURE0,oldTexture=0,oldUnpack=4,oldSampler=0;
    glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram);glGetIntegerv(GL_ACTIVE_TEXTURE,&oldActive);
    glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT,&oldUnpack);if(G.es3)glGetIntegerv(GL_SAMPLER_BINDING,&oldSampler);
    auto restore=[&](){
        glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(oldTexture));
        if(G.es3)glBindSampler(0,static_cast<GLuint>(oldSampler));
        glPixelStorei(GL_UNPACK_ALIGNMENT,oldUnpack);glActiveTexture(oldActive);
        glUseProgram(static_cast<GLuint>(oldProgram));
    };
    std::string vsSource,fsSource;
    if(G.es3){
        vsSource="#version 300 es\nprecision highp float;\nin vec2 aPos;\nvoid main(){gl_Position=vec4(aPos,0.0,1.0);}\n";
        fsSource="#version 300 es\n#define FLAME_QUALITY 1\n";fsSource+=kClaudeFlameFS;
        replace_all(fsSource,"texture2D(","texture(");replace_all(fsSource,"gl_FragColor","fragColor");
        const size_t p=fsSource.find("\n#endif\n");
        if(p==std::string::npos){set_shader_log("Claude flame ES3 precision conversion failed");restore();return false;}
        fsSource.insert(p+8,"\nout vec4 fragColor;\n");
    }else{vsSource=kHornVS;fsSource="#define FLAME_QUALITY 1\n";fsSource+=kClaudeFlameFS;}
    GLuint vs=compile_horn_shader(GL_VERTEX_SHADER,vsSource.c_str(),50);
    GLuint fs=compile_horn_shader(GL_FRAGMENT_SHADER,fsSource.c_str(),52);
    if(!vs||!fs){if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);restore();return false;}
    GLuint prog=glCreateProgram();
    if(!prog){glDeleteShader(vs);glDeleteShader(fs);set_shader_log("Claude flame program creation failed");restore();return false;}
    glAttachShader(prog,vs);glAttachShader(prog,fs);glBindAttribLocation(prog,0,"aPos");glLinkProgram(prog);
    glDeleteShader(vs);glDeleteShader(fs);
    GLint linked=GL_FALSE;glGetProgramiv(prog,GL_LINK_STATUS,&linked);
    if(linked!=GL_TRUE){
        char log[1024]={};GLsizei len=0;glGetProgramInfoLog(prog,sizeof(log),&len,log);
        set_shader_log(log[0]?log:"Claude flame program link failed");glDeleteProgram(prog);restore();return false;
    }
    const GLint uOrigin=glGetUniformLocation(prog,"uOrigin"),uPx=glGetUniformLocation(prog,"uPx");
    const GLint uTime=glGetUniformLocation(prog,"uTime"),uPower=glGetUniformLocation(prog,"uPower");
    const GLint uFlash=glGetUniformLocation(prog,"uFlash"),uAlpha=glGetUniformLocation(prog,"uAlpha");
    const GLint uNoise=glGetUniformLocation(prog,"uNoise");
    if(uOrigin<0||uPx<0||uTime<0||uPower<0||uFlash<0||uAlpha<0||uNoise<0){
        set_shader_log("Claude flame uniforms unavailable");glDeleteProgram(prog);restore();return false;
    }
    claude_bake_noise();
    GLuint tex=0;glGenTextures(1,&tex);
    if(tex){
        glBindTexture(GL_TEXTURE_2D,tex);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,kClaudeNoiseSize,kClaudeNoiseSize,0,GL_RGBA,GL_UNSIGNED_BYTE,g_claudeNoiseRGBA);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    }
    const GLenum err=glGetError();
    if(!tex||err!=GL_NO_ERROR){
        set_shader_log("Claude flame noise texture creation failed");if(tex)glDeleteTextures(1,&tex);
        glDeleteProgram(prog);restore();return false;
    }
    glUseProgram(prog);glUniform1i(uNoise,0);
    G.flameProg=prog;G.flameTex=tex;G.fOrigin=uOrigin;G.fPx=uPx;G.fTime=uTime;
    G.fPower=uPower;G.fFlash=uFlash;G.fAlpha=uAlpha;G.flameOk=true;
    restore();return true;
}
static bool draw_claude_flame(int surfaceH,int anchorX,int anchorY,float scale,float elapsed) {
    if(!G.flameOk||surfaceH<=0||!std::isfinite(elapsed)||elapsed<0.0f)return false;
    constexpr float flashTime=0.6f,hotTime=1.5f,fadeTime=0.9f;
    const float age=elapsed;
    float hot=1.0f;
    if(age>hotTime){const float u=std::max(0.0f,std::min(1.0f,(age-hotTime)/fadeTime));hot=1.0f-u*u*(3.0f-2.0f*u);}
    if(age>hotTime+fadeTime)hot=0.0f;
    const float flash=std::max(0.0f,std::min(1.0f,1.0f-age/flashTime));
    const float power=hot+flash*0.35f,alpha=hot*std::min(1.0f,age/0.08f);
    if(power<=0.001f||alpha<=0.002f)return false;
    const float px=kClaudeFlameSizeFrac*static_cast<float>(surfaceH);
    if(px<2.0f)return false;
    /* Keep the flame anchored to the projected head, independent of meter shift. */
    const float axisX=static_cast<float>(anchorX); // center on projected player head
    const float baseY=static_cast<float>(anchorY)+51.0f*scale;
    store_float(g_flame_axis_x_bits, axisX);
    store_float(g_flame_base_y_bits, baseY);
    const float x0=axisX-0.5f*px,y0=baseY-0.20f*px;
    GLint oldVp[4]={0,0,0,0},oldActive=GL_TEXTURE0,oldTex=0,oldSampler=0;
    glGetIntegerv(GL_VIEWPORT,oldVp);glGetIntegerv(GL_ACTIVE_TEXTURE,&oldActive);
    glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTex);
    if(G.es3){glGetIntegerv(GL_SAMPLER_BINDING,&oldSampler);glBindSampler(0,0);}
    glBindTexture(GL_TEXTURE_2D,G.flameTex);
    glViewport(static_cast<GLint>(floorf(x0)),static_cast<GLint>(floorf(y0)),
               static_cast<GLsizei>(ceilf(px)+1.0f),static_cast<GLsizei>(ceilf(px)+1.0f));
    glUseProgram(G.flameProg);glUniform2f(G.fOrigin,axisX,y0);glUniform1f(G.fPx,px);
    glUniform1f(G.fTime,age);glUniform1f(G.fPower,power);glUniform1f(G.fFlash,flash);glUniform1f(G.fAlpha,alpha);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    const GLenum err = glGetError();
    if(G.es3)glBindSampler(0,static_cast<GLuint>(oldSampler));
    glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(oldTex));glActiveTexture(oldActive);
    glViewport(oldVp[0],oldVp[1],oldVp[2],oldVp[3]);
    return err==GL_NO_ERROR;
}

static bool init_banner_gl(void) {
    discard_stale_gl_errors();
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
    discard_stale_gl_errors();
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
        
        set_shader_log("glGetString(GL_VERSION) returned null");
        return -101;
    }
    G.es3 = strstr(reinterpret_cast<const char *>(version), "OpenGL ES 3") != nullptr;
    
    GLint oldProgram = 0, oldBuffer = 0, oldVao = 0, oldActive = GL_TEXTURE0, oldTexture = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    if (G.es3) glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    
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
        
        return !vs ? -110 : -120;
    }

    g_gl_init_stage.store(30, std::memory_order_relaxed);
    G.prog = glCreateProgram();
    
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
    if (G.uOrigin < 0 || G.uScale < 0 || G.uFill < 0 || G.uFlash < 0 ||
        G.uAlpha < 0 || G.uDirFill < 0 || G.uDirCap < 0 || G.uTint < 0 ||
        G.uLabelTint < 0 || G.uLabelSize < 0 || G.uLabelTex < 0 ||
        G.uLabelTint < 0 || G.uLabelSize < 0 || G.uLabelTex < 0) {
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
    const GLenum resourceError = glGetError();
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
    G.flameOk = init_claude_flame_gl();
    if (!G.flameOk) LOGE("Claude green flame renderer unavailable; meter/banner remain active");
    
    glBindTexture(GL_TEXTURE_2D, oldTexture);
    glActiveTexture(oldActive);
    glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
    glUseProgram(oldProgram);
    
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

    const float scale = (160.0f * std::max(0.72f, std::min(1.35f, surfaceH / 720.0f))) / 319.0f;
    /* Retain the v50-v53 anchor math, but shift farther right to clear the shooting hand. */
    const float meterShiftX = kMeterShiftDesignX * scale;
    const float meterShiftDown = 0.5f * scale;
    const float bx = anchorX + 43.0f*scale - 123.5f*scale + meterShiftX;
    store_float(g_meter_origin_x_bits, bx);
    const float topY = anchorY + 48.0f*scale - meterShiftDown;
    const float by = topY - 319.0f*scale;
    store_float(g_meter_origin_y_bits, by);
    const GLint vx = static_cast<GLint>(floorf(bx + 60.0f*scale));
    const GLint vy = static_cast<GLint>(floorf(by + 19.0f*scale));
    const GLsizei vw = static_cast<GLsizei>(ceilf(187.0f*scale)+1.0f);
    const GLsizei vh = static_cast<GLsizei>(ceilf(300.0f*scale)+1.0f);

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
    glUniform1f(G.uAlpha, 1.0f); // restore meter opacity after any prior fire-only pass
    glUniform2f(G.uDirFill,
        cosf(kHornAngBot + (kHornAngTop-kHornAngBot)*progress),
        sinf(kHornAngBot + (kHornAngTop-kHornAngBot)*progress));
    glUniform3f(G.uTint, 0.21f, 0.82f, 0.23f);
    if (resultTiming == 3) glUniform3f(G.uLabelTint, 0.21f, 0.82f, 0.23f);
    else if (resultTiming == 2 || resultTiming == 4) glUniform3f(G.uLabelTint, 1.0f, 0.60f, 0.10f);
    else glUniform3f(G.uLabelTint, 0.90f, 0.15f, 0.15f);
    glUniform2f(G.uLabelSize, G.labelWidth, G.labelWidth > 0.0f ? 7.0f : 0.0f);

    if (G.es3) glBindVertexArray(G.vao);
    else {
        glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    /* Claude's head-anchored noise flame draws after the meter and before feedback. */
    if (fireActive) {
        if (draw_claude_flame(surfaceH, anchorX, anchorY, scale, fireElapsed))
            g_perfect_fire_draw_calls.fetch_add(1, std::memory_order_relaxed);
        else
            g_perfect_fire_draw_failures.fetch_add(1, std::memory_order_relaxed);
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
    const float hornBx = anchorX + 43.0f * scale - 123.5f * scale +
        kMeterShiftDesignX * scale;
    const float meterRight = floorf(hornBx + 60.0f * scale) +
        ceilf(187.0f * scale) + 1.0f;
    const float meterTop = anchorY + 48.0f * scale - 0.5f * scale;

    // Put the feedback panel one small gap to the meter's right. Its top edge
    // shares the meter's top edge. Shrink only when needed to keep it on-screen.
    const float bannerGap = 2.0f * scale;
    const float bannerLeft = meterRight + bannerGap;
    const float availableW = static_cast<float>(viewportX + surfaceW) - bannerLeft;
    if (availableW <= 8.0f * scale) return;
    float bannerScale = scale * 0.55f;
    float bannerW = 420.0f * bannerScale;
    float bannerH = 130.0f * bannerScale;
    if (bannerW > availableW) {
        bannerScale = availableW / 420.0f;
        bannerW = 420.0f * bannerScale;
        bannerH = 130.0f * bannerScale;
    }
    float bx0 = bannerLeft;
    float by0 = meterTop - bannerH;
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
    if (G.es3) {
        glBindVertexArray(G.vao);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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

    /* OFF suppresses all plugin-owned shotmeter feedback. The original game
       release handler above still runs unchanged. */
    if (g_plugin_enabled.load(std::memory_order_acquire) == 0) return;

    /* Only the tracked player's live normal jumpshot state is eligible. This
       rejects scripted/special/other-player event paths without guessing timing. */
    if (!actor || !tracked_before ||
        reinterpret_cast<uintptr_t>(actor) != tracked_before ||
        state_before != 2 || timing < 1 || timing > 5) return;

    g_eligible_local_release_events.fetch_add(1, std::memory_order_relaxed);
    g_release_timing_counts[timing].fetch_add(1, std::memory_order_relaxed);

    uint32_t arg0Bits = 0, arg1Bits = 0;
    memcpy(&arg0Bits, &arg0, sizeof(arg0Bits));
    memcpy(&arg1Bits, &arg1, sizeof(arg1Bits));
    const uint64_t eventNs = monotonic_ns();
    const uintptr_t actorValue = reinterpret_cast<uintptr_t>(actor);
    const bool sameSignature =
        g_last_release_actor.load(std::memory_order_relaxed) == actorValue &&
        g_last_release_signature_timing.load(std::memory_order_relaxed) == timing &&
        g_last_release_signature_arg0.load(std::memory_order_relaxed) == arg0Bits &&
        g_last_release_signature_arg1.load(std::memory_order_relaxed) == arg1Bits;
    const uint64_t previousEventNs = g_last_release_signature_ns.load(std::memory_order_relaxed);
    if (sameSignature && previousEventNs != 0 && eventNs >= previousEventNs &&
        eventNs - previousEventNs <= 50000000ULL)
        g_suspected_duplicate_release_events.fetch_add(1, std::memory_order_relaxed);
    g_last_release_actor.store(actorValue, std::memory_order_relaxed);
    g_last_release_signature_timing.store(timing, std::memory_order_relaxed);
    g_last_release_signature_arg0.store(arg0Bits, std::memory_order_relaxed);
    g_last_release_signature_arg1.store(arg1Bits, std::memory_order_relaxed);
    g_last_release_signature_ns.store(eventNs, std::memory_order_relaxed);

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
    if (g_plugin_enabled.load(std::memory_order_acquire) == 0) return;
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
    GLint vp[4]={0,0,0,0}, fbo=-1;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
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
__attribute__((noinline))
static void draw_customization_ui(EGLDisplay display, EGLSurface surface);

static EGLBoolean swap_proxy(EGLDisplay display, EGLSurface surface) {
    if (g_customization_open.load(std::memory_order_acquire) != 0)
        draw_customization_ui(display, surface);
    else
        draw_overlay(display, surface);
    return g_original_swap ? g_original_swap(display, surface) : EGL_FALSE;
}

/*
 * Native touch ingress contract verified from VCBoot_AndroidMain:
 * 0=ACTION_DOWN, 1=ACTION_POINTER_DOWN, 2=ACTION_MOVE, 3=ACTION_CANCEL.
 * ACTION_UP is not sent through PushEvent on this build. Coordinates are
 * normalized against the current game surface. A UI tap is therefore emitted
 * at ACTION_DOWN, not by waiting for a non-existent ACTION_UP event.
 */
static void touch_event_proxy(void *device, int32_t event_type, int32_t pointer_id,
                              float x, float y, int32_t flags) {
    if (g_customization_open.load(std::memory_order_acquire) != 0) {
        if (event_type < 0 || event_type > 3 ||
            !std::isfinite(x) || !std::isfinite(y)) {
            g_touch_event_invalid.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_touch_event_counts[event_type].fetch_add(1, std::memory_order_relaxed);

        if (event_type == 0) {
            /* ACTION_DOWN is the UI's tap edge. The game queue is deliberately
               consumed while the plugin page is modal. */
            store_float(g_ui_touch_x_bits, x);
            store_float(g_ui_touch_y_bits, y);
            store_float(g_ui_tap_x_bits, x);
            store_float(g_ui_tap_y_bits, y);
            g_ui_tap_sequence.fetch_add(1, std::memory_order_acq_rel);
            g_ui_touch_pointer_id.store(pointer_id, std::memory_order_release);
            g_ui_touch_down.store(1, std::memory_order_release);
        } else if (event_type == 1) {
            /* Secondary pointers are tracked but do not activate UI items. */
            store_float(g_ui_touch_x_bits, x);
            store_float(g_ui_touch_y_bits, y);
        } else if (event_type == 2) {
            if (g_ui_touch_pointer_id.load(std::memory_order_acquire) == pointer_id) {
                store_float(g_ui_touch_x_bits, x);
                store_float(g_ui_touch_y_bits, y);
            }
        } else if (event_type == 3) {
            g_ui_touch_down.store(0, std::memory_order_release);
            g_ui_touch_pointer_id.store(-1, std::memory_order_release);
        }
        return;
    }

    const PushTouchEventFn original = g_original_push_touch_event;
    if (original) original(device, event_type, pointer_id, x, y, flags);
}

static bool take_customization_tap(float *x, float *y) {
    if (!x || !y) return false;
    const uint32_t sequence = g_ui_tap_sequence.load(std::memory_order_acquire);
    const uint32_t consumed = g_ui_tap_consumed.load(std::memory_order_relaxed);
    if (sequence == consumed) return false;
    *x = load_float(g_ui_tap_x_bits);
    *y = load_float(g_ui_tap_y_bits);
    g_ui_tap_consumed.store(sequence, std::memory_order_release);
    return true;
}


struct CustomUiVertex {
    float x, y;
    float r, g, b, a;
};

static struct {
    EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0, vbo = 0, vao = 0;
    GLint position = -1, color = -1;
    bool es3 = false;
    bool ready = false;
} UI;

static uint8_t custom_ui_glyph_row(char ch, int row) {
    if (row < 0 || row >= 7) return 0;
    switch (ch) {
        case 'A': { const uint8_t v[7]={14,17,17,31,17,17,17}; return v[row]; }
        case 'B': { const uint8_t v[7]={30,17,17,30,17,17,30}; return v[row]; }
        case 'C': { const uint8_t v[7]={14,17,16,16,16,17,14}; return v[row]; }
        case 'D': { const uint8_t v[7]={30,17,17,17,17,17,30}; return v[row]; }
        case 'E': { const uint8_t v[7]={31,16,16,30,16,16,31}; return v[row]; }
        case 'F': { const uint8_t v[7]={31,16,16,30,16,16,16}; return v[row]; }
        case 'G': { const uint8_t v[7]={14,17,16,23,17,17,15}; return v[row]; }
        case 'H': { const uint8_t v[7]={17,17,17,31,17,17,17}; return v[row]; }
        case 'I': { const uint8_t v[7]={14,4,4,4,4,4,14}; return v[row]; }
        case 'J': { const uint8_t v[7]={7,2,2,2,18,18,12}; return v[row]; }
        case 'K': { const uint8_t v[7]={17,18,20,24,20,18,17}; return v[row]; }
        case 'L': { const uint8_t v[7]={16,16,16,16,16,16,31}; return v[row]; }
        case 'M': { const uint8_t v[7]={17,27,21,21,17,17,17}; return v[row]; }
        case 'N': { const uint8_t v[7]={17,25,21,19,17,17,17}; return v[row]; }
        case 'O': { const uint8_t v[7]={14,17,17,17,17,17,14}; return v[row]; }
        case 'P': { const uint8_t v[7]={30,17,17,30,16,16,16}; return v[row]; }
        case 'Q': { const uint8_t v[7]={14,17,17,17,21,18,13}; return v[row]; }
        case 'R': { const uint8_t v[7]={30,17,17,30,20,18,17}; return v[row]; }
        case 'S': { const uint8_t v[7]={15,16,16,14,1,1,30}; return v[row]; }
        case 'T': { const uint8_t v[7]={31,4,4,4,4,4,4}; return v[row]; }
        case 'U': { const uint8_t v[7]={17,17,17,17,17,17,14}; return v[row]; }
        case 'V': { const uint8_t v[7]={17,17,17,17,17,10,4}; return v[row]; }
        case 'W': { const uint8_t v[7]={17,17,17,21,21,21,10}; return v[row]; }
        case 'X': { const uint8_t v[7]={17,17,10,4,10,17,17}; return v[row]; }
        case 'Y': { const uint8_t v[7]={17,17,10,4,4,4,4}; return v[row]; }
        case 'Z': { const uint8_t v[7]={31,1,2,4,8,16,31}; return v[row]; }
        case '0': { const uint8_t v[7]={14,17,19,21,25,17,14}; return v[row]; }
        case '1': { const uint8_t v[7]={4,12,4,4,4,4,14}; return v[row]; }
        case '2': { const uint8_t v[7]={14,17,1,2,4,8,31}; return v[row]; }
        case '3': { const uint8_t v[7]={30,1,1,14,1,1,30}; return v[row]; }
        case '4': { const uint8_t v[7]={2,6,10,18,31,2,2}; return v[row]; }
        case '5': { const uint8_t v[7]={31,16,16,30,1,1,30}; return v[row]; }
        case '6': { const uint8_t v[7]={14,16,16,30,17,17,14}; return v[row]; }
        case '7': { const uint8_t v[7]={31,1,2,4,8,8,8}; return v[row]; }
        case '8': { const uint8_t v[7]={14,17,17,14,17,17,14}; return v[row]; }
        case '9': { const uint8_t v[7]={14,17,17,15,1,1,14}; return v[row]; }
        case '-': { const uint8_t v[7]={0,0,0,31,0,0,0}; return v[row]; }
        case ':': { const uint8_t v[7]={0,4,4,0,4,4,0}; return v[row]; }
        case '.': { const uint8_t v[7]={0,0,0,0,0,12,12}; return v[row]; }
        case '/': { const uint8_t v[7]={1,2,2,4,8,8,16}; return v[row]; }
        case '+': { const uint8_t v[7]={0,4,4,31,4,4,0}; return v[row]; }
        default: return 0;
    }
}

static void custom_ui_add_rect(std::vector<CustomUiVertex> &vertices,
                               float x0, float y0, float x1, float y1,
                               float width, float height,
                               float r, float g, float b, float a) {
    const float left = 2.0f * x0 / width - 1.0f;
    const float right = 2.0f * x1 / width - 1.0f;
    const float top = 1.0f - 2.0f * y0 / height;
    const float bottom = 1.0f - 2.0f * y1 / height;
    const CustomUiVertex v0{left, top, r, g, b, a};
    const CustomUiVertex v1{right, top, r, g, b, a};
    const CustomUiVertex v2{right, bottom, r, g, b, a};
    const CustomUiVertex v3{left, bottom, r, g, b, a};
    vertices.push_back(v0); vertices.push_back(v1); vertices.push_back(v2);
    vertices.push_back(v0); vertices.push_back(v2); vertices.push_back(v3);
}

static void custom_ui_add_text(std::vector<CustomUiVertex> &vertices,
                               const char *text, float x, float y, float scale,
                               float width, float height,
                               float r, float g, float b, float a) {
    if (!text || scale <= 0.0f) return;
    float cursor = x;
    for (const char *p = text; *p; ++p) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        for (int row = 0; row < 7; ++row) {
            const uint8_t bits = custom_ui_glyph_row(ch, row);
            for (int col = 0; col < 5; ++col) {
                if ((bits & (1u << (4 - col))) != 0) {
                    const float px = cursor + static_cast<float>(col) * scale;
                    const float py = y + static_cast<float>(row) * scale;
                    custom_ui_add_rect(vertices, px, py, px + scale * 0.82f,
                                       py + scale * 0.82f, width, height, r, g, b, a);
                }
            }
        }
        cursor += scale * 6.0f;
    }
}

static bool custom_ui_build_program(void) {
    if (UI.ready && UI.context == eglGetCurrentContext()) return true;
    UI = {};
    UI.context = eglGetCurrentContext();
    if (UI.context == EGL_NO_CONTEXT) return false;
    const GLubyte *version = glGetString(GL_VERSION);
    UI.es3 = version && strstr(reinterpret_cast<const char *>(version), "OpenGL ES 3") != nullptr;
    const char *vs2 =
        "attribute vec2 aPos; attribute vec4 aColor; varying vec4 vColor;"
        "void main(){gl_Position=vec4(aPos,0.0,1.0);vColor=aColor;}";
    const char *fs2 =
        "precision mediump float; varying vec4 vColor;"
        "void main(){gl_FragColor=vColor;}";
    const char *vs3 =
        "#version 300 es\nprecision highp float; in vec2 aPos; in vec4 aColor; out vec4 vColor;"
        "void main(){gl_Position=vec4(aPos,0.0,1.0);vColor=aColor;}";
    const char *fs3 =
        "#version 300 es\nprecision mediump float; in vec4 vColor; out vec4 fragColor;"
        "void main(){fragColor=vColor;}";
    const char *vsSource = UI.es3 ? vs3 : vs2;
    const char *fsSource = UI.es3 ? fs3 : fs2;
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    glShaderSource(vs, 1, &vsSource, nullptr);
    glShaderSource(fs, 1, &fsSource, nullptr);
    glCompileShader(vs);
    glCompileShader(fs);
    GLint vsOk = GL_FALSE, fsOk = GL_FALSE;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &vsOk);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &fsOk);
    if (vsOk != GL_TRUE || fsOk != GL_TRUE) {
        glDeleteShader(vs); glDeleteShader(fs);
        return false;
    }
    UI.program = glCreateProgram();
    if (!UI.program) {
        glDeleteShader(vs); glDeleteShader(fs);
        return false;
    }
    glAttachShader(UI.program, vs);
    glAttachShader(UI.program, fs);
    glBindAttribLocation(UI.program, 0, "aPos");
    glBindAttribLocation(UI.program, 1, "aColor");
    glLinkProgram(UI.program);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(UI.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(UI.program); UI.program = 0;
        return false;
    }
    UI.position = glGetAttribLocation(UI.program, "aPos");
    UI.color = glGetAttribLocation(UI.program, "aColor");
    glGenBuffers(1, &UI.vbo);
    if (UI.es3) glGenVertexArrays(1, &UI.vao);
    if (UI.position < 0 || UI.color < 0 || !UI.vbo || (UI.es3 && !UI.vao)) {
        if (UI.vao) glDeleteVertexArrays(1, &UI.vao);
        if (UI.vbo) glDeleteBuffers(1, &UI.vbo);
        glDeleteProgram(UI.program);
        UI = {};
        return false;
    }
    UI.ready = true;
    return true;
}

static void custom_ui_draw_batch(const std::vector<CustomUiVertex> &vertices,
                                 int width, int height) {
    if (vertices.empty() || !custom_ui_build_program()) return;
    GLint oldProgram = 0, oldBuffer = 0, oldViewport[4] = {0,0,0,0};
    GLint oldSrcRgb = GL_SRC_ALPHA, oldDstRgb = GL_ONE_MINUS_SRC_ALPHA;
    GLint oldSrcA = GL_SRC_ALPHA, oldDstA = GL_ONE_MINUS_SRC_ALPHA;
    GLint oldEqRgb = GL_FUNC_ADD, oldEqA = GL_FUNC_ADD, oldVao = 0;
    const GLboolean wasBlend = glIsEnabled(GL_BLEND);
    const GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean wasScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean wasCull = glIsEnabled(GL_CULL_FACE);
    const GLboolean wasStencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &oldSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &oldDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &oldDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &oldEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &oldEqA);
    GLint a0Enabled=0,a0Size=2,a0Type=GL_FLOAT,a0Normalized=0,a0Stride=0,a0Buffer=0;
    GLint a1Enabled=0,a1Size=4,a1Type=GL_FLOAT,a1Normalized=0,a1Stride=0,a1Buffer=0;
    void *a0Pointer=nullptr,*a1Pointer=nullptr;
    if (UI.es3) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
        glBindVertexArray(UI.vao);
    } else {
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a0Enabled);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a0Size);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a0Type);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a0Normalized);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a0Stride);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a0Buffer);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a0Pointer);
        glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a1Enabled);
        glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a1Size);
        glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a1Type);
        glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a1Normalized);
        glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a1Stride);
        glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a1Buffer);
        glGetVertexAttribPointerv(1, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a1Pointer);
    }

    glViewport(0, 0, width, height);
    if (wasDepth) glDisable(GL_DEPTH_TEST);
    if (wasScissor) glDisable(GL_SCISSOR_TEST);
    if (wasCull) glDisable(GL_CULL_FACE);
    if (wasStencil) glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(UI.program);
    glBindBuffer(GL_ARRAY_BUFFER, UI.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(CustomUiVertex)),
                 vertices.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(CustomUiVertex),
                          reinterpret_cast<const void *>(offsetof(CustomUiVertex, x)));
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(CustomUiVertex),
                          reinterpret_cast<const void *>(offsetof(CustomUiVertex, r)));
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

    if (UI.es3) {
        glBindVertexArray(static_cast<GLuint>(oldVao));
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(a0Buffer));
        glVertexAttribPointer(0, a0Size, static_cast<GLenum>(a0Type), a0Normalized,
                              a0Stride, a0Pointer);
        if (!a0Enabled) glDisableVertexAttribArray(0);
        else glEnableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(a1Buffer));
        glVertexAttribPointer(1, a1Size, static_cast<GLenum>(a1Type), a1Normalized,
                              a1Stride, a1Pointer);
        if (!a1Enabled) glDisableVertexAttribArray(1);
        else glEnableVertexAttribArray(1);
    }
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldBuffer));
    glUseProgram(static_cast<GLuint>(oldProgram));
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glBlendEquationSeparate(oldEqRgb, oldEqA);
    glBlendFuncSeparate(oldSrcRgb, oldDstRgb, oldSrcA, oldDstA);
    if (wasBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (wasDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (wasScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (wasCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (wasStencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
}

static void draw_customization_ui(EGLDisplay display, EGLSurface surface) {
    if (g_customization_open.load(std::memory_order_acquire) == 0) return;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return;
    GLint framebuffer = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer != 0) return;
    EGLint width = 0, height = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &height) != EGL_TRUE ||
        width <= 0 || height <= 0) return;

    float tapX = 0.0f, tapY = 0.0f;
    if (take_customization_tap(&tapX, &tapY)) {
        if (tapY >= 0.19f && tapY <= 0.34f && tapX >= 0.07f && tapX <= 0.93f) {
            const uint32_t tab = tapX < 0.36f ? 0u : (tapX < 0.64f ? 1u : 2u);
            g_customization_tab.store(tab, std::memory_order_release);
        } else if (tapY >= 0.77f && tapY <= 0.91f && tapX >= 0.72f) {
            (void)shotmeter_plugin_close_customization();
        } else if (tapY >= 0.38f && tapY <= 0.75f) {
            const uint32_t tab = g_customization_tab.load(std::memory_order_acquire);
            const uint32_t category = tab == 0 ? SHOTMETER_PLUGIN_CATEGORY_DESIGN :
                (tab == 1 ? SHOTMETER_PLUGIN_CATEGORY_AUDIO :
                            SHOTMETER_PLUGIN_CATEGORY_GREEN_ANIMATION);
            (void)shotmeter_plugin_set_selection(category, 0u);
        }
    }
    if (g_customization_open.load(std::memory_order_acquire) == 0) return;

    std::vector<CustomUiVertex> v;
    v.reserve(14000);
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const float panelL = 0.055f*w, panelT = 0.075f*h;
    const float panelR = 0.945f*w, panelB = 0.925f*h;
    const uint32_t tab = g_customization_tab.load(std::memory_order_acquire);

    custom_ui_add_rect(v,0,0,w,h,w,h,0.015f,0.020f,0.030f,0.72f);
    custom_ui_add_rect(v,panelL,panelT,panelR,panelB,w,h,0.055f,0.070f,0.095f,0.98f);
    custom_ui_add_rect(v,panelL,panelT,panelR,panelT+0.012f*h,w,h,0.17f,0.78f,0.32f,1.0f);
    custom_ui_add_rect(v,panelL+0.02f*w,panelT+0.09f*h,panelR-0.02f*w,panelT+0.0915f*h,w,h,
                       0.25f,0.32f,0.40f,0.75f);

    custom_ui_add_text(v,"POT SHOTMETER",panelL+0.035f*w,panelT+0.035f*h,
                       std::max(2.0f,h/170.0f),w,h,0.93f,0.97f,1.0f,1.0f);

    const float tabTop=panelT+0.12f*h, tabBottom=panelT+0.245f*h;
    const float tabLeft=panelL+0.02f*w, tabRight=panelR-0.02f*w;
    const float tabWidth=(tabRight-tabLeft)/3.0f;
    const float activeL=tabLeft+static_cast<float>(tab)*tabWidth;
    custom_ui_add_rect(v,tabLeft,tabTop,tabRight,tabBottom,w,h,0.035f,0.045f,0.065f,1.0f);
    custom_ui_add_rect(v,activeL,tabTop,activeL+tabWidth,tabBottom,w,h,
                       0.10f,0.24f,0.17f,1.0f);
    custom_ui_add_rect(v,activeL,tabBottom-0.006f*h,activeL+tabWidth,tabBottom,w,h,
                       0.22f,0.95f,0.40f,1.0f);
    const float tabTextScale=std::max(1.4f,h/470.0f);
    custom_ui_add_text(v,"DESIGN",tabLeft+0.045f*w,tabTop+0.045f*h,tabTextScale,w,h,
                       tab==0?0.45f:0.72f,tab==0?1.0f:0.78f,tab==0?0.60f:0.88f,1.0f);
    custom_ui_add_text(v,"AUDIO",tabLeft+tabWidth+0.075f*w,tabTop+0.045f*h,tabTextScale,w,h,
                       tab==1?0.45f:0.72f,tab==1?1.0f:0.78f,tab==1?0.60f:0.88f,1.0f);
    custom_ui_add_text(v,"GREEN ANIMATION",tabLeft+2.0f*tabWidth+0.025f*w,
                       tabTop+0.055f*h,std::max(1.0f,h/650.0f),w,h,
                       tab==2?0.45f:0.72f,tab==2?1.0f:0.78f,tab==2?0.60f:0.88f,1.0f);

    const float contentTop=panelT+0.29f*h, contentBottom=panelB-0.12f*h;
    const float left=panelL+0.035f*w, mid=panelL+0.48f*w, right=panelR-0.035f*w;
    custom_ui_add_rect(v,left,contentTop,right,contentBottom,w,h,0.035f,0.047f,0.066f,1.0f);
    custom_ui_add_rect(v,left+0.012f*w,contentTop+0.018f*h,mid-0.015f*w,contentBottom-0.018f*h,
                       w,h,0.075f,0.095f,0.12f,1.0f);
    custom_ui_add_rect(v,mid+0.01f*w,contentTop+0.018f*h,right-0.012f*w,contentBottom-0.018f*h,
                       w,h,0.045f,0.060f,0.080f,1.0f);
    custom_ui_draw_batch(v,width,height);
    v.clear();

    const char *section = tab==0 ? "METER DESIGNS" : (tab==1 ? "AUDIO EFFECTS" : "GREEN ANIMATIONS");
    const char *item = tab==0 ? "HORN METER" : (tab==1 ? "GREEN GIANT" : "CLAUDE FLAME");
    const char *detail = tab==0 ? "CUSTOM METER PREVIEW" :
                         (tab==1 ? "PERFECT RELEASE SOUND" : "HEAD ANCHORED FLAME");
    const float labelScale=std::max(1.5f,h/560.0f);
    custom_ui_add_text(v,section,left+0.03f*w,contentTop+0.05f*h,labelScale,w,h,
                       0.56f,0.90f,0.68f,1.0f);
    custom_ui_add_rect(v,left+0.02f*w,contentTop+0.12f*h,mid-0.03f*w,
                       contentTop+0.122f*h,w,h,0.22f,0.30f,0.37f,1.0f);
    custom_ui_add_rect(v,left+0.02f*w,contentTop+0.16f*h,mid-0.03f*w,
                       contentTop+0.31f*h,w,h,0.09f,0.14f,0.17f,1.0f);
    custom_ui_add_rect(v,left+0.02f*w,contentTop+0.16f*h,left+0.035f*w,
                       contentTop+0.31f*h,w,h,0.20f,0.90f,0.38f,1.0f);
    custom_ui_add_text(v,item,left+0.055f*w,contentTop+0.19f*h,labelScale,w,h,
                       0.93f,0.97f,1.0f,1.0f);
    custom_ui_add_text(v,"SELECTED",left+0.055f*w,contentTop+0.255f*h,
                       std::max(1.2f,h/680.0f),w,h,0.45f,0.90f,0.60f,1.0f);
    custom_ui_add_text(v,detail,left+0.03f*w,contentTop+0.36f*h,
                       std::max(1.1f,h/690.0f),w,h,0.64f,0.72f,0.80f,1.0f);

    const float previewX=mid+0.04f*w, previewY=contentTop+0.12f*h;
    const float previewW=right-mid-0.085f*w, previewH=contentBottom-contentTop-0.18f*h;
    if (tab == 0) {
        if (G.ctx != eglGetCurrentContext()) {
            G = {};
            G.ctx = eglGetCurrentContext();
        }
        if (!G.ok) {
            const int initCode=init_horn_gl();
            G.ok=(initCode==0);
            g_shader_init_code.store(initCode,std::memory_order_relaxed);
        }
        if (G.ok) {
            const int ax=static_cast<int>(previewX+previewW*0.50f);
            const int ay=static_cast<int>(previewY+previewH*0.78f);
            draw_horn(width,height,ax,ay,0.68f,false,-1,false,0.0f);
        }
    } else if (tab == 2) {
        if (G.ctx != eglGetCurrentContext()) {
            G = {};
            G.ctx = eglGetCurrentContext();
        }
        if (!G.ok) {
            const int initCode=init_horn_gl();
            G.ok=(initCode==0);
            g_shader_init_code.store(initCode,std::memory_order_relaxed);
        }
        if (G.ok && !G.flameOk) (void)init_claude_flame_gl();
        if (G.flameOk) {
            const int ax=static_cast<int>(previewX+previewW*0.50f);
            const int ay=static_cast<int>(previewY+previewH*0.72f);
            (void)draw_claude_flame(height,ax,ay,0.9f,0.35f);
        }
    } else {
        const float barW=previewW/9.0f;
        for(int i=0;i<7;++i) {
            const float fraction=static_cast<float>((i*17)%9+1)/10.0f;
            const float bh=previewH*(0.20f+0.65f*fraction);
            custom_ui_add_rect(v,previewX+barW*(static_cast<float>(i)+1.0f),
                previewY+previewH-bh,previewX+barW*(static_cast<float>(i)+1.0f)+barW*0.45f,
                previewY+previewH,w,h,0.12f,0.85f,0.38f,0.9f);
        }
    }

    custom_ui_add_text(v,"PREVIEW",mid+0.035f*w,contentTop+0.05f*h,labelScale,w,h,
                       0.76f,0.84f,0.93f,1.0f);
    const float footerY=panelB-0.095f*h;
    custom_ui_add_rect(v,panelL+0.02f*w,footerY,panelR-0.02f*w,panelB-0.025f*h,
                       w,h,0.035f,0.045f,0.062f,1.0f);
    custom_ui_add_rect(v,panelR-0.18f*w,footerY+0.01f*h,panelR-0.035f*w,
                       panelB-0.04f*h,w,h,0.10f,0.28f,0.18f,1.0f);
    custom_ui_add_text(v,"BACK",panelR-0.135f*w,footerY+0.035f*h,
                       std::max(1.5f,h/570.0f),w,h,0.80f,1.0f,0.86f,1.0f);
    custom_ui_add_text(v,"TAP A TAB OR ITEM",panelL+0.04f*w,footerY+0.035f*h,
                       std::max(1.2f,h/680.0f),w,h,0.48f,0.58f,0.70f,1.0f);

    custom_ui_draw_batch(v,width,height);
}

static int32_t save_plugin_settings_internal(void) {
    char path[256];
    const int path_len = snprintf(path, sizeof(path), "%s/%s",
                                  FILES_DIR, "shotmeter.plugin-settings-v1");
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path))
        return SHOTMETER_PLUGIN_ERROR_IO;

    char contents[160];
    const int length = snprintf(
        contents, sizeof(contents),
        "version=1\nenabled=%u\ndesign=%u\naudio=%u\ngreen_animation=%u\n",
        g_plugin_enabled.load(std::memory_order_acquire),
        g_selected_design.load(std::memory_order_acquire),
        g_selected_audio.load(std::memory_order_acquire),
        g_selected_green_animation.load(std::memory_order_acquire));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(contents))
        return SHOTMETER_PLUGIN_ERROR_IO;

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return SHOTMETER_PLUGIN_ERROR_IO;
    size_t written_total = 0;
    while (written_total < static_cast<size_t>(length)) {
        const ssize_t written = write(fd, contents + written_total,
                                      static_cast<size_t>(length) - written_total);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            close(fd);
            return SHOTMETER_PLUGIN_ERROR_IO;
        }
        written_total += static_cast<size_t>(written);
    }
    const int sync_result = fsync(fd);
    const int close_result = close(fd);
    return (sync_result == 0 && close_result == 0)
        ? SHOTMETER_PLUGIN_OK : SHOTMETER_PLUGIN_ERROR_IO;
}

static int32_t save_plugin_settings(void) {
    pthread_mutex_lock(&g_plugin_settings_mutex);
    const int32_t result = save_plugin_settings_internal();
    pthread_mutex_unlock(&g_plugin_settings_mutex);
    return result;
}

static void load_plugin_settings(void) {
    /* A menu API write made before the bootstrap finished takes precedence
       over any older on-disk setting that has not yet been read. */
    if (g_plugin_settings_modified.load(std::memory_order_acquire) != 0) return;
    char path[256];
    const int path_len = snprintf(path, sizeof(path), "%s/%s",
                                  FILES_DIR, "shotmeter.plugin-settings-v1");
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) return;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char contents[256] = {};
    const ssize_t count = read(fd, contents, sizeof(contents) - 1);
    close(fd);
    if (count <= 0) return;

    unsigned version = 0, enabled = 0, design = 0, audio = 0, green_animation = 0;
    const int fields = sscanf(contents,
        "version=%u\nenabled=%u\ndesign=%u\naudio=%u\ngreen_animation=%u",
        &version, &enabled, &design, &audio, &green_animation);
    if (fields != 5 || version != SHOTMETER_PLUGIN_API_VERSION ||
        enabled > 1 || design > 255 || audio > 255 || green_animation > 255)
        return;

    g_plugin_enabled.store(enabled, std::memory_order_release);
    g_selected_design.store(design, std::memory_order_release);
    g_selected_audio.store(audio, std::memory_order_release);
    g_selected_green_animation.store(green_animation, std::memory_order_release);
}

extern "C" __attribute__((visibility("default")))
uint32_t shotmeter_plugin_get_api_version(void) {
    return SHOTMETER_PLUGIN_API_VERSION;
}

extern "C" __attribute__((visibility("default")))
uint32_t shotmeter_plugin_get_capabilities(void) {
    return SHOTMETER_PLUGIN_CAP_LIVE_ENABLE_DISABLE |
           SHOTMETER_PLUGIN_CAP_PERSISTENT_SETTINGS |
           SHOTMETER_PLUGIN_CAP_CUSTOMIZATION_STATE;
}

extern "C" __attribute__((visibility("default")))
int32_t shotmeter_plugin_set_enabled(uint32_t enabled) {
    if (enabled > 1) return SHOTMETER_PLUGIN_ERROR_ARGUMENT;
    g_plugin_enabled.store(enabled, std::memory_order_release);
    g_plugin_settings_modified.store(1, std::memory_order_release);
    if (enabled == 0) {
        g_result_until_ns.store(0, std::memory_order_release);
        g_perfect_until_ns.store(0, std::memory_order_release);
        g_sound_requests.store(0, std::memory_order_release);
        pthread_mutex_lock(&g_sound_mutex);
        if (g_sl_play) (void)(*g_sl_play)->SetPlayState(g_sl_play, SL_PLAYSTATE_STOPPED);
        if (g_sl_queue) (void)(*g_sl_queue)->Clear(g_sl_queue);
        pthread_mutex_unlock(&g_sound_mutex);
    }
    return save_plugin_settings();
}

extern "C" __attribute__((visibility("default")))
uint32_t shotmeter_plugin_get_enabled(void) {
    return g_plugin_enabled.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default")))
int32_t shotmeter_plugin_open_customization(void) {
    g_ui_touch_pointer_id.store(-1, std::memory_order_release);
    g_ui_touch_down.store(0, std::memory_order_release);
    g_ui_tap_consumed.store(g_ui_tap_sequence.load(std::memory_order_acquire), std::memory_order_release);
    g_customization_open.store(1, std::memory_order_release);
    return SHOTMETER_PLUGIN_OK;
}

extern "C" __attribute__((visibility("default")))
int32_t shotmeter_plugin_close_customization(void) {
    g_customization_open.store(0, std::memory_order_release);
    g_ui_touch_down.store(0, std::memory_order_release);
    g_ui_touch_pointer_id.store(-1, std::memory_order_release);
    return SHOTMETER_PLUGIN_OK;
}

extern "C" __attribute__((visibility("default")))
uint32_t shotmeter_plugin_is_customization_open(void) {
    return g_customization_open.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default")))
int32_t shotmeter_plugin_set_selection(uint32_t category, uint32_t selection_id) {
    if (selection_id > 255) return SHOTMETER_PLUGIN_ERROR_ARGUMENT;
    switch (category) {
        case SHOTMETER_PLUGIN_CATEGORY_DESIGN:
            g_selected_design.store(selection_id, std::memory_order_release);
            g_plugin_settings_modified.store(1, std::memory_order_release);
            break;
        case SHOTMETER_PLUGIN_CATEGORY_AUDIO:
            g_selected_audio.store(selection_id, std::memory_order_release);
            g_plugin_settings_modified.store(1, std::memory_order_release);
            break;
        case SHOTMETER_PLUGIN_CATEGORY_GREEN_ANIMATION:
            g_selected_green_animation.store(selection_id, std::memory_order_release);
            g_plugin_settings_modified.store(1, std::memory_order_release);
            break;
        default:
            return SHOTMETER_PLUGIN_ERROR_ARGUMENT;
    }
    return save_plugin_settings();
}

extern "C" __attribute__((visibility("default")))
int32_t shotmeter_plugin_get_selection(uint32_t category) {
    switch (category) {
        case SHOTMETER_PLUGIN_CATEGORY_DESIGN:
            return static_cast<int32_t>(g_selected_design.load(std::memory_order_acquire));
        case SHOTMETER_PLUGIN_CATEGORY_AUDIO:
            return static_cast<int32_t>(g_selected_audio.load(std::memory_order_acquire));
        case SHOTMETER_PLUGIN_CATEGORY_GREEN_ANIMATION:
            return static_cast<int32_t>(g_selected_green_animation.load(std::memory_order_acquire));
        default:
            return SHOTMETER_PLUGIN_ERROR_ARGUMENT;
    }
}

extern "C" __attribute__((visibility("default")))
int32_t shotmeter_plugin_flush_settings(void) {
    return save_plugin_settings();
}

extern "C" __attribute__((visibility("default")))
uint32_t shotmeter_plugin_get_init_state(void) {
    return g_init_state.load(std::memory_order_acquire);
}


/* Native POT MOD page integration.
 * Clone the game's own one-row MENU page shape and Difficulty-style toggle
 * object, then redirect the native SlideOnNav_OptionsMenu descriptor to a
 * nine-row copy. The original eight rows and the adjacent Playoffs table stay
 * untouched. All custom allocations live for the plugin lifetime.
 */
static constexpr uintptr_t kSlideOptionsDescriptorOffset = 0x2AB3288ULL;
static constexpr uintptr_t kDebugOneRowMenuTemplateOffset = 0x295CE18ULL;
static constexpr uintptr_t kGameplayToggleRowTemplateOffset = 0x273A9E0ULL;
static constexpr uintptr_t kToggleInterfaceConstructorOffset = 0x0F3F3ACULL;
static constexpr uintptr_t kToggleValueStringFunctionOffset = 0x0E01C08ULL;
static constexpr uintptr_t kVcTextDrawPltOffset = 0x00BC9040ULL;
static constexpr size_t kMenuDescriptorBytes = 0xE0U;
static constexpr size_t kOptionRowBytes = 0x38U;
static constexpr size_t kSlideOptionRowStride = 0xEA8U;
static constexpr uint32_t kPotModPageNameCrc = 0x6058305FU;  // CRC32("POT_MOD")
static constexpr uint32_t kShotmeterRowNameCrc = 0x5947D8D8U; // CRC32("shotmeter")

using NativeToggleCtorFn = void (*)(void *, bool (*)(void), bool (*)(void),
                                    void (*)(void *), uint32_t);
using NativeValueStringFn = uint32_t (*)(int32_t);
using NativeTextDrawFn = void (*)(void *, uint32_t, void *);

static bool pot_mod_toggle_increment(void) {
    const bool was_enabled = g_plugin_enabled.load(std::memory_order_acquire) != 0;
    const int32_t set_result = shotmeter_plugin_set_enabled(1);
    if (!was_enabled) (void)shotmeter_plugin_open_customization();
    LOGI("POT MOD native toggle increment: previous=%u set_result=%d",
         was_enabled ? 1U : 0U, set_result);
    return true;
}

static bool pot_mod_toggle_decrement(void) {
    const bool was_enabled = g_plugin_enabled.load(std::memory_order_acquire) != 0;
    const int32_t set_result = shotmeter_plugin_set_enabled(0);
    if (was_enabled) (void)shotmeter_plugin_close_customization();
    LOGI("POT MOD native toggle decrement: previous=%u set_result=%d",
         was_enabled ? 1U : 0U, set_result);
    return true;
}

static void pot_mod_toggle_draw_value(void *game_text) {
    if (!game_text || g_game_base == 0) return;
    const auto get_value_string = reinterpret_cast<NativeValueStringFn>(
        g_game_base + kToggleValueStringFunctionOffset);
    const auto draw_text = reinterpret_cast<NativeTextDrawFn>(
        g_game_base + kVcTextDrawPltOffset);
    const int32_t enabled = g_plugin_enabled.load(std::memory_order_acquire) ? 1 : 0;
    const uint32_t string_crc = get_value_string(enabled);
    uint64_t empty_args = 0;
    draw_text(game_text, string_crc, &empty_args);
}

static bool install_native_pot_mod_menu(void) {
    int32_t expected_state = 0;
    if (!g_pot_menu_install_state.compare_exchange_strong(
            expected_state, 1, std::memory_order_acq_rel)) {
        return expected_state == 2;
    }
    if (!g_game || g_game_base == 0) {
        g_pot_menu_install_state.store(3, std::memory_order_release);
        LOGE("POT MOD menu install failed: game handle/base unavailable");
        return false;
    }

    auto *options_descriptor = reinterpret_cast<uint8_t *>(
        g_game_base + kSlideOptionsDescriptorOffset);
    auto *page_template = reinterpret_cast<const uint8_t *>(
        g_game_base + kDebugOneRowMenuTemplateOffset);
    auto *row_template = reinterpret_cast<const uint8_t *>(
        g_game_base + kGameplayToggleRowTemplateOffset);

    uintptr_t old_rows_address = 0;
    uint32_t old_count = 0;
    memcpy(&old_rows_address, options_descriptor + 0x58, sizeof(old_rows_address));
    memcpy(&old_count, options_descriptor + 0xB0, sizeof(old_count));
    if (!old_rows_address || old_count != 8) {
        g_pot_menu_install_state.store(3, std::memory_order_release);
        LOGE("POT MOD menu install refused: unexpected OPTIONS descriptor base=%p count=%u",
             reinterpret_cast<void *>(old_rows_address), old_count);
        return false;
    }

    auto *new_options_rows = static_cast<uint8_t *>(
        calloc(9U, kSlideOptionRowStride));
    auto *new_page_descriptor = static_cast<uint8_t *>(
        calloc(1U, kMenuDescriptorBytes));
    auto *new_page_rows = static_cast<uint8_t *>(
        calloc(1U, kOptionRowBytes));
    auto *new_toggle_interface = static_cast<uint8_t *>(calloc(1U, 0x38U));
    if (!new_options_rows || !new_page_descriptor ||
        !new_page_rows || !new_toggle_interface) {
        free(new_options_rows);
        free(new_page_descriptor);
        free(new_page_rows);
        free(new_toggle_interface);
        g_pot_menu_install_state.store(3, std::memory_order_release);
        LOGE("POT MOD menu install failed: allocation failure");
        return false;
    }

    memcpy(new_options_rows, reinterpret_cast<const void *>(old_rows_address),
           8U * kSlideOptionRowStride);
    memcpy(new_page_descriptor, page_template, kMenuDescriptorBytes);
    memcpy(new_page_rows, row_template, kOptionRowBytes);

    const auto toggle_ctor = reinterpret_cast<NativeToggleCtorFn>(
        g_game_base + kToggleInterfaceConstructorOffset);
    toggle_ctor(new_toggle_interface,
                &pot_mod_toggle_increment,
                &pot_mod_toggle_decrement,
                &pot_mod_toggle_draw_value,
                0U);

    /* The normal gameplay row is a type-1 native toggle row. Reuse only its
       row layout; its original Difficulty interface object is not reused. */
    const uint32_t row_type = 1U;
    const uint32_t zero_crc = 0U;
    memcpy(new_page_rows + 0x00, &row_type, sizeof(row_type));
    memcpy(new_page_rows + 0x04, &kShotmeterRowNameCrc, sizeof(kShotmeterRowNameCrc));
    memcpy(new_page_rows + 0x08, &zero_crc, sizeof(zero_crc));
    memcpy(new_page_rows + 0x10, &new_toggle_interface, sizeof(new_toggle_interface));

    /* A standard MENU descriptor stores its option-row pointer at +0x60 and
       row count at +0xB4. The cloned template already carries the native page
       event handler and layout/style pointers. */
    memcpy(new_page_descriptor + 0x60, &new_page_rows, sizeof(new_page_rows));
    const uint32_t one_row = 1U;
    memcpy(new_page_descriptor + 0xB4, &one_row, sizeof(one_row));
    memcpy(new_page_descriptor + 0x0C, &kPotModPageNameCrc,
           sizeof(kPotModPageNameCrc));

    /* Existing type-5 rows use a MENU* at +0x28. Clone the Presentation page
       entry's native shape, then point it at the newly created POT MOD page. */
    uint8_t *pot_row = new_options_rows + 8U * kSlideOptionRowStride;
    memcpy(pot_row, new_options_rows + 7U * kSlideOptionRowStride,
           kSlideOptionRowStride);
    memcpy(pot_row + 0x08, &kPotModPageNameCrc, sizeof(kPotModPageNameCrc));
    memcpy(pot_row + 0x10, &kPotModPageNameCrc, sizeof(kPotModPageNameCrc));
    memcpy(pot_row + 0x28, &new_page_descriptor, sizeof(new_page_descriptor));

    /* Publish the new row array first; raise count last so the game never sees
       a count of nine with an incomplete ninth record. */
    memcpy(options_descriptor + 0x58, &new_options_rows, sizeof(new_options_rows));
    const uint32_t nine_rows = 9U;
    __atomic_store_n(reinterpret_cast<uint32_t *>(options_descriptor + 0xB0),
                     nine_rows, __ATOMIC_RELEASE);

    g_pot_options_rows = new_options_rows;
    g_pot_page_descriptor = new_page_descriptor;
    g_pot_page_rows = new_page_rows;
    g_pot_toggle_interface = new_toggle_interface;
    g_pot_menu_install_state.store(2, std::memory_order_release);
    LOGI("POT MOD native menu installed: OPTIONS rows 8->9, page=%p row=%p toggle=%p",
         new_page_descriptor, new_page_rows, new_toggle_interface);
    return true;
}

static jint initialize_agent(void) {
    g_attach_calls.fetch_add(1, std::memory_order_relaxed);
    uint32_t settings_expected = 0;
    if (g_plugin_settings_loaded.compare_exchange_strong(
            settings_expected, 1, std::memory_order_acq_rel)) {
        load_plugin_settings();
    }
    uint32_t expected = 0;
    if (!g_init_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        g_duplicate_attach_calls.fetch_add(1, std::memory_order_relaxed);
        return JNI_OK;
    }
    uint32_t probability_cases = 0;
    const bool probability_map_ok = probability_mapping_self_test(&probability_cases);
    g_probability_map_self_test_pass.store(probability_map_ok ? 1U : 0U, std::memory_order_release);
    g_probability_map_self_test_cases.store(probability_cases, std::memory_order_release);
    uint32_t grade_validation_cases = 0;
    const bool grade_validation_ok =
        grade_validation_self_test(&grade_validation_cases);
    g_probability_grade_validation_self_test_pass.store(
        grade_validation_ok ? 1U : 0U, std::memory_order_release);
    g_probability_grade_validation_self_test_cases.store(
        grade_validation_cases, std::memory_order_release);
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
        
        return JNI_OK;
    }

    /* Bake the CPU-only flame noise before the first gameplay frame. Doing this
       lazily inside init_claude_flame_gl stalls the render thread on the first shot. */
    claude_bake_noise();

    g_game = open_game();
    if (g_game) {
        g_component = reinterpret_cast<volatile uint8_t *>(resolve(g_game, kComponentSymbol));
        g_do_draw_address = resolve(g_game, kDoDrawSymbol);
        g_head_address = resolve(g_game, kHeadSymbol);
        g_saved_getter_address = resolve(g_game, kSavedGetterSymbol);
        g_project_address = resolve(g_game, kProjectSymbol);
        g_swap_address = resolve(g_game, kSwapSymbol);
        g_push_touch_event_address = resolve(g_game, "_ZN14VCTOUCH_DEVICE9PushEventE17VCTOUCHEVENT_TYPEiffi");
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
        const bool pot_menu_ok = install_native_pot_mod_menu();
        LOGI("POT MOD native menu initialization state=%d",
             pot_menu_ok ? 2 : g_pot_menu_install_state.load(std::memory_order_acquire));
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
        if (g_push_touch_event_address) {
            void *original_touch = nullptr;
            g_touch_event_hook_code = DobbyHook(g_push_touch_event_address,
                (void *)&touch_event_proxy, &original_touch);
            g_original_push_touch_event = reinterpret_cast<PushTouchEventFn>(original_touch);
        } else g_touch_event_hook_code = -1006;
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
            if (g_probability_hook_code == 0 && g_original_balanced_dice &&
                probability_map_ok && grade_validation_ok) {
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
    for (int timing = 1; timing <= 5; ++timing) {
        g_probability_timing_last_grade[timing].store(-1, std::memory_order_relaxed);
        g_probability_timing_last_status[timing].store(kGradeNotRequired, std::memory_order_relaxed);
    }
    const bool sound_ready = initialize_sound();
    if (sound_ready) {
        pthread_t sound_thread;
        if (pthread_create(&sound_thread, nullptr, sound_worker, nullptr) == 0)
            pthread_detach(sound_thread);
        else g_sound_init_code.store(-2002, std::memory_order_release);
    }
    LOGI("v69 embedded Green Giant audio init=%s code=%d", sound_ready ? "ready" : "unavailable", g_sound_init_code.load());
    g_init_state.store(2, std::memory_order_release);
    
LOGI("shotmeter.dobby-v69-auto-bootstrap attached; probability_hook_code=%d enabled=%u",
         g_probability_hook_code, g_probability_enabled.load());
    return JNI_OK;
}
extern "C" JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM *vm, char *, void *) {
    if (!vm) return JNI_ERR;
    return initialize_agent();
}

extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *) {
    LOGI("v69 auto-bootstrap agent unloading");
}


/*
 * JNI bootstrap for APK loading:
 * - JNI_OnLoad is called by ART when Java loads libshotmeter.so.
 * - A detached worker waits for the game's original ELF to be present.
 * - Once present, it calls the same initialization path used by manual
 *   Agent_OnAttach. The v67 rendering, hook, and embedded-audio code is kept.
 *
 * The APK must still call System.loadLibrary("shotmeter") (or otherwise load
 * this library). Merely storing this ELF under lib/arm64-v8a is not enough.
 */
static std::atomic<uint32_t> g_auto_bootstrap_state{0};

static bool game_library_is_loaded_for_bootstrap(void) {
    void *handle = dlopen(kSoname, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen(kFilename, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return false;
    dlclose(handle);
    return true;
}

static void *auto_bootstrap_worker(void *) {
    LOGI("v69 bootstrap worker waiting for game ELF");
    const struct timespec retry_delay{0, 250000000L};
    constexpr uint32_t kMaxChecks = 480; // 120 seconds maximum
    for (uint32_t attempt = 1; attempt <= kMaxChecks; ++attempt) {
        if (game_library_is_loaded_for_bootstrap()) {
            LOGI("v69 bootstrap detected game ELF after %u checks", attempt);
            const jint attach_result =
                initialize_agent();
            const uint32_t init_state =
                g_init_state.load(std::memory_order_acquire);
            if (attach_result == JNI_OK && init_state == 2) {
                g_auto_bootstrap_state.store(2, std::memory_order_release);
                LOGI("v69 bootstrap initialization completed");
            } else {
                g_auto_bootstrap_state.store(3, std::memory_order_release);
                LOGE("v69 bootstrap initialization did not complete: attach=%d state=%u",
                     attach_result, init_state);
            }
            return nullptr;
        }
        nanosleep(&retry_delay, nullptr);
    }
    g_auto_bootstrap_state.store(3, std::memory_order_release);
    LOGE("v69 bootstrap timed out waiting for the game ELF");
    return nullptr;
}

static bool start_auto_bootstrap(const char *origin) {
    uint32_t expected = 0;
    if (!g_auto_bootstrap_state.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel)) {
        LOGI("v69 bootstrap duplicate start ignored from %s; state=%u",
             origin ? origin : "unknown", expected);
        return expected == 1 || expected == 2;
    }

    pthread_t worker{};
    const int create_result =
        pthread_create(&worker, nullptr, auto_bootstrap_worker, nullptr);
    if (create_result != 0) {
        g_auto_bootstrap_state.store(3, std::memory_order_release);
        LOGE("v69 bootstrap worker creation failed from %s: %d",
             origin ? origin : "unknown", create_result);
        return false;
    }
    pthread_detach(worker);
    LOGI("v69 bootstrap worker started from %s", origin ? origin : "unknown");
    return true;
}

/*
 * This library is linked into the game's ELF through DT_NEEDED. Android's
 * linker runs this constructor when it loads the dependency, but does not
 * promise to call JNI_OnLoad for a dependency. Query ART's existing VM here,
 * then use the same deferred attach worker as the JNI_OnLoad fallback.
 */
/*
 * No JavaVM is needed by the native hook initializer: initialize_agent()
 * never dereferences one. The constructor can therefore start the worker
 * directly, avoiding a private ART symbol lookup that may be unavailable
 * from the app's linker namespace.
 */
__attribute__((constructor))
static void shotmeter_dependency_constructor(void) {
    (void)start_auto_bootstrap("ELF constructor");
}

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *) {
    if (!vm) return JNI_ERR;
    (void)start_auto_bootstrap("JNI_OnLoad");
    return JNI_VERSION_1_6;
}

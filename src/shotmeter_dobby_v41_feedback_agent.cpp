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

#include <atomic>

#include "dobby.h"

#define LOG_TAG "ShotmeterDobbyV41"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define SHOTMETER_FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAGNOSTIC_NAME "shotmeter.dobby-v41-feedback-diagnostic"

static const char kGameLibrarySoname[] = "libnba_clean_opt.so";
static const char kGameLibraryFilename[] = "libnba2k20_clean_opt.so";

static const char kReleaseEventSymbol[] =
    "_Z20EVT_UserShotReleasedP12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGff";
static const char kTimingClassifierSymbol[] =
    "_Z24HUR_GetShotReleaseTimingPK12AI_NBA_ACTORf";
static const char kThresholdValuesSymbol[] =
    "_Z35HUR_GetReleaseTimingThresholdValuesPK12AI_NBA_ACTORPf";
static const char kTimingBoundsSymbol[] =
    "_Z41HUR_GetReleaseTimingWindowBoundsForTimingPK12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGRfS3_";
static const char kFeedbackShouldShowSymbol[] = "_ZN21ShotFeedbackComponent10ShouldShowEP12AI_NBA_ACTOR";
static const char kFeedbackDrawSymbol[] = "_ZN21ShotFeedbackComponent4DrawEv";
static const char kFeedbackDoDrawSymbol[] = "_ZN21ShotFeedbackComponent6DoDrawEi";
static const char kResetRequestName[] = "shotmeter.dobby-v41-reset";

using release_event_fn = void (*)(void *actor, int32_t timing, float arg0, float arg1);
using timing_classifier_fn = int32_t (*)(const void *actor, float timing_error);
using threshold_values_fn = void (*)(const void *actor, float *values);
using timing_bounds_fn = void (*)(const void *actor, int32_t timing,
                                  float *first_bound, float *second_bound);
using feedback_should_show_fn = bool (*)(void *component, void *actor);
using feedback_draw_fn = void (*)(void *component);
using feedback_do_draw_fn = void (*)(void *component, int32_t draw_mode);

static release_event_fn g_original_release_event = nullptr;
static timing_classifier_fn g_original_timing_classifier = nullptr;
static threshold_values_fn g_original_threshold_values = nullptr;
static timing_bounds_fn g_original_timing_bounds = nullptr;
static feedback_should_show_fn g_original_feedback_should_show = nullptr;
static feedback_draw_fn g_original_feedback_draw = nullptr;
static feedback_do_draw_fn g_original_feedback_do_draw = nullptr;

static void *g_game_library_handle = nullptr;
static const char *g_game_library_name = "not-found";
static void *g_release_event_address = nullptr;
static void *g_timing_classifier_address = nullptr;
static void *g_threshold_values_address = nullptr;
static void *g_timing_bounds_address = nullptr;
static void *g_feedback_should_show_address = nullptr;
static void *g_feedback_draw_address = nullptr;
static void *g_feedback_do_draw_address = nullptr;

static int g_self_test_code = -999;
static int g_self_test_baseline = -999;
static int g_self_test_original = -999;
static int g_self_test_patched = -999;
static int g_release_event_hook_code = -999;
static int g_timing_classifier_hook_code = -999;
static int g_threshold_values_hook_code = -999;
static int g_timing_bounds_hook_code = -999;
static int g_feedback_should_show_hook_code = -999;
static int g_feedback_draw_hook_code = -999;
static int g_feedback_do_draw_hook_code = -999;

static std::atomic<uint32_t> g_release_event_samples{0};
static std::atomic<uint32_t> g_classifier_samples{0};
static std::atomic<uint32_t> g_threshold_samples{0};
static std::atomic<uint32_t> g_bounds_samples{0};
static std::atomic<uint32_t> g_feedback_should_show_samples{0};
static std::atomic<uint32_t> g_feedback_draw_samples{0};
static std::atomic<uint32_t> g_feedback_do_draw_samples{0};
static std::atomic<uint32_t> g_diagnostic_reset_count{0};
static std::atomic<int32_t> g_last_feedback_should_show{-1};

static std::atomic<int32_t> g_last_event_timing{INT32_MIN};
static std::atomic<uint32_t> g_last_event_arg0_bits{0};
static std::atomic<uint32_t> g_last_event_arg1_bits{0};

static std::atomic<int32_t> g_last_classifier_timing{INT32_MIN};
static std::atomic<uint32_t> g_last_classifier_error_bits{0};

static std::atomic<uint32_t> g_last_threshold_bits[6]{};
static std::atomic<int32_t> g_last_bounds_timing{INT32_MIN};
static std::atomic<uint32_t> g_last_bounds_first_bits{0};
static std::atomic<uint32_t> g_last_bounds_second_bits{0};

static volatile int g_test_bias = 100;
__attribute__((noinline, optnone))
static int shotmeter_dobby_test_target(int value) {
    return value + g_test_bias;
}
using shotmeter_test_fn = int (*)(int);
static shotmeter_test_fn g_original_test_target = nullptr;

__attribute__((noinline, optnone))
static int shotmeter_dobby_test_proxy(int value) {
    if (g_original_test_target == nullptr) return -9999;
    return g_original_test_target(value) + 7;
}

__attribute__((noinline, optnone))
static int shotmeter_dobby_invoke_target(int value) {
    shotmeter_test_fn volatile call_target = &shotmeter_dobby_test_target;
    return call_target(value);
}

static void store_float_bits(std::atomic<uint32_t> &target, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    target.store(bits, std::memory_order_relaxed);
}

static float load_float_bits(const std::atomic<uint32_t> &source) {
    const uint32_t bits = source.load(std::memory_order_relaxed);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void *open_already_loaded_game_library(void) {
    const int flags = RTLD_NOW | RTLD_NOLOAD;
    dlerror();
    void *handle = dlopen(kGameLibrarySoname, flags);
    const char *error = dlerror();
    if (handle != nullptr) {
        g_game_library_name = kGameLibrarySoname;
        LOGI("Found already-loaded game ELF by SONAME %s: %p",
             kGameLibrarySoname, handle);
        return handle;
    }
    LOGI("RTLD_NOLOAD for %s did not resolve: %s",
         kGameLibrarySoname, error != nullptr ? error : "no linker detail");

    dlerror();
    handle = dlopen(kGameLibraryFilename, flags);
    error = dlerror();
    if (handle != nullptr) {
        g_game_library_name = kGameLibraryFilename;
        LOGI("Found already-loaded game ELF by filename %s: %p",
             kGameLibraryFilename, handle);
        return handle;
    }
    LOGE("Game ELF is not available through RTLD_NOLOAD (%s; %s)",
         kGameLibraryFilename, error != nullptr ? error : "no linker detail");
    return nullptr;
}

static void *resolve_game_symbol(void *handle, const char *name) {
    if (handle == nullptr) return nullptr;
    dlerror();
    void *address = dlsym(handle, name);
    const char *error = dlerror();
    if (error != nullptr) {
        LOGE("dlsym failed for %s: %s", name, error);
        return nullptr;
    }
    LOGI("Resolved game symbol %s at %p", name, address);
    return address;
}

/* Observation-only proxy: forward the original event exactly once. */
__attribute__((noinline))
static void release_event_proxy(void *actor, int32_t timing,
                                float arg0, float arg1) {
    const release_event_fn original = g_original_release_event;
    if (original != nullptr) original(actor, timing, arg0, arg1);

    const uint32_t sample =
        g_release_event_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    g_last_event_timing.store(timing, std::memory_order_relaxed);
    store_float_bits(g_last_event_arg0_bits, arg0);
    store_float_bits(g_last_event_arg1_bits, arg1);

    LOGI("EVT_UserShotReleased count=%u actor=%p timing=%d arg0=%.6f arg1=%.6f",
         sample, actor, timing, arg0, arg1);
}

/* Capture the classifier's real result for its actual normalized error input. */
__attribute__((noinline))
static int32_t timing_classifier_proxy(const void *actor, float timing_error) {
    const timing_classifier_fn original = g_original_timing_classifier;
    if (original == nullptr) return INT32_MIN;

    const int32_t result = original(actor, timing_error);
    const uint32_t sample =
        g_classifier_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    g_last_classifier_timing.store(result, std::memory_order_relaxed);
    store_float_bits(g_last_classifier_error_bits, timing_error);

    if (sample <= 64 || sample % 64 == 0) {
        LOGI("HUR_GetShotReleaseTiming count=%u actor=%p error=%.7f class=%d",
             sample, actor, timing_error, result);
    }
    return result;
}

/*
 * The original helper writes six floats: two sentinels and four adjusted
 * timing thresholds. Read them only after the original helper has filled
 * the caller's output buffer.
 */
__attribute__((noinline))
static void threshold_values_proxy(const void *actor, float *values) {
    const threshold_values_fn original = g_original_threshold_values;
    if (original != nullptr) original(actor, values);
    if (values == nullptr) return;

    const uint32_t sample =
        g_threshold_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    for (int i = 0; i < 6; ++i) {
        store_float_bits(g_last_threshold_bits[i], values[i]);
    }

    if (sample <= 32 || sample % 64 == 0) {
        LOGI("HUR_GetReleaseTimingThresholdValues count=%u actor=%p "
             "v0=%.7g v1=%.7g v2=%.7g v3=%.7g v4=%.7g v5=%.7g",
             sample, actor, values[0], values[1], values[2],
             values[3], values[4], values[5]);
    }
}

/* Observe the stock feedback eligibility predicate, preserving its return value. */
__attribute__((noinline))
static bool feedback_should_show_proxy(void *component, void *actor) {
    const feedback_should_show_fn original = g_original_feedback_should_show;
    if (original == nullptr) return false;
    const bool result = original(component, actor);
    const uint32_t sample = g_feedback_should_show_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    g_last_feedback_should_show.store(result ? 1 : 0, std::memory_order_relaxed);
    if (sample <= 32 || sample % 128 == 0)
        LOGI("ShotFeedbackComponent::ShouldShow count=%u component=%p actor=%p result=%d",
             sample, component, actor, result ? 1 : 0);
    return result;
}
__attribute__((noinline))
static void feedback_draw_proxy(void *component) {
    const feedback_draw_fn original = g_original_feedback_draw;
    if (original != nullptr) original(component);
    const uint32_t sample = g_feedback_draw_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sample <= 32 || sample % 128 == 0)
        LOGI("ShotFeedbackComponent::Draw count=%u component=%p", sample, component);
}
__attribute__((noinline))
static void feedback_do_draw_proxy(void *component, int32_t draw_mode) {
    const feedback_do_draw_fn original = g_original_feedback_do_draw;
    if (original != nullptr) original(component, draw_mode);
    const uint32_t sample = g_feedback_do_draw_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sample <= 32 || sample % 128 == 0)
        LOGI("ShotFeedbackComponent::DoDraw count=%u component=%p draw_mode=%d",
             sample, component, draw_mode);
}

/* Independent class-specific bounds helper; output argument order is retained. */
__attribute__((noinline))
static void timing_bounds_proxy(const void *actor, int32_t timing,
                                float *first_bound, float *second_bound) {
    const timing_bounds_fn original = g_original_timing_bounds;
    if (original != nullptr) {
        original(actor, timing, first_bound, second_bound);
    }
    if (first_bound == nullptr || second_bound == nullptr) return;

    const uint32_t sample =
        g_bounds_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    g_last_bounds_timing.store(timing, std::memory_order_relaxed);
    store_float_bits(g_last_bounds_first_bits, *first_bound);
    store_float_bits(g_last_bounds_second_bits, *second_bound);

    if (sample <= 32 || sample % 64 == 0) {
        LOGI("HUR_GetReleaseTimingWindowBoundsForTiming count=%u actor=%p "
             "class=%d first=%.7g second=%.7g",
             sample, actor, timing, *first_bound, *second_bound);
    }
}

static bool consume_reset_request(void) {
    char path[256];
    const int path_len = snprintf(path, sizeof(path), "%s/%s", SHOTMETER_FILES_DIR, kResetRequestName);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    close(fd);
    if (unlink(path) != 0) { LOGE("could not consume reset marker: errno=%d", errno); return false; }
    g_release_event_samples.store(0, std::memory_order_relaxed);
    g_classifier_samples.store(0, std::memory_order_relaxed);
    g_threshold_samples.store(0, std::memory_order_relaxed);
    g_bounds_samples.store(0, std::memory_order_relaxed);
    g_feedback_should_show_samples.store(0, std::memory_order_relaxed);
    g_feedback_draw_samples.store(0, std::memory_order_relaxed);
    g_feedback_do_draw_samples.store(0, std::memory_order_relaxed);
    g_last_event_timing.store(INT32_MIN, std::memory_order_relaxed);
    g_last_event_arg0_bits.store(0, std::memory_order_relaxed);
    g_last_event_arg1_bits.store(0, std::memory_order_relaxed);
    g_last_classifier_timing.store(INT32_MIN, std::memory_order_relaxed);
    g_last_classifier_error_bits.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 6; ++i) g_last_threshold_bits[i].store(0, std::memory_order_relaxed);
    g_last_bounds_timing.store(INT32_MIN, std::memory_order_relaxed);
    g_last_bounds_first_bits.store(0, std::memory_order_relaxed);
    g_last_bounds_second_bits.store(0, std::memory_order_relaxed);
    g_last_feedback_should_show.store(-1, std::memory_order_relaxed);
    g_diagnostic_reset_count.fetch_add(1, std::memory_order_relaxed);
    LOGI("diagnostic reset consumed; hook registrations remain installed");
    return true;
}

static void write_diagnostic(void) {
    char path[256];
    const int path_len = snprintf(path, sizeof(path), "%s/%s",
                                  SHOTMETER_FILES_DIR, DIAGNOSTIC_NAME);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        LOGE("diagnostic path formatting failed");
        return;
    }

    const uint32_t events = g_release_event_samples.load(std::memory_order_relaxed);
    const uint32_t classifiers = g_classifier_samples.load(std::memory_order_relaxed);
    const uint32_t thresholds = g_threshold_samples.load(std::memory_order_relaxed);
    const uint32_t bounds = g_bounds_samples.load(std::memory_order_relaxed);

    const bool all_registered =
        g_release_event_hook_code == 0 && g_original_release_event != nullptr &&
        g_timing_classifier_hook_code == 0 && g_original_timing_classifier != nullptr &&
        g_threshold_values_hook_code == 0 && g_original_threshold_values != nullptr &&
        g_timing_bounds_hook_code == 0 && g_original_timing_bounds != nullptr &&
        g_feedback_should_show_hook_code == 0 && g_original_feedback_should_show != nullptr &&
        g_feedback_draw_hook_code == 0 && g_original_feedback_draw != nullptr &&
        g_feedback_do_draw_hook_code == 0 && g_original_feedback_do_draw != nullptr;

    const uint32_t feedback_should_show = g_feedback_should_show_samples.load(std::memory_order_relaxed);
    const uint32_t feedback_draw = g_feedback_draw_samples.load(std::memory_order_relaxed);
    const uint32_t feedback_do_draw = g_feedback_do_draw_samples.load(std::memory_order_relaxed);
    const bool any_feedback_callbacks = feedback_should_show != 0 || feedback_draw != 0 || feedback_do_draw != 0;
    const bool any_callbacks =
        events != 0 || classifiers != 0 || thresholds != 0 || bounds != 0 || any_feedback_callbacks;

    const char *status = "HOOK_REGISTRATION_FAILED";
    if (all_registered && any_callbacks) {
        status = any_feedback_callbacks ? "FEEDBACK_PATH_CALLBACKS_OBSERVED" : "TIMING_CALLBACKS_OBSERVED";
    } else if (all_registered) {
        status = "HOOKS_REGISTERED_WAITING_FOR_CALLBACKS";
    } else if (g_game_library_handle == nullptr) {
        status = "GAME_LIBRARY_NOT_FOUND";
    } else {
        status = "PARTIAL_OR_FAILED_HOOK_REGISTRATION";
    }

    char contents[4096];
    (void)snprintf(
        contents, sizeof(contents),
        "agent=v41\n"
        "engine=Dobby\n"
        "diagnostic_reset_command=touch files/shotmeter.dobby-v41-reset\n"
        "diagnostic_reset_count=%u\n"
        "engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7\n"
        "self_test_status=%s\n"
        "self_test_hook_code=%d\n"
        "self_test_baseline=%d\n"
        "self_test_original=%d\n"
        "self_test_patched=%d\n"
        "self_test_expected_patched=112\n"
        "target_library=%s\n"
        "target_library_handle=%p\n"
        "release_event_address=%p\n"
        "release_event_hook_code=%d\n"
        "timing_classifier_address=%p\n"
        "timing_classifier_hook_code=%d\n"
        "threshold_values_address=%p\n"
        "threshold_values_hook_code=%d\n"
        "timing_bounds_address=%p\n"
        "timing_bounds_hook_code=%d\n"
        "feedback_should_show_address=%p\n"
        "feedback_should_show_hook_code=%d\n"
        "feedback_draw_address=%p\n"
        "feedback_draw_hook_code=%d\n"
        "feedback_do_draw_address=%p\n"
        "feedback_do_draw_hook_code=%d\n"
        "release_event_callbacks=%u\n"
        "classifier_callbacks=%u\n"
        "threshold_values_callbacks=%u\n"
        "timing_bounds_callbacks=%u\n"
        "feedback_should_show_callbacks=%u\n"
        "feedback_draw_callbacks=%u\n"
        "feedback_do_draw_callbacks=%u\n"
        "last_feedback_should_show=%d\n"
        "last_event_timing=%d\n"
        "last_event_arg0=%.7f\n"
        "last_event_arg1=%.7f\n"
        "last_classifier_timing=%d\n"
        "last_classifier_error=%.7f\n"
        "last_threshold_0=%.7g\n"
        "last_threshold_1=%.7g\n"
        "last_threshold_2=%.7g\n"
        "last_threshold_3=%.7g\n"
        "last_threshold_4=%.7g\n"
        "last_threshold_5=%.7g\n"
        "last_bounds_timing=%d\n"
        "last_bounds_first=%.7g\n"
        "last_bounds_second=%.7g\n"
        "game_object_state_modified=0\n"
        "original_library_file_modified=0\n"
        "shot_timing_modified=0\n"
        "audio_or_rendering_changed=0\n"
        "status=%s\n",
        g_diagnostic_reset_count.load(std::memory_order_relaxed),
        g_self_test_code == 0 ? "PASS" : "FAIL",
        g_self_test_code,
        g_self_test_baseline, g_self_test_original, g_self_test_patched,
        g_game_library_name, g_game_library_handle,
        g_release_event_address, g_release_event_hook_code,
        g_timing_classifier_address, g_timing_classifier_hook_code,
        g_threshold_values_address, g_threshold_values_hook_code,
        g_timing_bounds_address, g_timing_bounds_hook_code,
        g_feedback_should_show_address, g_feedback_should_show_hook_code,
        g_feedback_draw_address, g_feedback_draw_hook_code,
        g_feedback_do_draw_address, g_feedback_do_draw_hook_code,
        events, classifiers, thresholds, bounds,
        feedback_should_show, feedback_draw, feedback_do_draw,
        g_last_feedback_should_show.load(std::memory_order_relaxed),
        g_last_event_timing.load(std::memory_order_relaxed),
        load_float_bits(g_last_event_arg0_bits),
        load_float_bits(g_last_event_arg1_bits),
        g_last_classifier_timing.load(std::memory_order_relaxed),
        load_float_bits(g_last_classifier_error_bits),
        load_float_bits(g_last_threshold_bits[0]),
        load_float_bits(g_last_threshold_bits[1]),
        load_float_bits(g_last_threshold_bits[2]),
        load_float_bits(g_last_threshold_bits[3]),
        load_float_bits(g_last_threshold_bits[4]),
        load_float_bits(g_last_threshold_bits[5]),
        g_last_bounds_timing.load(std::memory_order_relaxed),
        load_float_bits(g_last_bounds_first_bits),
        load_float_bits(g_last_bounds_second_bits),
        status);

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOGE("cannot write %s: errno=%d", DIAGNOSTIC_NAME, errno);
        return;
    }
    const size_t length = strlen(contents);
    const ssize_t written = write(fd, contents, length);
    if (written < 0 || static_cast<size_t>(written) != length) {
        LOGE("incomplete diagnostic write: written=%ld expected=%lu",
             static_cast<long>(written), static_cast<unsigned long>(length));
    }
    close(fd);
}

static void *diagnostic_reporter(void *) {
    for (;;) {
        (void)consume_reset_request();
        write_diagnostic();
        sleep(1);
    }
}

extern "C" JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    (void)options;
    (void)reserved;
    if (vm == nullptr) {
        LOGE("Agent_OnAttach received a null JavaVM");
        return JNI_ERR;
    }

    /* Repeat the proven local Dobby trampoline test before game hooks. */
    const int baseline = shotmeter_dobby_invoke_target(5);
    void *test_original = nullptr;
    g_self_test_code = DobbyHook(
        reinterpret_cast<void *>(&shotmeter_dobby_test_target),
        reinterpret_cast<void *>(&shotmeter_dobby_test_proxy),
        &test_original);
    g_original_test_target = reinterpret_cast<shotmeter_test_fn>(test_original);
    const int original_result =
        g_original_test_target != nullptr ? g_original_test_target(5) : -1;
    const int patched_result = shotmeter_dobby_invoke_target(5);
    g_self_test_baseline = baseline;
    g_self_test_original = original_result;
    g_self_test_patched = patched_result;
    const bool self_test_passed =
        baseline == 105 && g_self_test_code == 0 &&
        original_result == 105 && patched_result == 112;

    LOGI("Dobby self-test baseline=%d hook=%d original=%d patched=%d status=%s",
         baseline, g_self_test_code, original_result, patched_result,
         self_test_passed ? "PASS" : "FAIL");

    if (!self_test_passed) {
        write_diagnostic();
        return JNI_OK;
    }

    /*
     * RTLD_NOLOAD is deliberate: only inspect the already-loaded game ELF.
     * The agent does not load, rename, replace, or modify the original file.
     */
    g_game_library_handle = open_already_loaded_game_library();
    if (g_game_library_handle != nullptr) {
        g_release_event_address =
            resolve_game_symbol(g_game_library_handle, kReleaseEventSymbol);
        g_timing_classifier_address =
            resolve_game_symbol(g_game_library_handle, kTimingClassifierSymbol);
        g_threshold_values_address =
            resolve_game_symbol(g_game_library_handle, kThresholdValuesSymbol);
        g_timing_bounds_address =
            resolve_game_symbol(g_game_library_handle, kTimingBoundsSymbol);
        g_feedback_should_show_address = resolve_game_symbol(g_game_library_handle, kFeedbackShouldShowSymbol);
        g_feedback_draw_address = resolve_game_symbol(g_game_library_handle, kFeedbackDrawSymbol);
        g_feedback_do_draw_address = resolve_game_symbol(g_game_library_handle, kFeedbackDoDrawSymbol);

        if (g_release_event_address != nullptr) {
            void *original = nullptr;
            g_release_event_hook_code = DobbyHook(
                g_release_event_address,
                reinterpret_cast<void *>(&release_event_proxy),
                &original);
            g_original_release_event =
                reinterpret_cast<release_event_fn>(original);
            LOGI("EVT_UserShotReleased DobbyHook code=%d original=%p",
                 g_release_event_hook_code, original);
        } else {
            g_release_event_hook_code = -1002;
        }

        if (g_timing_classifier_address != nullptr) {
            void *original = nullptr;
            g_timing_classifier_hook_code = DobbyHook(
                g_timing_classifier_address,
                reinterpret_cast<void *>(&timing_classifier_proxy),
                &original);
            g_original_timing_classifier =
                reinterpret_cast<timing_classifier_fn>(original);
            LOGI("HUR_GetShotReleaseTiming DobbyHook code=%d original=%p",
                 g_timing_classifier_hook_code, original);
        } else {
            g_timing_classifier_hook_code = -1002;
        }

        if (g_threshold_values_address != nullptr) {
            void *original = nullptr;
            g_threshold_values_hook_code = DobbyHook(
                g_threshold_values_address,
                reinterpret_cast<void *>(&threshold_values_proxy),
                &original);
            g_original_threshold_values =
                reinterpret_cast<threshold_values_fn>(original);
            LOGI("HUR_GetReleaseTimingThresholdValues DobbyHook code=%d original=%p",
                 g_threshold_values_hook_code, original);
        } else {
            g_threshold_values_hook_code = -1002;
        }

        if (g_timing_bounds_address != nullptr) {
            void *original = nullptr;
            g_timing_bounds_hook_code = DobbyHook(
                g_timing_bounds_address,
                reinterpret_cast<void *>(&timing_bounds_proxy),
                &original);
            g_original_timing_bounds =
                reinterpret_cast<timing_bounds_fn>(original);
            LOGI("HUR_GetReleaseTimingWindowBoundsForTiming DobbyHook code=%d original=%p",
                 g_timing_bounds_hook_code, original);
        } else {
            g_timing_bounds_hook_code = -1002;
        }
        if (g_feedback_should_show_address != nullptr) {
            void *original = nullptr;
            g_feedback_should_show_hook_code = DobbyHook(g_feedback_should_show_address,
                reinterpret_cast<void *>(&feedback_should_show_proxy), &original);
            g_original_feedback_should_show = reinterpret_cast<feedback_should_show_fn>(original);
            LOGI("ShotFeedbackComponent::ShouldShow hook=%d original=%p", g_feedback_should_show_hook_code, original);
        } else { g_feedback_should_show_hook_code = -1002; }

        if (g_feedback_draw_address != nullptr) {
            void *original = nullptr;
            g_feedback_draw_hook_code = DobbyHook(g_feedback_draw_address,
                reinterpret_cast<void *>(&feedback_draw_proxy), &original);
            g_original_feedback_draw = reinterpret_cast<feedback_draw_fn>(original);
            LOGI("ShotFeedbackComponent::Draw hook=%d original=%p", g_feedback_draw_hook_code, original);
        } else { g_feedback_draw_hook_code = -1002; }

        if (g_feedback_do_draw_address != nullptr) {
            void *original = nullptr;
            g_feedback_do_draw_hook_code = DobbyHook(g_feedback_do_draw_address,
                reinterpret_cast<void *>(&feedback_do_draw_proxy), &original);
            g_original_feedback_do_draw = reinterpret_cast<feedback_do_draw_fn>(original);
            LOGI("ShotFeedbackComponent::DoDraw hook=%d original=%p", g_feedback_do_draw_hook_code, original);
        } else { g_feedback_do_draw_hook_code = -1002; }
    } else {
        g_release_event_hook_code = -1001;
        g_timing_classifier_hook_code = -1001;
        g_threshold_values_hook_code = -1001;
        g_timing_bounds_hook_code = -1001;
        g_feedback_should_show_hook_code = -1001;
        g_feedback_draw_hook_code = -1001;
        g_feedback_do_draw_hook_code = -1001;
    }

    write_diagnostic();

    pthread_t reporter;
    const int thread_result =
        pthread_create(&reporter, nullptr, diagnostic_reporter, nullptr);
    if (thread_result == 0) {
        (void)pthread_detach(reporter);
    } else {
        LOGE("diagnostic reporter thread could not start: code=%d", thread_result);
    }

    LOGI("v41 feedback-path observer attach completed; all game proxies are observation-only");
    return JNI_OK;
}

extern "C" JNIEXPORT void JNICALL
Agent_OnUnload(JavaVM *vm) {
    (void)vm;
    LOGI("ART is unloading the v40 Dobby timing observer");
}

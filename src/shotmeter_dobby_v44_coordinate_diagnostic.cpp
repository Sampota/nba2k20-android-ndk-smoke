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
#include <cmath>

#include "dobby.h"

#define LOG_TAG "ShotmeterDobbyV44"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAG_NAME "shotmeter.dobby-v44-coordinate-diagnostic"

static const char kGameSoname[] = "libnba_clean_opt.so";
static const char kGameFilename[] = "libnba2k20_clean_opt.so";
static const char kDoDrawSymbol[] =
    "_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR";
static const char kHeadLocationSymbol[] =
    "_Z30PlayerIconUtil_GetHeadLocationP12AI_NBA_ACTORRDv4_f";
static const char kSavedViewSymbol[] =
    "_Z32PlayerIconManager3D_GetSavedViewv";
static const char kProjectSymbol[] =
    "_Z14VCView_ProjectPK6VCVIEWPKDv4_fPS2_";

using DoDrawFn = void (*)(void *component, void *actor);
using HeadLocationFn = void (*)(void *actor, float *world_head);
using SavedViewFn = const void *(*)();
using ProjectFn = float (*)(const void *view, const float *world_position,
                            float *screen_position);
using TestFn = int (*)(int);

static DoDrawFn g_original_do_draw = nullptr;
static HeadLocationFn g_get_head_location = nullptr;
static SavedViewFn g_get_saved_view = nullptr;
static ProjectFn g_project = nullptr;

static void *g_game_handle = nullptr;
static const char *g_game_name = "not-found";
static void *g_do_draw_address = nullptr;
static void *g_head_location_address = nullptr;
static void *g_saved_view_address = nullptr;
static void *g_project_address = nullptr;

static int g_self_code = -999;
static int g_self_baseline = -999;
static int g_self_original = -999;
static int g_self_patched = -999;
static int g_do_draw_hook_code = -999;

static std::atomic<uint32_t> g_attach_calls{0};
static std::atomic<uint32_t> g_duplicate_attach_calls{0};
/* 0=not started, 1=initializing, 2=initialized, 3=self-test aborted. */
static std::atomic<uint32_t> g_initialization_state{0};
static std::atomic<uint32_t> g_do_draw_callbacks{0};
static std::atomic<uint32_t> g_head_location_calls{0};
static std::atomic<uint32_t> g_saved_view_available{0};
static std::atomic<uint32_t> g_projection_calls{0};
static std::atomic<uint32_t> g_projection_valid{0};
static std::atomic<int32_t> g_last_project_status{0};
static std::atomic<uintptr_t> g_last_actor{0};
static std::atomic<uintptr_t> g_last_view{0};
static std::atomic<uint32_t> g_last_head_bits[4] = {0U, 0U, 0U, 0U};
static std::atomic<uint32_t> g_last_screen_bits[3] = {0U, 0U, 0U};
static std::atomic<uint32_t> g_last_project_return_bits{0};

static volatile int g_test_bias = 100;

__attribute__((noinline, optnone))
static int test_target(int value) {
    return value + g_test_bias;
}

static TestFn g_original_test = nullptr;

__attribute__((noinline, optnone))
static int test_proxy(int value) {
    return g_original_test != nullptr ? g_original_test(value) + 7 : -9999;
}

__attribute__((noinline, optnone))
static int invoke_test(int value) {
    TestFn volatile target = &test_target;
    return target(value);
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

static void *open_game_library(void) {
    void *handle = dlopen(kGameSoname, RTLD_NOW | RTLD_NOLOAD);
    if (handle != nullptr) {
        g_game_name = kGameSoname;
        return handle;
    }
    handle = dlopen(kGameFilename, RTLD_NOW | RTLD_NOLOAD);
    if (handle != nullptr) g_game_name = kGameFilename;
    return handle;
}

static void *resolve_symbol(void *handle, const char *name) {
    if (handle == nullptr) return nullptr;
    dlerror();
    void *address = dlsym(handle, name);
    const char *error = dlerror();
    if (error != nullptr) {
        LOGE("dlsym %s failed: %s", name, error);
        return nullptr;
    }
    LOGI("resolved %s at %p", name, address);
    return address;
}

/*
 * Capture the head anchor while the stock DoDraw callback has supplied a
 * live AI_NBA_ACTOR. The same saved-view and VCView_Project path is used by
 * the stock player-icon visibility test. No game-object fields are written.
 */
static void capture_player_anchor(void *actor) {
    if (actor == nullptr || g_get_head_location == nullptr ||
        g_get_saved_view == nullptr || g_project == nullptr) {
        return;
    }

    float head[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_get_head_location(actor, head);
    g_head_location_calls.fetch_add(1, std::memory_order_relaxed);
    g_last_actor.store(reinterpret_cast<uintptr_t>(actor),
                       std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) {
        store_float_bits(g_last_head_bits[i], head[i]);
    }

    const void *view = g_get_saved_view();
    g_last_view.store(reinterpret_cast<uintptr_t>(view),
                      std::memory_order_relaxed);
    if (view == nullptr) {
        g_last_project_status.store(-1, std::memory_order_relaxed);
        return;
    }
    g_saved_view_available.fetch_add(1, std::memory_order_relaxed);

    float screen[4] = {NAN, NAN, NAN, NAN};
    const float reciprocal_clip_w = g_project(view, head, screen);
    g_projection_calls.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
        store_float_bits(g_last_screen_bits[i], screen[i]);
    }
    store_float_bits(g_last_project_return_bits, reciprocal_clip_w);

    const bool valid = std::isfinite(screen[0]) &&
                       std::isfinite(screen[1]) &&
                       std::isfinite(screen[2]) &&
                       std::isfinite(reciprocal_clip_w);
    if (valid) {
        g_projection_valid.fetch_add(1, std::memory_order_relaxed);
        g_last_project_status.store(1, std::memory_order_relaxed);
    } else {
        g_last_project_status.store(-2, std::memory_order_relaxed);
    }
}

__attribute__((noinline))
static void do_draw_proxy(void *component, void *actor) {
    g_do_draw_callbacks.fetch_add(1, std::memory_order_relaxed);

    /* Capture before the stock renderer changes its transient render state. */
    capture_player_anchor(actor);

    const DoDrawFn original = g_original_do_draw;
    if (original != nullptr) original(component, actor);
}

static void write_diagnostic(void) {
    char path[256];
    const int path_length =
        snprintf(path, sizeof(path), "%s/%s", FILES_DIR, DIAG_NAME);
    if (path_length <= 0 ||
        static_cast<size_t>(path_length) >= sizeof(path)) {
        LOGE("diagnostic path formatting failed");
        return;
    }

    const uint32_t callbacks = g_do_draw_callbacks.load();
    const uint32_t head_calls = g_head_location_calls.load();
    const uint32_t views = g_saved_view_available.load();
    const uint32_t projections = g_projection_calls.load();
    const uint32_t valid = g_projection_valid.load();

    const bool self_test_passed =
        g_self_code == 0 && g_self_baseline == 105 &&
        g_self_original == 105 && g_self_patched == 112;
    const bool hooks_registered =
        g_do_draw_hook_code == 0 && g_original_do_draw != nullptr &&
        g_get_head_location != nullptr && g_get_saved_view != nullptr &&
        g_project != nullptr;

    const char *status = "HOOK_REGISTRATION_FAILED";
    if (!self_test_passed) {
        status = "DOBBY_SELF_TEST_FAILED";
    } else if (g_game_handle == nullptr) {
        status = "GAME_LIBRARY_NOT_FOUND";
    } else if (!hooks_registered) {
        status = "SYMBOL_OR_DODRAW_HOOK_MISSING";
    } else if (callbacks == 0) {
        status = "HOOKS_REGISTERED_WAITING_FOR_DODRAW";
    } else if (head_calls == 0) {
        status = "DODRAW_OBSERVED_WITHOUT_HEAD_CAPTURE";
    } else if (views == 0) {
        status = "HEAD_CAPTURED_SAVED_VIEW_NULL";
    } else if (projections != 0 && valid == 0) {
        status = "PROJECTION_RETURNED_NONFINITE_COORDINATES";
    } else if (valid != 0) {
        status = "HEAD_AND_SCREEN_COORDINATES_CAPTURED";
    } else {
        status = "HEAD_CAPTURED_WAITING_FOR_PROJECTION";
    }

    char body[3072];
    (void)snprintf(
        body, sizeof(body),
        "agent=v44\n"
        "purpose=read_only_player_head_projection_diagnostic\n"
        "engine=Dobby\n"
        "engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7\n"
        "target_library=%s\n"
        "do_draw_symbol=%s\n"
        "do_draw_address=%p\n"
        "do_draw_hook_code=%d\n"
        "head_location_symbol=%s\n"
        "head_location_address=%p\n"
        "saved_view_symbol=%s\n"
        "saved_view_address=%p\n"
        "project_symbol=%s\n"
        "project_address=%p\n"
        "self_test_status=%s\n"
        "self_test_hook_code=%d\n"
        "self_test_baseline=%d\n"
        "self_test_original=%d\n"
        "self_test_patched=%d\n"
        "attach_calls=%u\n"
        "duplicate_attach_calls=%u\n"
        "initialization_state=%u\n"
        "do_draw_callbacks=%u\n"
        "head_location_calls=%u\n"
        "saved_view_available=%u\n"
        "projection_calls=%u\n"
        "projection_valid=%u\n"
        "last_project_status=%d\n"
        "last_actor=%p\n"
        "last_view=%p\n"
        "last_head_x=%.8g\n"
        "last_head_y=%.8g\n"
        "last_head_z=%.8g\n"
        "last_head_w=%.8g\n"
        "last_screen_x=%.8g\n"
        "last_screen_y=%.8g\n"
        "last_screen_z=%.8g\n"
        "last_project_reciprocal_clip_w=%.8g\n"
        "game_object_fields_modified=0\n"
        "shot_timing_modified=0\n"
        "original_meter_modified=0\n"
        "original_library_file_modified=0\n"
        "custom_graphics_drawn=0\n"
        "status=%s\n",
        g_game_name,
        kDoDrawSymbol, g_do_draw_address, g_do_draw_hook_code,
        kHeadLocationSymbol, g_head_location_address,
        kSavedViewSymbol, g_saved_view_address,
        kProjectSymbol, g_project_address,
        self_test_passed ? "PASS" : "FAIL",
        g_self_code, g_self_baseline, g_self_original, g_self_patched,
        g_attach_calls.load(), g_duplicate_attach_calls.load(),
        g_initialization_state.load(),
        callbacks, head_calls, views, projections, valid,
        g_last_project_status.load(),
        reinterpret_cast<void *>(g_last_actor.load()),
        reinterpret_cast<void *>(g_last_view.load()),
        load_float_bits(g_last_head_bits[0]),
        load_float_bits(g_last_head_bits[1]),
        load_float_bits(g_last_head_bits[2]),
        load_float_bits(g_last_head_bits[3]),
        load_float_bits(g_last_screen_bits[0]),
        load_float_bits(g_last_screen_bits[1]),
        load_float_bits(g_last_screen_bits[2]),
        load_float_bits(g_last_project_return_bits),
        status);

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOGE("cannot write diagnostic: errno=%d", errno);
        return;
    }
    const size_t length = strlen(body);
    const ssize_t written = write(fd, body, length);
    if (written < 0 || static_cast<size_t>(written) != length) {
        LOGE("incomplete diagnostic write %ld/%lu",
             static_cast<long>(written), static_cast<unsigned long>(length));
    }
    close(fd);
}

static void *diagnostic_reporter(void *) {
    for (;;) {
        write_diagnostic();
        sleep(1);
    }
}

static void start_diagnostic_reporter(void) {
    pthread_t reporter;
    const int thread_result =
        pthread_create(&reporter, nullptr, diagnostic_reporter, nullptr);
    if (thread_result == 0) {
        (void)pthread_detach(reporter);
    } else {
        LOGE("diagnostic reporter thread creation failed: code=%d",
             thread_result);
    }
}

extern "C" JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    (void)options;
    (void)reserved;
    if (vm == nullptr) return JNI_ERR;

    g_attach_calls.fetch_add(1, std::memory_order_relaxed);
    uint32_t expected_state = 0;
    if (!g_initialization_state.compare_exchange_strong(
            expected_state, 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        g_duplicate_attach_calls.fetch_add(1, std::memory_order_relaxed);
        LOGI("duplicate Agent_OnAttach call ignored; state=%u duplicates=%u",
             g_initialization_state.load(),
             g_duplicate_attach_calls.load());
        return JNI_OK;
    }

    g_self_baseline = invoke_test(5);
    void *test_original = nullptr;
    g_self_code = DobbyHook(reinterpret_cast<void *>(&test_target),
                            reinterpret_cast<void *>(&test_proxy),
                            &test_original);
    g_original_test = reinterpret_cast<TestFn>(test_original);
    g_self_original = g_original_test != nullptr ? g_original_test(5) : -1;
    g_self_patched = invoke_test(5);

    const bool self_test_passed =
        g_self_code == 0 && g_self_baseline == 105 &&
        g_self_original == 105 && g_self_patched == 112;
    LOGI("Dobby self-test baseline=%d hook=%d original=%d patched=%d status=%s",
         g_self_baseline, g_self_code, g_self_original, g_self_patched,
         self_test_passed ? "PASS" : "FAIL");
    if (!self_test_passed) {
        g_initialization_state.store(3, std::memory_order_release);
        write_diagnostic();
        start_diagnostic_reporter();
        return JNI_OK;
    }

    g_game_handle = open_game_library();
    if (g_game_handle != nullptr) {
        g_do_draw_address = resolve_symbol(g_game_handle, kDoDrawSymbol);
        g_head_location_address =
            resolve_symbol(g_game_handle, kHeadLocationSymbol);
        g_saved_view_address =
            resolve_symbol(g_game_handle, kSavedViewSymbol);
        g_project_address = resolve_symbol(g_game_handle, kProjectSymbol);

        g_get_head_location =
            reinterpret_cast<HeadLocationFn>(g_head_location_address);
        g_get_saved_view = reinterpret_cast<SavedViewFn>(g_saved_view_address);
        g_project = reinterpret_cast<ProjectFn>(g_project_address);

        if (g_do_draw_address != nullptr && g_get_head_location != nullptr &&
            g_get_saved_view != nullptr && g_project != nullptr) {
            void *original = nullptr;
            g_do_draw_hook_code =
                DobbyHook(g_do_draw_address,
                          reinterpret_cast<void *>(&do_draw_proxy), &original);
            g_original_do_draw = reinterpret_cast<DoDrawFn>(original);
            LOGI("DoDraw coordinate diagnostic hook code=%d trampoline=%p",
                 g_do_draw_hook_code, original);
        } else {
            g_do_draw_hook_code = -1002;
        }
    } else {
        g_do_draw_hook_code = -1001;
    }

    g_initialization_state.store(2, std::memory_order_release);
    write_diagnostic();
    start_diagnostic_reporter();

    LOGI("v44 idempotent player-anchor diagnostic attached; no custom graphics or game fields modified");
    return JNI_OK;
}

extern "C" JNIEXPORT void JNICALL
Agent_OnUnload(JavaVM *vm) {
    (void)vm;
    LOGI("v44 player-anchor diagnostic unloading");
}

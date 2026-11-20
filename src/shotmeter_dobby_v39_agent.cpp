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

#define LOG_TAG "ShotmeterDobbyV39"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define SHOTMETER_FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAGNOSTIC_NAME "shotmeter.dobby-v39-diagnostic"

static const char kGameLibrarySoname[] = "libnba_clean_opt.so";
static const char kGameLibraryFilename[] = "libnba2k20_clean_opt.so";

static const char kDoDrawSymbol[] =
    "_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR";
static const char kShotReleasedSymbol[] =
    "_Z20EVT_UserShotReleasedP12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGff";
static const char kSetMaterialEnabledSymbol[] =
    "_ZNK22ShotIndicatorComponent18SetMaterialEnabledE10string_crc5_bool";

using do_draw_fn = void (*)(void *component, void *actor);
using shot_released_fn = void (*)(void *actor, int32_t timing, float arg0, float arg1);
using set_material_enabled_fn = void (*)(void *component, uint32_t material_crc, bool enabled);

/* Original trampolines are assigned immediately after each successful DobbyHook. */
static do_draw_fn g_original_do_draw = nullptr;
static shot_released_fn g_original_shot_released = nullptr;
static set_material_enabled_fn g_original_set_material_enabled = nullptr;

static void *g_game_library_handle = nullptr;
static const char *g_game_library_name = "not-found";
static void *g_do_draw_address = nullptr;
static void *g_shot_released_address = nullptr;
static void *g_set_material_enabled_address = nullptr;

static int g_self_test_code = -999;
static int g_self_test_baseline = -999;
static int g_self_test_original = -999;
static int g_self_test_patched = -999;
static int g_do_draw_hook_code = -999;
static int g_shot_released_hook_code = -999;
static int g_set_material_enabled_hook_code = -999;

static std::atomic<uint32_t> g_draw_samples{0};
static std::atomic<uint32_t> g_release_samples{0};
static std::atomic<uint32_t> g_material_samples{0};
static std::atomic<int32_t> g_last_timing{INT32_MIN};
static std::atomic<uint32_t> g_last_arg0_bits{0};
static std::atomic<uint32_t> g_last_arg1_bits{0};
static std::atomic<uint32_t> g_last_material_crc{0};
static std::atomic<uint32_t> g_last_material_enabled{0};

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

/*
 * Observation-only proxies: call the original function and record arguments
 * or counts. They do not read or write game-object fields and do not alter
 * timing, rendering, material state, audio, or gameplay state.
 */
__attribute__((noinline))
static void do_draw_proxy(void *component, void *actor) {
    const do_draw_fn original = g_original_do_draw;
    if (original != nullptr) original(component, actor);

    const uint32_t sample =
        g_draw_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sample == 1 || sample % 600 == 0) {
        LOGI("DoDraw callback count=%u component=%p actor=%p",
             sample, component, actor);
    }
}

__attribute__((noinline))
static void shot_released_proxy(void *actor, int32_t timing,
                                float arg0, float arg1) {
    const shot_released_fn original = g_original_shot_released;
    if (original != nullptr) original(actor, timing, arg0, arg1);

    const uint32_t sample =
        g_release_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    g_last_timing.store(timing, std::memory_order_relaxed);
    store_float_bits(g_last_arg0_bits, arg0);
    store_float_bits(g_last_arg1_bits, arg1);

    LOGI("EVT_UserShotReleased callback count=%u actor=%p timing_enum=%d arg0=%.6f arg1=%.6f",
         sample, actor, timing, arg0, arg1);
}

__attribute__((noinline))
static void set_material_enabled_proxy(void *component, uint32_t material_crc,
                                       bool enabled) {
    const set_material_enabled_fn original = g_original_set_material_enabled;
    if (original != nullptr) original(component, material_crc, enabled);

    const uint32_t sample =
        g_material_samples.fetch_add(1, std::memory_order_relaxed) + 1;
    g_last_material_crc.store(material_crc, std::memory_order_relaxed);
    g_last_material_enabled.store(enabled ? 1U : 0U, std::memory_order_relaxed);

    if (sample <= 32 || sample % 64 == 0) {
        LOGI("SetMaterialEnabled callback count=%u component=%p material_crc=0x%08x enabled=%u",
             sample, component, material_crc, enabled ? 1U : 0U);
    }
}

static float load_float_bits(const std::atomic<uint32_t> &source) {
    const uint32_t bits = source.load(std::memory_order_relaxed);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void write_diagnostic(void) {
    char path[256];
    const int path_len = snprintf(path, sizeof(path), "%s/%s",
                                  SHOTMETER_FILES_DIR, DIAGNOSTIC_NAME);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        LOGE("diagnostic path formatting failed");
        return;
    }

    const uint32_t draws = g_draw_samples.load(std::memory_order_relaxed);
    const uint32_t releases = g_release_samples.load(std::memory_order_relaxed);
    const uint32_t materials = g_material_samples.load(std::memory_order_relaxed);
    const bool all_registered =
        g_do_draw_hook_code == 0 && g_original_do_draw != nullptr &&
        g_shot_released_hook_code == 0 && g_original_shot_released != nullptr &&
        g_set_material_enabled_hook_code == 0 &&
        g_original_set_material_enabled != nullptr;

    const char *status = "HOOK_REGISTRATION_FAILED";
    if (all_registered && (draws != 0 || releases != 0 || materials != 0)) {
        status = "CALLBACKS_OBSERVED";
    } else if (all_registered) {
        status = "HOOKS_REGISTERED_WAITING_FOR_CALLBACKS";
    } else if (g_game_library_handle == nullptr) {
        status = "GAME_LIBRARY_NOT_FOUND";
    } else {
        status = "PARTIAL_OR_FAILED_HOOK_REGISTRATION";
    }

    char contents[2048];
    (void)snprintf(
        contents, sizeof(contents),
        "agent=v39\n"
        "engine=Dobby\n"
        "engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7\n"
        "self_test_status=%s\n"
        "self_test_hook_code=%d\n"
        "self_test_baseline=%d\n"
        "self_test_original=%d\n"
        "self_test_patched=%d\n"
        "self_test_expected_patched=112\n"
        "target_library=%s\n"
        "target_library_handle=%p\n"
        "do_draw_address=%p\n"
        "do_draw_hook_code=%d\n"
        "shot_released_address=%p\n"
        "shot_released_hook_code=%d\n"
        "set_material_enabled_address=%p\n"
        "set_material_enabled_hook_code=%d\n"
        "do_draw_callbacks=%u\n"
        "shot_released_callbacks=%u\n"
        "set_material_enabled_callbacks=%u\n"
        "last_timing_enum=%d\n"
        "last_release_arg0=%.6f\n"
        "last_release_arg1=%.6f\n"
        "last_material_crc=0x%08x\n"
        "last_material_enabled=%u\n"
        "game_object_state_modified=0\n"
        "original_library_file_modified=0\n"
        "audio_or_rendering_changed=0\n"
        "status=%s\n",
        g_self_test_code == 0 ? "PASS" : "FAIL",
        g_self_test_code,
        g_self_test_baseline, g_self_test_original, g_self_test_patched,
        g_game_library_name, g_game_library_handle,
        g_do_draw_address, g_do_draw_hook_code,
        g_shot_released_address, g_shot_released_hook_code,
        g_set_material_enabled_address, g_set_material_enabled_hook_code,
        draws, releases, materials,
        g_last_timing.load(std::memory_order_relaxed),
        load_float_bits(g_last_arg0_bits),
        load_float_bits(g_last_arg1_bits),
        g_last_material_crc.load(std::memory_order_relaxed),
        g_last_material_enabled.load(std::memory_order_relaxed),
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
    /* Refresh the private marker once per second for three minutes. */
    for (int second = 0; second < 180; ++second) {
        write_diagnostic();
        sleep(1);
    }
    write_diagnostic();
    return nullptr;
}

extern "C" JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    (void)options;
    (void)reserved;
    if (vm == nullptr) {
        LOGE("Agent_OnAttach received a null JavaVM");
        return JNI_ERR;
    }

    /* Re-run the proven v38 self-test before attempting any game symbol. */
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
     * RTLD_NOLOAD is intentional: this agent must only inspect an ELF that
     * the game has already loaded. It will not load, replace, or rename the
     * original NBA 2K20 library.
     */
    g_game_library_handle = open_already_loaded_game_library();
    if (g_game_library_handle != nullptr) {
        g_do_draw_address = resolve_game_symbol(g_game_library_handle, kDoDrawSymbol);
        g_shot_released_address =
            resolve_game_symbol(g_game_library_handle, kShotReleasedSymbol);
        g_set_material_enabled_address =
            resolve_game_symbol(g_game_library_handle, kSetMaterialEnabledSymbol);

        /* Install the less frequent event/material hooks before the draw hook. */
        if (g_shot_released_address != nullptr) {
            void *original = nullptr;
            g_shot_released_hook_code = DobbyHook(
                g_shot_released_address,
                reinterpret_cast<void *>(&shot_released_proxy),
                &original);
            g_original_shot_released =
                reinterpret_cast<shot_released_fn>(original);
            LOGI("EVT_UserShotReleased DobbyHook code=%d original=%p",
                 g_shot_released_hook_code, original);
        } else {
            g_shot_released_hook_code = -1002;
        }

        if (g_set_material_enabled_address != nullptr) {
            void *original = nullptr;
            g_set_material_enabled_hook_code = DobbyHook(
                g_set_material_enabled_address,
                reinterpret_cast<void *>(&set_material_enabled_proxy),
                &original);
            g_original_set_material_enabled =
                reinterpret_cast<set_material_enabled_fn>(original);
            LOGI("SetMaterialEnabled DobbyHook code=%d original=%p",
                 g_set_material_enabled_hook_code, original);
        } else {
            g_set_material_enabled_hook_code = -1002;
        }

        if (g_do_draw_address != nullptr) {
            void *original = nullptr;
            g_do_draw_hook_code = DobbyHook(
                g_do_draw_address,
                reinterpret_cast<void *>(&do_draw_proxy),
                &original);
            g_original_do_draw = reinterpret_cast<do_draw_fn>(original);
            LOGI("DoDraw DobbyHook code=%d original=%p",
                 g_do_draw_hook_code, original);
        } else {
            g_do_draw_hook_code = -1002;
        }
    } else {
        g_do_draw_hook_code = -1001;
        g_shot_released_hook_code = -1001;
        g_set_material_enabled_hook_code = -1001;
    }

    write_diagnostic();

    pthread_t reporter;
    const int thread_result = pthread_create(&reporter, nullptr,
                                             diagnostic_reporter, nullptr);
    if (thread_result == 0) {
        (void)pthread_detach(reporter);
    } else {
        LOGE("diagnostic reporter thread could not start: code=%d", thread_result);
    }

    LOGI("v39 attach completed; hooks are observation-only");
    return JNI_OK;
}

extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
    (void)vm;
    LOGI("ART is unloading the v39 Dobby diagnostic agent");
}

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

#define LOG_TAG "ShotmeterDobbyV45"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAG_NAME "shotmeter.dobby-v45-saved-view-diagnostic"

/*
 * These are ELF virtual addresses/structure offsets confirmed from the
 * supplied libnba_clean_opt.so disassembly:
 *   PlayerIconManager3D global = 0x3c700e0
 *   saved VCVIEW member       = manager + 0x90
 *   sizeof(VCVIEW)            = 0x240 bytes
 *
 * The runtime address is rebased with dladdr() so ASLR is respected.
 */
static constexpr uintptr_t kPlayerIconManagerOffset = 0x3c700e0ULL;
static constexpr uintptr_t kSavedViewMemberOffset = 0x90ULL;
static constexpr size_t kViewSize = 0x240U;

static const char kGameSoname[] = "libnba_clean_opt.so";
static const char kGameFilename[] = "libnba2k20_clean_opt.so";
static const char kDoDrawSymbol[] =
    "_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR";
static const char kHeadLocationSymbol[] =
    "_Z30PlayerIconUtil_GetHeadLocationP12AI_NBA_ACTORRDv4_f";
static const char kSavedViewSymbol[] =
    "_Z32PlayerIconManager3D_GetSavedViewv";
static const char kParallelViewSymbol[] = "_Z19Gui_GetParallelViewv";
static const char kRenderStateSymbol[] = "_Z21VCView_GetRenderStateP6VCVIEW";
static const char kProjectSymbol[] =
    "_Z14VCView_ProjectPK6VCVIEWPKDv4_fPS2_";

using DoDrawFn = void (*)(void *component, void *actor);
using HeadLocationFn = void (*)(void *actor, float *world_head);
using ParallelViewFn = const void *(*)();
using GetRenderStateFn = void (*)(void *view_buffer);
using ProjectFn = float (*)(const void *view, const float *world_position,
                            float *screen_position);
using TestFn = int (*)(int);

static DoDrawFn g_original_do_draw = nullptr;
static HeadLocationFn g_get_head_location = nullptr;
static ParallelViewFn g_get_parallel_view = nullptr;
static GetRenderStateFn g_get_render_state = nullptr;
static ProjectFn g_project = nullptr;

static void *g_game_handle = nullptr;
static const char *g_game_name = "not-found";
static void *g_do_draw_address = nullptr;
static void *g_head_location_address = nullptr;
static void *g_saved_view_address = nullptr;
static void *g_parallel_view_function_address = nullptr;
static void *g_render_state_address = nullptr;
static void *g_project_address = nullptr;
static uintptr_t g_game_load_base = 0;
static uintptr_t g_manager_view_address = 0;

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

static std::atomic<uint32_t> g_saved_view_reads{0};
static std::atomic<uint32_t> g_saved_projection_calls{0};
static std::atomic<uint32_t> g_saved_projection_valid{0};
static std::atomic<int32_t> g_last_saved_project_status{0};
static std::atomic<uintptr_t> g_last_saved_view{0};
static std::atomic<uint32_t> g_last_saved_screen_bits[3] = {0U, 0U, 0U};
static std::atomic<uint32_t> g_last_saved_reciprocal_bits{0};

static std::atomic<uint32_t> g_parallel_view_available{0};
static std::atomic<uint32_t> g_parallel_projection_calls{0};
static std::atomic<uint32_t> g_parallel_projection_valid{0};
static std::atomic<int32_t> g_last_parallel_project_status{0};
static std::atomic<uintptr_t> g_last_parallel_view{0};
static std::atomic<uint32_t> g_last_parallel_screen_bits[3] = {0U, 0U, 0U};
static std::atomic<uint32_t> g_last_parallel_reciprocal_bits{0};

static std::atomic<uintptr_t> g_last_actor{0};
static std::atomic<uint32_t> g_last_head_bits[4] = {0U, 0U, 0U, 0U};

static std::atomic<uint32_t> g_manager_flag_zero_samples{0};
static std::atomic<uint32_t> g_manager_flag_nonzero_samples{0};
static std::atomic<uint32_t> g_last_manager_draw_flag{0};

static std::atomic<uint32_t> g_active_projection_calls{0};
static std::atomic<uint32_t> g_active_projection_valid{0};
static std::atomic<int32_t> g_last_active_project_status{0};
static std::atomic<uint32_t> g_last_active_screen_bits[3] = {0U, 0U, 0U};
static std::atomic<uint32_t> g_last_active_reciprocal_bits{0};

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

static bool derive_manager_saved_view_address(void) {
    if (g_saved_view_address == nullptr) return false;

    Dl_info info{};
    if (dladdr(g_saved_view_address, &info) == 0 ||
        info.dli_fbase == nullptr) {
        LOGE("dladdr failed for saved-view getter");
        return false;
    }

    g_game_load_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    g_manager_view_address =
        g_game_load_base + kPlayerIconManagerOffset +
        kSavedViewMemberOffset;

    LOGI("game base=%p manager=%p saved view=%p",
         info.dli_fbase,
         reinterpret_cast<void *>(g_game_load_base +
                                  kPlayerIconManagerOffset),
         reinterpret_cast<void *>(g_manager_view_address));
    return true;
}

static bool projection_result_is_valid(const float *screen,
                                       float reciprocal_clip_w) {
    return std::isfinite(screen[0]) &&
           std::isfinite(screen[1]) &&
           std::isfinite(screen[2]) &&
           std::isfinite(reciprocal_clip_w) &&
           std::fabs(reciprocal_clip_w) > 1.0e-8f;
}

/*
 * VCView_Project may lazily refresh cached projection data according to the
 * VCVIEW flags. To keep this diagnostic read-only with respect to game-owned
 * memory, it always receives a private 16-byte-aligned 0x240-byte snapshot.
 */
static uint32_t read_manager_draw_flag(void) {
    if (g_game_load_base == 0) return 0xffffffffU;
    uint32_t value = 0;
    const uintptr_t address =
        g_game_load_base + kPlayerIconManagerOffset + 0x80ULL;
    memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
    return value;
}

/*
 * Snapshot the currently active CPU display-list view into private storage.
 * VCView_GetRenderState(destination) is a 0x240-byte copy from display-list
 * offset +0x190; it does not alter the display list or any game-owned view.
 */
static void project_active_render_state_copy(const float *head) {
    if (g_get_render_state == nullptr || g_project == nullptr) {
        g_last_active_project_status.store(-1, std::memory_order_relaxed);
        return;
    }

    alignas(16) uint8_t view_copy[kViewSize] = {};
    g_get_render_state(static_cast<void *>(view_copy));

    float screen[4] = {NAN, NAN, NAN, NAN};
    const float reciprocal_clip_w =
        g_project(static_cast<const void *>(view_copy), head, screen);

    g_active_projection_calls.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
        store_float_bits(g_last_active_screen_bits[i], screen[i]);
    }
    store_float_bits(g_last_active_reciprocal_bits, reciprocal_clip_w);

    if (projection_result_is_valid(screen, reciprocal_clip_w)) {
        g_active_projection_valid.fetch_add(1, std::memory_order_relaxed);
        g_last_active_project_status.store(1, std::memory_order_relaxed);
    } else {
        g_last_active_project_status.store(-2, std::memory_order_relaxed);
    }
}

static void project_saved_view_copy(const float *head) {
    if (g_manager_view_address == 0 || g_project == nullptr) {
        g_last_saved_project_status.store(-1, std::memory_order_relaxed);
        return;
    }

    alignas(16) uint8_t view_copy[kViewSize];
    memcpy(view_copy, reinterpret_cast<const void *>(g_manager_view_address),
           sizeof(view_copy));
    g_saved_view_reads.fetch_add(1, std::memory_order_relaxed);
    g_last_saved_view.store(g_manager_view_address, std::memory_order_relaxed);

    float screen[4] = {NAN, NAN, NAN, NAN};
    const float reciprocal_clip_w =
        g_project(static_cast<const void *>(view_copy), head, screen);

    g_saved_projection_calls.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
        store_float_bits(g_last_saved_screen_bits[i], screen[i]);
    }
    store_float_bits(g_last_saved_reciprocal_bits, reciprocal_clip_w);

    if (projection_result_is_valid(screen, reciprocal_clip_w)) {
        g_saved_projection_valid.fetch_add(1, std::memory_order_relaxed);
        g_last_saved_project_status.store(1, std::memory_order_relaxed);
    } else {
        g_last_saved_project_status.store(-2, std::memory_order_relaxed);
    }
}

static void project_parallel_view_copy(const float *head) {
    if (g_get_parallel_view == nullptr || g_project == nullptr) {
        g_last_parallel_project_status.store(-1, std::memory_order_relaxed);
        return;
    }

    const void *parallel_view = g_get_parallel_view();
    g_last_parallel_view.store(reinterpret_cast<uintptr_t>(parallel_view),
                               std::memory_order_relaxed);
    if (parallel_view == nullptr) {
        g_last_parallel_project_status.store(-1, std::memory_order_relaxed);
        return;
    }

    g_parallel_view_available.fetch_add(1, std::memory_order_relaxed);
    alignas(16) uint8_t view_copy[kViewSize];
    memcpy(view_copy, parallel_view, sizeof(view_copy));

    float screen[4] = {NAN, NAN, NAN, NAN};
    const float reciprocal_clip_w =
        g_project(static_cast<const void *>(view_copy), head, screen);

    g_parallel_projection_calls.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
        store_float_bits(g_last_parallel_screen_bits[i], screen[i]);
    }
    store_float_bits(g_last_parallel_reciprocal_bits, reciprocal_clip_w);

    if (projection_result_is_valid(screen, reciprocal_clip_w)) {
        g_parallel_projection_valid.fetch_add(1, std::memory_order_relaxed);
        g_last_parallel_project_status.store(1, std::memory_order_relaxed);
    } else {
        g_last_parallel_project_status.store(-2, std::memory_order_relaxed);
    }
}

/*
 * DoDraw is reached after PlayerIconManager3D_Draw2D snapshots the current
 * perspective display-list view into manager+0x90 and switches to the GUI
 * parallel view. Compare both projections from private copies. No game-owned
 * view bytes, actor fields, shot timing, or rendering state are written.
 */
static void capture_player_anchor(void *actor) {
    if (actor == nullptr || g_get_head_location == nullptr ||
        g_project == nullptr) {
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

    const uint32_t manager_flag = read_manager_draw_flag();
    g_last_manager_draw_flag.store(manager_flag, std::memory_order_relaxed);
    if (manager_flag == 0) {
        g_manager_flag_zero_samples.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_manager_flag_nonzero_samples.fetch_add(1, std::memory_order_relaxed);
    }

    project_active_render_state_copy(head);
    project_saved_view_copy(head);
    project_parallel_view_copy(head);
}

__attribute__((noinline))
static void do_draw_proxy(void *component, void *actor) {
    g_do_draw_callbacks.fetch_add(1, std::memory_order_relaxed);
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
    const uint32_t saved_reads = g_saved_view_reads.load();
    const uint32_t saved_calls = g_saved_projection_calls.load();
    const uint32_t saved_valid = g_saved_projection_valid.load();
    const uint32_t parallel_views = g_parallel_view_available.load();
    const uint32_t parallel_calls = g_parallel_projection_calls.load();
    const uint32_t parallel_valid = g_parallel_projection_valid.load();

    const bool self_test_passed =
        g_self_code == 0 && g_self_baseline == 105 &&
        g_self_original == 105 && g_self_patched == 112;
    const bool hooks_registered =
        g_do_draw_hook_code == 0 && g_original_do_draw != nullptr &&
        g_get_head_location != nullptr && g_saved_view_address != nullptr &&
        g_get_parallel_view != nullptr && g_get_render_state != nullptr &&
        g_project != nullptr &&
        g_manager_view_address != 0;

    const char *status = "HOOK_REGISTRATION_FAILED";
    if (!self_test_passed) {
        status = "DOBBY_SELF_TEST_FAILED";
    } else if (g_game_handle == nullptr) {
        status = "GAME_LIBRARY_NOT_FOUND";
    } else if (!hooks_registered) {
        status = "SYMBOL_OR_MANAGER_VIEW_RESOLUTION_FAILED";
    } else if (callbacks == 0) {
        status = "HOOKS_REGISTERED_WAITING_FOR_DODRAW";
    } else if (head_calls == 0) {
        status = "DODRAW_OBSERVED_WITHOUT_HEAD_CAPTURE";
    } else if (saved_reads == 0) {
        status = "HEAD_CAPTURED_MANAGER_VIEW_NOT_READ";
    } else if (saved_calls != 0 && saved_valid == 0) {
        status = "SAVED_VIEW_PROJECTION_INVALID";
    } else if (saved_valid != 0) {
        status = "SAVED_VIEW_HEAD_PROJECTION_CAPTURED";
    } else {
        status = "HEAD_CAPTURED_WAITING_FOR_PROJECTION";
    }

    char body[4608];
    (void)snprintf(
        body, sizeof(body),
        "agent=v45\n"
        "purpose=read_only_saved_perspective_vs_gui_projection_diagnostic\n"
        "engine=Dobby\n"
        "engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7\n"
        "target_library=%s\n"
        "do_draw_symbol=%s\n"
        "do_draw_address=%p\n"
        "do_draw_hook_code=%d\n"
        "head_location_symbol=%s\n"
        "head_location_address=%p\n"
        "saved_view_getter_symbol=%s\n"
        "saved_view_getter_address=%p\n"
        "parallel_view_symbol=%s\n"
        "parallel_view_function_address=%p\n"
        "render_state_symbol=%s\n"
        "render_state_address=%p\n"
        "project_symbol=%s\n"
        "project_address=%p\n"
        "game_load_base=%p\n"
        "player_icon_manager_offset=0x3c700e0\n"
        "saved_view_member_offset=0x90\n"
        "manager_saved_view_address=%p\n"
        "vcview_copy_size=576\n"
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
        "saved_view_reads=%u\n"
        "saved_projection_calls=%u\n"
        "saved_projection_valid=%u\n"
        "last_saved_project_status=%d\n"
        "last_saved_view=%p\n"
        "last_saved_screen_x=%.8g\n"
        "last_saved_screen_y=%.8g\n"
        "last_saved_screen_z=%.8g\n"
        "last_saved_reciprocal_clip_w=%.8g\n"
        "parallel_view_available=%u\n"
        "parallel_projection_calls=%u\n"
        "parallel_projection_valid=%u\n"
        "last_parallel_project_status=%d\n"
        "manager_draw_flag_at_last_capture=%u\n"
        "manager_flag_zero_samples=%u\n"
        "manager_flag_nonzero_samples=%u\n"
        "active_projection_calls=%u\n"
        "active_projection_valid=%u\n"
        "last_active_project_status=%d\n"
        "last_active_screen_x=%.8g\n"
        "last_active_screen_y=%.8g\n"
        "last_active_screen_z=%.8g\n"
        "last_active_reciprocal_clip_w=%.8g\n"
        "last_parallel_view=%p\n"
        "last_parallel_screen_x=%.8g\n"
        "last_parallel_screen_y=%.8g\n"
        "last_parallel_screen_z=%.8g\n"
        "last_parallel_reciprocal_clip_w=%.8g\n"
        "last_actor=%p\n"
        "last_head_x=%.8g\n"
        "last_head_y=%.8g\n"
        "last_head_z=%.8g\n"
        "last_head_w=%.8g\n"
        "projection_input_view_copied=1\n"
        "game_owned_view_bytes_written=0\n"
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
        kParallelViewSymbol, g_parallel_view_function_address,
        kRenderStateSymbol, g_render_state_address,
        kProjectSymbol, g_project_address,
        reinterpret_cast<void *>(g_game_load_base),
        reinterpret_cast<void *>(g_manager_view_address),
        self_test_passed ? "PASS" : "FAIL",
        g_self_code, g_self_baseline, g_self_original, g_self_patched,
        g_attach_calls.load(), g_duplicate_attach_calls.load(),
        g_initialization_state.load(),
        callbacks, head_calls, saved_reads, saved_calls, saved_valid,
        g_last_saved_project_status.load(),
        reinterpret_cast<void *>(g_last_saved_view.load()),
        load_float_bits(g_last_saved_screen_bits[0]),
        load_float_bits(g_last_saved_screen_bits[1]),
        load_float_bits(g_last_saved_screen_bits[2]),
        load_float_bits(g_last_saved_reciprocal_bits),
        parallel_views, parallel_calls, parallel_valid,
        g_last_parallel_project_status.load(),
        reinterpret_cast<void *>(g_last_parallel_view.load()),
        load_float_bits(g_last_parallel_screen_bits[0]),
        load_float_bits(g_last_parallel_screen_bits[1]),
        load_float_bits(g_last_parallel_screen_bits[2]),
        load_float_bits(g_last_parallel_reciprocal_bits),
        g_last_manager_draw_flag.load(),
        g_manager_flag_zero_samples.load(),
        g_manager_flag_nonzero_samples.load(),
        g_active_projection_calls.load(),
        g_active_projection_valid.load(),
        g_last_active_project_status.load(),
        load_float_bits(g_last_active_screen_bits[0]),
        load_float_bits(g_last_active_screen_bits[1]),
        load_float_bits(g_last_active_screen_bits[2]),
        load_float_bits(g_last_active_reciprocal_bits),
        reinterpret_cast<void *>(g_last_actor.load()),
        load_float_bits(g_last_head_bits[0]),
        load_float_bits(g_last_head_bits[1]),
        load_float_bits(g_last_head_bits[2]),
        load_float_bits(g_last_head_bits[3]),
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
        g_parallel_view_function_address =
            resolve_symbol(g_game_handle, kParallelViewSymbol);
        g_render_state_address =
            resolve_symbol(g_game_handle, kRenderStateSymbol);
        g_project_address = resolve_symbol(g_game_handle, kProjectSymbol);

        g_get_head_location =
            reinterpret_cast<HeadLocationFn>(g_head_location_address);
        g_get_parallel_view =
            reinterpret_cast<ParallelViewFn>(g_parallel_view_function_address);
        g_get_render_state =
            reinterpret_cast<GetRenderStateFn>(g_render_state_address);
        g_project = reinterpret_cast<ProjectFn>(g_project_address);

        const bool manager_view_ready = derive_manager_saved_view_address();
        if (g_do_draw_address != nullptr && g_get_head_location != nullptr &&
            g_saved_view_address != nullptr && manager_view_ready &&
            g_get_parallel_view != nullptr && g_get_render_state != nullptr &&
            g_project != nullptr) {
            void *original = nullptr;
            g_do_draw_hook_code =
                DobbyHook(g_do_draw_address,
                          reinterpret_cast<void *>(&do_draw_proxy), &original);
            g_original_do_draw = reinterpret_cast<DoDrawFn>(original);
            LOGI("DoDraw saved-view diagnostic hook code=%d trampoline=%p",
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

    LOGI("v45 live render-state, saved-view and GUI comparison attached; VCView_Project receives private copies only; no game-owned view bytes modified");
    return JNI_OK;
}

extern "C" JNIEXPORT void JNICALL
Agent_OnUnload(JavaVM *vm) {
    (void)vm;
    LOGI("v45 saved-view projection diagnostic unloading");
}

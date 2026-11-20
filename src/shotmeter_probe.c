#include <android/log.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "shadowhook.h"

#define LOG_TAG "ShotmeterProbe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * Read-only instrumentation for NBA 2K20 Mobile's original ARM64 library.
 *
 * The original APK stores the main ELF as libnba2k20_clean_opt.so, while the
 * ELF DT_SONAME is libnba_clean_opt.so. The integration plan renames the
 * untouched original file to its own SONAME before the proxy loads it.
 */
static const char kTargetLibrary[] = "libnba_clean_opt.so";
static const char kDoDrawSymbol[] =
    "_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR";
static const char kShotReleasedSymbol[] =
    "_Z20EVT_UserShotReleasedP12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGff";

typedef void (*shot_indicator_do_draw_fn)(void *component, void *actor);
typedef void (*user_shot_released_fn)(void *actor, int32_t timing, float arg0, float arg1);

static void *g_do_draw_stub = NULL;
static void *g_shot_released_stub = NULL;

static atomic_flag g_install_lock = ATOMIC_FLAG_INIT;
static _Atomic uint32_t g_draw_samples = 0;
static _Atomic uint32_t g_release_samples = 0;
static _Atomic uint32_t g_last_state = UINT32_MAX;
static _Atomic int32_t g_last_timing = INT32_MIN;

enum {
    SHOTMETER_PROBE_OK = 0,
    SHOTMETER_PROBE_DRAW_HOOK_FAILED = 1,
    SHOTMETER_PROBE_RELEASE_HOOK_FAILED = 2,
    SHOTMETER_PROBE_INIT_FAILED = 4
};

static uint32_t read_u32(const void *base, size_t offset) {
    uint32_t value = 0;
    memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    return value;
}

static int32_t read_i32(const void *base, size_t offset) {
    int32_t value = 0;
    memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    return value;
}

static float read_f32(const void *base, size_t offset) {
    float value = 0.0f;
    memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    return value;
}

static void shot_indicator_do_draw_proxy(void *component, void *actor) {
    /* Shared mode requires calling the next proxy/original through this API. */
    SHADOWHOOK_CALL_PREV(shot_indicator_do_draw_proxy, shot_indicator_do_draw_fn,
                         component, actor);

    if (component == NULL) {
        SHADOWHOOK_POP_STACK();
        return;
    }

    const uint32_t sample = atomic_fetch_add_explicit(
        &g_draw_samples, 1, memory_order_relaxed) + 1;

    /*
     * Component offsets come from the static audit:
     * +0x08 tracked player, +0x10 live timing value, +0x14 visual progress,
     * +0x18 state, +0x1C release timing enum, +0x20 release error,
     * +0x28 resource pointer. This hook only reads them.
     */
    void *tracked_player = NULL;
    void *resource = NULL;
    memcpy(&tracked_player, (const unsigned char *)component + 0x08, sizeof(tracked_player));
    memcpy(&resource, (const unsigned char *)component + 0x28, sizeof(resource));

    const float live_value = read_f32(component, 0x10);
    const float visual_progress = read_f32(component, 0x14);
    const uint32_t state = read_u32(component, 0x18);
    const int32_t timing = read_i32(component, 0x1C);
    const float release_error = read_f32(component, 0x20);

    const uint32_t previous_state = atomic_exchange_explicit(
        &g_last_state, state, memory_order_relaxed);
    const int32_t previous_timing = atomic_exchange_explicit(
        &g_last_timing, timing, memory_order_relaxed);

    /* Log first sample, state/timing transitions, then one sample per 120 draws. */
    if (sample == 1 || state != previous_state || timing != previous_timing || sample % 120 == 0) {
        LOGI("meter sample=%u actor=%p tracked=%p resource=%p state=%u timing=%d "
             "live=%.5f progress=%.5f release_error=%.5f finite=%d",
             sample, actor, tracked_player, resource, state, timing,
             live_value, visual_progress, release_error,
             (isfinite(live_value) && isfinite(visual_progress) && isfinite(release_error)) ? 1 : 0);
    }

    /* Required even when this proxy did no work, to clear ShadowHook's chain state. */
    SHADOWHOOK_POP_STACK();
}

static void user_shot_released_proxy(void *actor, int32_t timing, float arg0, float arg1) {
    SHADOWHOOK_CALL_PREV(user_shot_released_proxy, user_shot_released_fn,
                         actor, timing, arg0, arg1);

    const uint32_t sample = atomic_fetch_add_explicit(
        &g_release_samples, 1, memory_order_relaxed) + 1;

    /* Enum 3 is only a static GREEN candidate; capture it without triggering effects. */
    if (timing == 3 || sample == 1 || sample % 16 == 0) {
        LOGI("release sample=%u actor=%p timing_enum=%d arg0=%.5f arg1=%.5f",
             sample, actor, timing, arg0, arg1);
    }

    SHADOWHOOK_POP_STACK();
}

/*
 * Call only after the original game ELF has been loaded into the process.
 * The function does not load, patch, or modify the original library itself.
 */
__attribute__((visibility("default")))
int shotmeter_probe_install(void) {
    while (atomic_flag_test_and_set_explicit(&g_install_lock, memory_order_acquire)) {
        /* Installation is a one-time startup operation. */
    }

    int result = SHOTMETER_PROBE_OK;

    const int init_result = shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
    if (init_result != SHADOWHOOK_ERRNO_OK) {
        LOGE("shadowhook_init failed: code=%d message=%s",
             init_result, shadowhook_to_errmsg(init_result));
        result |= SHOTMETER_PROBE_INIT_FAILED;
        atomic_flag_clear_explicit(&g_install_lock, memory_order_release);
        return result;
    }

    if (g_do_draw_stub == NULL) {
        g_do_draw_stub = shadowhook_hook_sym_name(
            kTargetLibrary, kDoDrawSymbol,
            (void *)shot_indicator_do_draw_proxy,
            NULL);

        if (g_do_draw_stub == NULL) {
            const int error = shadowhook_get_errno();
            LOGE("DoDraw hook failed: code=%d message=%s",
                 error, shadowhook_to_errmsg(error));
            result |= SHOTMETER_PROBE_DRAW_HOOK_FAILED;
        } else {
            LOGI("DoDraw hook registered: stub=%p errno=%d",
                 g_do_draw_stub, shadowhook_get_errno());
        }
    }

    if (g_shot_released_stub == NULL) {
        g_shot_released_stub = shadowhook_hook_sym_name(
            kTargetLibrary, kShotReleasedSymbol,
            (void *)user_shot_released_proxy,
            NULL);

        if (g_shot_released_stub == NULL) {
            const int error = shadowhook_get_errno();
            LOGE("EVT_UserShotReleased hook failed: code=%d message=%s",
                 error, shadowhook_to_errmsg(error));
            result |= SHOTMETER_PROBE_RELEASE_HOOK_FAILED;
        } else {
            LOGI("EVT_UserShotReleased hook registered: stub=%p errno=%d",
                 g_shot_released_stub, shadowhook_get_errno());
        }
    }

    if (g_do_draw_stub != NULL && g_shot_released_stub != NULL) {
        LOGI("read-only capture hooks ready; no meter, release timing, audio, or rendering values are modified");
    }

    atomic_flag_clear_explicit(&g_install_lock, memory_order_release);
    return result;
}

__attribute__((visibility("default")))
uint32_t shotmeter_probe_get_draw_sample_count(void) {
    return atomic_load_explicit(&g_draw_samples, memory_order_relaxed);
}

__attribute__((visibility("default")))
uint32_t shotmeter_probe_get_release_sample_count(void) {
    return atomic_load_explicit(&g_release_samples, memory_order_relaxed);
}

__attribute__((visibility("default")))
const char *shotmeter_probe_get_version(void) {
    return "shotmeter-readonly-probe-0.1.0";
}

#include <android/log.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dobby.h"

#define LOG_TAG "ShotmeterDobby"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define SHOTMETER_FILES_DIR \
    "/data/user/0/com.t2ksports.nba2k20and/files"

/*
 * Isolated engine self-test only. This deliberately hooks a function inside
 * this agent, not NBA 2K20. It verifies that Dobby can patch an ARM64 code
 * page and execute an original-function trampoline in the target process.
 */
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

static void write_diagnostic(const char *contents) {
    char path[256];
    const int path_len = snprintf(path, sizeof(path), "%s/%s",
                                  SHOTMETER_FILES_DIR,
                                  "shotmeter.dobby-diagnostic");
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        LOGE("diagnostic path formatting failed");
        return;
    }

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOGE("cannot write diagnostic: errno=%d", errno);
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

extern "C" JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    (void)options;
    (void)reserved;

    if (vm == nullptr) {
        LOGE("Agent_OnAttach received a null JavaVM");
        return JNI_ERR;
    }

    const int baseline = shotmeter_dobby_invoke_target(5);
    void *original = nullptr;
    const int hook_result = DobbyHook(
        reinterpret_cast<void *>(&shotmeter_dobby_test_target),
        reinterpret_cast<void *>(&shotmeter_dobby_test_proxy),
        &original);

    g_original_test_target = reinterpret_cast<shotmeter_test_fn>(original);

    const int original_result = g_original_test_target != nullptr
        ? g_original_test_target(5) : -1;
    const int patched_result = shotmeter_dobby_invoke_target(5);
    const bool passed = baseline == 105 &&
                        hook_result == 0 &&
                        original_result == 105 &&
                        patched_result == 112;

    char diagnostic[512];
    (void)snprintf(
        diagnostic, sizeof(diagnostic),
        "engine=Dobby\n"
        "engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7\n"
        "baseline_before=%d\n"
        "hook_install_code=%d\n"
        "original_trampoline_after=%d\n"
        "patched_call_after=%d\n"
        "expected_patched_call=112\n"
        "status=%s\n"
        "game_hooks_attempted=0\n",
        baseline, hook_result, original_result, patched_result,
        passed ? "PASS" : "FAIL");

    write_diagnostic(diagnostic);
    if (passed) {
        LOGI("Dobby self-hook test passed: baseline=%d original=%d patched=%d",
             baseline, original_result, patched_result);
    } else {
        LOGE("Dobby self-hook test failed: baseline=%d hook=%d original=%d patched=%d",
             baseline, hook_result, original_result, patched_result);
    }

    /* Keep the diagnostic agent loaded even when the self-test fails. */
    return JNI_OK;
}

extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
    (void)vm;
    LOGI("ART is unloading the Dobby diagnostic agent");
}

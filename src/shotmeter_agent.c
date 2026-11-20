#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "shotmeter_probe.h"

#define LOG_TAG "ShotmeterAgent"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define SHOTMETER_FILES_DIR \
    "/data/user/0/com.t2ksports.nba2k20and/files"

/*
 * Write small proof markers into this app's private files directory. These
 * distinguish the ELF constructor (linker load) from Agent_OnAttach (ART
 * callback). A failed marker write is diagnostic only and never aborts attach.
 */
static void write_marker(const char *name, const char *contents) {
    char path[256];
    const int path_len = snprintf(path, sizeof(path), "%s/%s",
                                  SHOTMETER_FILES_DIR, name);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
        LOGE("marker path formatting failed for %s", name);
        return;
    }

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOGE("cannot create marker %s: errno=%d (%s)", name, errno, strerror(errno));
        return;
    }

    const size_t length = strlen(contents);
    const ssize_t written = write(fd, contents, length);
    if (written < 0 || (size_t)written != length) {
        LOGE("incomplete marker write for %s: written=%ld expected=%lu errno=%d",
             name, (long)written, (unsigned long)length, errno);
    }
    close(fd);
}

/*
 * This runs when the dynamic linker maps libshotmeter.so, before ART calls
 * Agent_OnAttach. It is not proof that the JVMTI callback was invoked.
 */
__attribute__((constructor))
static void shotmeter_library_constructor(void) {
    write_marker("shotmeter.library-loaded", "ELF constructor executed\n");
    LOGI("libshotmeter.so ELF constructor executed");
}

/*
 * Android ART attaches this library through the JVMTI agent mechanism:
 *
 *   adb shell cmd activity attach-agent <process>
 *       /data/user/0/com.t2ksports.nba2k20and/files/libshotmeter.so
 *
 * The original APK remains unchanged; ART loads this external ELF into the
 * already-running debuggable game process and calls Agent_OnAttach.
 */
JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    (void)reserved;

    if (vm == NULL) {
        LOGE("Agent_OnAttach received a null JavaVM");
        return JNI_ERR;
    }

    write_marker("shotmeter.agent-attached", "Agent_OnAttach invoked\n");
    LOGI("Agent_OnAttach invoked by ART; options=%s",
         options != NULL ? options : "(none)");

    /*
     * The game should be running before the agent is attached, so its
     * NativeActivity library should already be mapped. The probe installs
     * read-only hooks and reports per-symbol failures to logcat.
     */
    const int status = shotmeter_probe_install();

    char result[64];
    (void)snprintf(result, sizeof(result), "probe_status=0x%x\n",
                   (unsigned int)status);
    write_marker("shotmeter.probe-result", result);

    char diagnostic[256];
    (void)snprintf(diagnostic, sizeof(diagnostic),
                   "probe_status=0x%x\nshadowhook_init_code=%d\nshadowhook_init_message=%s\n",
                   (unsigned int)status,
                   shotmeter_probe_get_init_error_code(),
                   shotmeter_probe_get_init_error_message());
    write_marker("shotmeter.init-diagnostic", diagnostic);

    if (status != 0) {
        LOGE("shotmeter_probe_install returned status mask 0x%x",
             (unsigned int)status);
    } else {
        LOGI("all read-only shotmeter capture hooks registered");
    }

    /*
     * Keep the agent loaded even if an individual hook is unavailable.
     * The marker records the status so hook resolution can be diagnosed
     * independently from successful ART attachment.
     */
    return JNI_OK;
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
    (void)vm;
    LOGI("ART is unloading libshotmeter.so");
}

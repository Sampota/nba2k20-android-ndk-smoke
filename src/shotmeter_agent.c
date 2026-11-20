#include <android/log.h>
#include <jni.h>
#include <stdint.h>

#include "shotmeter_probe.h"

#define LOG_TAG "ShotmeterAgent"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * Android ART attaches this library through the JVMTI agent mechanism:
 *
 *   adb shell cmd activity attach-agent <process>
 *       /data/user/0/<package>/files/libshotmeter.so
 *
 * This entry point is intentionally separate from JNI_OnLoad. The original
 * APK remains unchanged; ART loads this one external ELF into the already
 * running debuggable game process and calls Agent_OnAttach.
 */
JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    (void)reserved;

    if (vm == NULL) {
        LOGE("Agent_OnAttach received a null JavaVM");
        return JNI_ERR;
    }

    LOGI("JVMTI attach accepted by ART; options=%s",
         options != NULL ? options : "(none)");

    /*
     * The game should be running before the agent is attached, so its
     * NativeActivity library should already be mapped. The probe installs
     * read-only hooks and reports per-symbol failures to logcat.
     */
    const int status = shotmeter_probe_install();
    if (status != 0) {
        LOGE("shotmeter_probe_install returned status mask 0x%x",
             (unsigned int)status);
    } else {
        LOGI("all read-only shotmeter capture hooks registered");
    }

    /*
     * Keep the agent loaded even if an individual hook is unavailable.
     * This lets us inspect the detailed ShadowHook errors and refine symbol
     * resolution without making ART tear down a successfully loaded agent.
     */
    return JNI_OK;
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
    (void)vm;
    LOGI("ART is unloading libshotmeter.so");
}

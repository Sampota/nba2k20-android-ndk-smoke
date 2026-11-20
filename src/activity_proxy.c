#include <android/log.h>
#include <android/native_activity.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stddef.h>

#include "shotmeter_probe.h"

#define LOG_TAG "ShotmeterActivityProxy"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * The APK's NativeActivity metadata selects libnba2k20_clean_opt.so.
 * The untouched game ELF has DT_SONAME=libnba_clean_opt.so, so the planned
 * separate test APK will package it as libnba_clean_opt.so and this proxy
 * under the manifest-selected filename.
 */
static const char kOriginalLibrary[] = "libnba_clean_opt.so";

typedef void (*original_activity_on_create_fn)(
    ANativeActivity *activity, void *saved_state, size_t saved_state_size);
typedef jint (*original_jni_on_load_fn)(JavaVM *vm, void *reserved);

static pthread_mutex_t g_load_mutex = PTHREAD_MUTEX_INITIALIZER;
static void *g_original_handle = NULL;
static original_activity_on_create_fn g_original_activity_on_create = NULL;
static original_jni_on_load_fn g_original_jni_on_load = NULL;
static int g_original_load_status = 0; /* 0=not attempted, 1=ready, -1=failed */

static int ensure_original_library_loaded(void) {
    pthread_mutex_lock(&g_load_mutex);

    if (g_original_load_status == 0) {
        dlerror();
        g_original_handle = dlopen(kOriginalLibrary, RTLD_NOW | RTLD_LOCAL);
        if (g_original_handle == NULL) {
            const char *error = dlerror();
            LOGE("dlopen(%s) failed: %s", kOriginalLibrary,
                 error != NULL ? error : "unknown linker error");
            g_original_load_status = -1;
        } else {
            dlerror();
            g_original_activity_on_create = (original_activity_on_create_fn)
                dlsym(g_original_handle, "ANativeActivity_onCreate");
            const char *activity_error = dlerror();

            dlerror();
            g_original_jni_on_load = (original_jni_on_load_fn)
                dlsym(g_original_handle, "JNI_OnLoad");
            const char *jni_error = dlerror();

            if (g_original_activity_on_create == NULL || activity_error != NULL ||
                g_original_jni_on_load == NULL || jni_error != NULL) {
                LOGE("Original exports unavailable: ANativeActivity_onCreate=%p (%s), "
                     "JNI_OnLoad=%p (%s)",
                     (void *)g_original_activity_on_create,
                     activity_error != NULL ? activity_error : "ok",
                     (void *)g_original_jni_on_load,
                     jni_error != NULL ? jni_error : "ok");
                g_original_load_status = -1;
            } else {
                LOGI("Loaded untouched original ELF as %s: handle=%p "
                     "ANativeActivity_onCreate=%p JNI_OnLoad=%p",
                     kOriginalLibrary, g_original_handle,
                     (void *)g_original_activity_on_create,
                     (void *)g_original_jni_on_load);
                g_original_load_status = 1;
            }
        }
    }

    const int result = g_original_load_status;
    pthread_mutex_unlock(&g_load_mutex);
    return result;
}

/*
 * Forward Java VM initialization if the protected Application loads the
 * manifest-selected native library with System.loadLibrary(). NativeActivity
 * may instead load the same library directly; in that case this JNI entry
 * point is not assumed to have been called.
 */
__attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    if (ensure_original_library_loaded() != 1) {
        LOGE("Cannot forward JNI_OnLoad because the original library did not load");
        return JNI_ERR;
    }

    const jint version = g_original_jni_on_load(vm, reserved);
    LOGI("Forwarded original JNI_OnLoad, returned 0x%x", (unsigned int)version);
    return version;
}

/*
 * NativeActivity's required entry point. Hook installation happens only after
 * dlopen() has completed the original ELF's constructors and before the game
 * receives ANativeActivity_onCreate().
 */
__attribute__((visibility("default")))
void ANativeActivity_onCreate(
    ANativeActivity *activity, void *saved_state, size_t saved_state_size) {
    if (ensure_original_library_loaded() != 1) {
        LOGE("Cannot forward ANativeActivity_onCreate; refusing to call a null entry point");
        return;
    }

    const int probe_status = shotmeter_probe_install();
    LOGI("Read-only probe install returned status mask 0x%x", (unsigned int)probe_status);

    g_original_activity_on_create(activity, saved_state, saved_state_size);
}

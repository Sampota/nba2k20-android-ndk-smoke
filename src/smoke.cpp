#include <android/log.h>

namespace {
constexpr char kLogTag[] = "ShotmeterNdkSmoke";
}

extern "C" __attribute__((visibility("default")))
int shotmeter_ndk_smoke_probe() {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "Android NDK ARM64 smoke library loaded");
    return 0x2A20;
}

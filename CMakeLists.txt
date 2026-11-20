cmake_minimum_required(VERSION 4.0.2)
project(nba2k20_shotmeter_native LANGUAGES C CXX ASM)

include(FetchContent)

# Pin the upstream hook engine to a release tag; do not build from mutable main.
FetchContent_Declare(
    shadowhook_source
    GIT_REPOSITORY https://github.com/bytedance/android-inline-hook.git
    GIT_TAG v2.0.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR shadowhook/src/main/cpp
)
FetchContent_MakeAvailable(shadowhook_source)

# ShadowHook v2.0.1's upstream CMake enables -Weverything -Werror. NDK 30
# ships Clang 21, which adds diagnostics not fatal under its older toolchains.
# Keep upstream warnings visible but non-fatal; our own probe remains -Werror.
target_compile_options(shadowhook PRIVATE -Wno-error)
# Upstream uses one version map for both ARM and ARM64. Its ARM32-only VFP
# glue exports do not exist in the ARM64 build, and LLD rejects them as
# undefined version-script entries. Generate an ARM64-specific map with only
# those two unavailable ARM32 exports removed.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(_shadowhook_map_source
        "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/shadowhook.map.txt")
    set(_shadowhook_map_arm64
        "${shadowhook_source_BINARY_DIR}/shadowhook-arm64.map.txt")
    file(READ "${_shadowhook_map_source}" _shadowhook_map_contents)
    string(REPLACE "        shadowhook_interceptor_glue_vfpv3d16;\n" ""
        _shadowhook_map_contents "${_shadowhook_map_contents}")
    string(REPLACE "        shadowhook_interceptor_glue_vfpv3d32;\n" ""
        _shadowhook_map_contents "${_shadowhook_map_contents}")
    file(WRITE "${_shadowhook_map_arm64}" "${_shadowhook_map_contents}")

    get_target_property(_shadowhook_link_options shadowhook LINK_OPTIONS)
    list(FILTER _shadowhook_link_options EXCLUDE REGEX "version-script=")
    set_property(TARGET shadowhook PROPERTY LINK_OPTIONS "${_shadowhook_link_options}")
    target_link_options(shadowhook PRIVATE
        "-Wl,--version-script=${_shadowhook_map_arm64}")
endif()

find_library(android_log log REQUIRED)

# Keep the original toolchain smoke target as a separate, harmless check.
add_library(shotmeter_ndk_smoke SHARED src/smoke.cpp)
target_link_libraries(shotmeter_ndk_smoke PRIVATE ${android_log})
target_compile_features(shotmeter_ndk_smoke PRIVATE cxx_std_17)
target_compile_options(shotmeter_ndk_smoke PRIVATE -Wall -Wextra -Werror)

# Read-only instrumentation. This does not change game state or render UI.
add_library(shotmeter_probe SHARED src/shotmeter_probe.c)
target_include_directories(shotmeter_probe PRIVATE
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/include"
)
target_link_libraries(shotmeter_probe PRIVATE shadowhook ${android_log})
target_compile_features(shotmeter_probe PRIVATE c_std_11)
target_compile_options(shotmeter_probe PRIVATE
    -Wall -Wextra -Werror
    -fvisibility=hidden
)
set_target_properties(shotmeter_probe PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# Manifest-selected NativeActivity entry library for a separate test APK.
# The clean original game ELF is not linked or modified; the proxy dlopens it
# under its existing DT_SONAME and forwards the original NativeActivity/JNI APIs.
find_library(android_dl dl REQUIRED)
add_library(shotmeter_activity_proxy SHARED src/activity_proxy.c)
target_include_directories(shotmeter_activity_proxy PRIVATE src)
target_link_libraries(shotmeter_activity_proxy PRIVATE
    shotmeter_probe
    ${android_log}
    ${android_dl}
)
target_compile_features(shotmeter_activity_proxy PRIVATE c_std_11)
target_compile_options(shotmeter_activity_proxy PRIVATE
    -Wall -Wextra -Werror
    -fvisibility=hidden
)
set_target_properties(shotmeter_activity_proxy PROPERTIES
    OUTPUT_NAME "nba2k20_clean_opt"
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

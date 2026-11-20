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
# undefined version-script entries. Generate an ARM64-specific map.
set(_shadowhook_map_source
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/shadowhook.map.txt")
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(_shadowhook_arch arm64)
    set(_shadowhook_arch_link_flags "-Wl,-z,max-page-size=16384")
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
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(_shadowhook_arch arm)
    set(_shadowhook_arch_link_flags "")
    file(READ "${_shadowhook_map_source}" _shadowhook_map_contents)
else()
    message(FATAL_ERROR "Unsupported ABI for the shotmeter agent: ${ANDROID_ABI}")
endif()

# Build ShadowHook's upstream sources as position-independent objects and
# merge them into libshotmeter.so. The final agent is a single-ELF deliverable;
# it must not depend on a separately installed libshadowhook.so.
file(GLOB _shadowhook_embedded_sources CONFIGURE_DEPENDS
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/*.c"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/arch/${_shadowhook_arch}/*.c"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/arch/${_shadowhook_arch}/*.S"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/common/*.c"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/third_party/*/*.c"
)
add_library(shadowhook_embedded OBJECT ${_shadowhook_embedded_sources})
target_compile_features(shadowhook_embedded PRIVATE c_std_11)
target_compile_options(shadowhook_embedded PRIVATE
    -Wno-error
    -Os
    -ffunction-sections
    -fdata-sections
)
target_include_directories(shadowhook_embedded PRIVATE
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/include"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/arch/${_shadowhook_arch}"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/common"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/third_party/xdl"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/third_party/bsd"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/third_party/lss"
)
set_target_properties(shadowhook_embedded PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Keep the upstream shared targets available for the historical probe/proxy
# experiments. They are not part of the new single-file agent artifact.
find_library(android_log log REQUIRED)
find_library(android_dl dl REQUIRED)

# Keep the original toolchain smoke target as a separate, harmless check.
add_library(shotmeter_ndk_smoke SHARED src/smoke.cpp)
target_link_libraries(shotmeter_ndk_smoke PRIVATE ${android_log})
target_compile_features(shotmeter_ndk_smoke PRIVATE cxx_std_17)
target_compile_options(shotmeter_ndk_smoke PRIVATE -Wall -Wextra -Werror)

# Historical standalone read-only probe and proxy candidates.
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

# JVMTI attach agent for the original debuggable APK. Agent_OnAttach is the
# Android ART entry point; it installs the read-only hooks in the current game
# process. ShadowHook's implementation is embedded into this same ELF.
set(_shotmeter_agent_map
    "${shadowhook_source_BINARY_DIR}/shotmeter-agent.map.txt")
string(REPLACE "    global:\n"
    "    global:\n        Agent_OnAttach;\n        Agent_OnUnload;\n"
    _shotmeter_agent_map_contents "${_shadowhook_map_contents}")
file(WRITE "${_shotmeter_agent_map}" "${_shotmeter_agent_map_contents}")

add_library(shotmeter_agent SHARED
    src/shotmeter_agent.c
    src/shotmeter_probe.c
    ${_shadowhook_embedded_sources}
)
target_include_directories(shotmeter_agent PRIVATE
    src
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/include"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/arch/${_shadowhook_arch}"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/common"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/third_party/xdl"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/third_party/bsd"
    "${shadowhook_source_SOURCE_DIR}/shadowhook/src/main/cpp/third_party/lss"
)
target_link_libraries(shotmeter_agent PRIVATE ${android_log} ${android_dl})
target_compile_features(shotmeter_agent PRIVATE c_std_11)
target_compile_options(shotmeter_agent PRIVATE
    -Wall -Wextra -Werror
    -Wno-error
    -Os
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
)
target_link_options(shotmeter_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Os
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_agent_map}"
)
set_target_properties(shotmeter_agent PROPERTIES
    OUTPUT_NAME "shotmeter"
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

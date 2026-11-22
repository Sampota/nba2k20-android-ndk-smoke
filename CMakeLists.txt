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

# Dobby is the v38 diagnostic hook engine. Pin an immutable commit and build
# its static target into a single JVMTI agent ELF; no Dobby .so is deployed.
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(NearBranch ON CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
set(Plugin.Android.BionicLinkerUtil OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    dobby_source
    GIT_REPOSITORY https://github.com/jmpews/Dobby.git
    GIT_TAG 5dfc8546954ce3b3198132ab13fddb89ee92cdd7
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(dobby_source)
set_target_properties(dobby_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Dobby's POSIX code-patching source includes headers relative to its
# source/ directory; expose that root explicitly on both upstream targets.
target_include_directories(dobby PRIVATE "${dobby_source_SOURCE_DIR}/source")
target_include_directories(dobby_static PRIVATE "${dobby_source_SOURCE_DIR}/source")
set_property(TARGET dobby PROPERTY EXCLUDE_FROM_ALL TRUE)

# This pinned Dobby POSIX file still includes a removed Cpu.h. Its only
# required ClearCache declaration is already provided by dobby_internal.h.
set(_dobby_posix_patch_source
    "${dobby_source_SOURCE_DIR}/source/Backend/UserMode/ExecMemory/code-patch-tool-posix.cc")
file(READ "${_dobby_posix_patch_source}" _dobby_posix_patch_contents)
string(REPLACE
    "core/arch/Cpu.h"
    "PlatformUnifiedInterface/ExecMemory/ClearCacheTool.h"
    _dobby_posix_patch_contents "${_dobby_posix_patch_contents}")
file(WRITE "${_dobby_posix_patch_source}" "${_dobby_posix_patch_contents}")

# The pinned upstream ARM64 bridge assembly spells its PC-relative symbol
# relocations using Mach-O @PAGE/@PAGEOFF even in its Android source list.
# Rewrite only these two operands to their equivalent AArch64 ELF relocations.
set(_dobby_arm64_bridge
    "${dobby_source_SOURCE_DIR}/source/TrampolineBridge/ClosureTrampolineBridge/arm64/closure_bridge_arm64.asm")
file(READ "${_dobby_arm64_bridge}" _dobby_arm64_bridge_contents)
string(REPLACE
    "adrp TMP_REG_0, cdecl(common_closure_bridge_handler)@PAGE"
    "adrp TMP_REG_0, :got:cdecl(common_closure_bridge_handler)"
    _dobby_arm64_bridge_contents "${_dobby_arm64_bridge_contents}")
string(REPLACE
    "add TMP_REG_0, TMP_REG_0, cdecl(common_closure_bridge_handler)@PAGEOFF"
    "ldr TMP_REG_0, [TMP_REG_0, :got_lo12:cdecl(common_closure_bridge_handler)]"
    _dobby_arm64_bridge_contents "${_dobby_arm64_bridge_contents}")
file(WRITE "${_dobby_arm64_bridge}" "${_dobby_arm64_bridge_contents}")

# The pinned Dobby revision names the ELF runtime-module base
# `load_address` in its Linux/Android implementation but exposes only
# `base` in the shared RuntimeModule structure. Keep both field spellings
# as an anonymous union so the Android/Linux and Mach-O backends compile.
set(_dobby_runtime_module_header
    "${dobby_source_SOURCE_DIR}/source/PlatformUtil/ProcessRuntime.h")
file(READ "${_dobby_runtime_module_header}" _dobby_runtime_module_contents)
string(REPLACE
    "  void *base;\n"
    "  union { void *base; void *load_address; };\n"
    _dobby_runtime_module_contents "${_dobby_runtime_module_contents}")
file(WRITE "${_dobby_runtime_module_header}" "${_dobby_runtime_module_contents}")

# The same pinned Dobby revision still uses the pre-accessor MemRange field
# name in its Linux/Android comparator.
set(_dobby_linux_runtime_source
    "${dobby_source_SOURCE_DIR}/source/Backend/UserMode/PlatformUtil/Linux/ProcessRuntime.cc")
file(READ "${_dobby_linux_runtime_source}" _dobby_linux_runtime_contents)
string(REPLACE
    "return (a.start < b.start);"
    "return (a.start() < b.start());"
    _dobby_linux_runtime_contents "${_dobby_linux_runtime_contents}")
file(WRITE "${_dobby_linux_runtime_source}" "${_dobby_linux_runtime_contents}")

# Avoid the upstream os_arch_features.h -> platform.h -> common.h include
# cycle for Android: use the equivalent POSIX mprotect call locally.
set(_dobby_os_arch_header
    "${dobby_source_SOURCE_DIR}/common/os_arch_features.h")
file(READ "${_dobby_os_arch_header}" _dobby_os_arch_contents)
string(REPLACE
    "#include <stddef.h>\n"
    "#include <stddef.h>\n#if defined(ANDROID)\n#include <sys/mman.h>\n#include <unistd.h>\n#endif\n"
    _dobby_os_arch_contents "${_dobby_os_arch_contents}")
string(REPLACE
    "  auto page = (void *)ALIGN_FLOOR(address, OSMemory::PageSize());\n  if (!OSMemory::SetPermission(page, OSMemory::PageSize(), kReadExecute)) {\n    return;\n  }"
    "  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);\n  auto page = (void *)ALIGN_FLOOR(address, page_size);\n  (void)size;\n  if (mprotect(page, page_size, PROT_READ | PROT_EXEC) != 0) {\n    return;\n  }"
    _dobby_os_arch_contents "${_dobby_os_arch_contents}")
file(WRITE "${_dobby_os_arch_header}" "${_dobby_os_arch_contents}")

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
    $<TARGET_OBJECTS:shadowhook_embedded>
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

# Isolated v38 Dobby diagnostic agent. It hooks only a local test function
# to validate ARM64 code patching/trampoline behavior before touching game code.
set(_shotmeter_dobby_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby.map.txt")
file(WRITE "${_shotmeter_dobby_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")

add_library(shotmeter_dobby_agent SHARED src/shotmeter_dobby_agent.cpp)
target_link_libraries(shotmeter_dobby_agent PRIVATE
    dobby_static
    ${android_log}
    ${android_dl}
)
target_compile_features(shotmeter_dobby_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_agent PRIVATE
    -Wall -Wextra -Werror
    -fvisibility=hidden
)
target_link_options(shotmeter_dobby_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_agent_map}"
)
set_target_properties(shotmeter_dobby_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v38"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# v39 keeps the proven v38 self-test and adds three observation-only hooks
# against symbols in an already-loaded NBA 2K20 ELF. Dobby remains embedded.
set(_shotmeter_dobby_v39_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v39.map.txt")
file(WRITE "${_shotmeter_dobby_v39_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")

add_library(shotmeter_dobby_v39_agent SHARED src/shotmeter_dobby_v39_agent.cpp)
target_link_libraries(shotmeter_dobby_v39_agent PRIVATE
    dobby_static
    ${android_log}
    ${android_dl}
)
target_compile_features(shotmeter_dobby_v39_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v39_agent PRIVATE
    -Wall -Wextra -Werror
    -fvisibility=hidden
)
target_link_options(shotmeter_dobby_v39_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v39_agent_map}"
)
set_target_properties(shotmeter_dobby_v39_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v39"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)


# v40 adds read-only taps for the native timing classifier, its six-value
# threshold output, its class-specific bounds helper, and the central release
# event. Dobby is embedded; this target remains a single ELF agent.
set(_shotmeter_dobby_v40_timing_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v40-timing.map.txt")
file(WRITE "${_shotmeter_dobby_v40_timing_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")

add_library(shotmeter_dobby_v40_timing_agent SHARED
    src/shotmeter_dobby_v40_timing_agent.cpp)
target_link_libraries(shotmeter_dobby_v40_timing_agent PRIVATE
    dobby_static
    ${android_log}
    ${android_dl}
)
target_compile_features(shotmeter_dobby_v40_timing_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v40_timing_agent PRIVATE
    -Wall -Wextra -Werror
    -fvisibility=hidden
)
target_link_options(shotmeter_dobby_v40_timing_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v40_timing_agent_map}"
)
set_target_properties(shotmeter_dobby_v40_timing_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v40"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# v41 adds read-only observation of the native stock feedback visibility/draw path
# and a reset marker handled by the long-lived diagnostic reporter.
set(_shotmeter_dobby_v41_feedback_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v41-feedback.map.txt")
file(WRITE "${_shotmeter_dobby_v41_feedback_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v41_feedback_agent SHARED
    src/shotmeter_dobby_v41_feedback_agent.cpp)
target_link_libraries(shotmeter_dobby_v41_feedback_agent PRIVATE dobby_static ${android_log} ${android_dl})
target_compile_features(shotmeter_dobby_v41_feedback_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v41_feedback_agent PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v41_feedback_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v41_feedback_agent_map}")
set_target_properties(shotmeter_dobby_v41_feedback_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v41"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# v42 draws an independent screen-space second shotmeter on the EGL backbuffer.
# It reads the stock ShotIndicatorComponent progress field only; it does not
# modify the component, its material/resource, or the original shot timing.
find_library(android_egl EGL REQUIRED)
find_library(android_glesv3 GLESv3 REQUIRED)
find_library(android_opensles OpenSLES REQUIRED)
set(_shotmeter_dobby_v42_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v42.map.txt")
file(WRITE "${_shotmeter_dobby_v42_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v42_second_meter_agent SHARED
    src/shotmeter_dobby_v42_second_meter.cpp)
target_link_libraries(shotmeter_dobby_v42_second_meter_agent PRIVATE
    dobby_static ${android_log} ${android_dl} ${android_egl} ${android_glesv3})
target_compile_features(shotmeter_dobby_v42_second_meter_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v42_second_meter_agent PRIVATE
    -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v42_second_meter_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v42_agent_map}")
set_target_properties(shotmeter_dobby_v42_second_meter_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v42"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)


# v43 is a read-only player-anchor diagnostic. It captures the player's head
# world position and projects it through the saved player-icon view from the
# stock DoDraw callback. It does not draw or modify any game-owned state.
set(_shotmeter_dobby_v43_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v43.map.txt")
file(WRITE "${_shotmeter_dobby_v43_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v43_coordinate_agent SHARED
    src/shotmeter_dobby_v43_coordinate_diagnostic.cpp)
target_link_libraries(shotmeter_dobby_v43_coordinate_agent PRIVATE
    dobby_static ${android_log} ${android_dl})
target_compile_features(shotmeter_dobby_v43_coordinate_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v43_coordinate_agent PRIVATE
    -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v43_coordinate_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v43_agent_map}")
set_target_properties(shotmeter_dobby_v43_coordinate_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v43"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)


# v44 keeps the v43 read-only player-anchor diagnostic and makes Agent_OnAttach
# idempotent. A second attach call in the same loaded ELF is counted and ignored,
# so it cannot re-run the Dobby self-test or overwrite a successful hook state.
set(_shotmeter_dobby_v44_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v44.map.txt")
file(WRITE "${_shotmeter_dobby_v44_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v44_coordinate_agent
    SHARED src/shotmeter_dobby_v44_coordinate_diagnostic.cpp)
target_link_libraries(shotmeter_dobby_v44_coordinate_agent PRIVATE
    dobby_static ${android_log} ${android_dl})
target_compile_features(shotmeter_dobby_v44_coordinate_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v44_coordinate_agent PRIVATE
    -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v44_coordinate_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v44_agent_map}")
set_target_properties(shotmeter_dobby_v44_coordinate_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v44"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)


# v46 combines the validated v45 perspective player-head projection with the
# v42 independent EGL overlay. The curved meter is drawn beside the live
# projected head anchor; stock component and timing data remain read-only.
set(_shotmeter_dobby_v46_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v46.map.txt")
file(WRITE "${_shotmeter_dobby_v46_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v46_player_anchored_agent
    SHARED src/shotmeter_dobby_v46_player_anchored_meter.cpp)
target_link_libraries(shotmeter_dobby_v46_player_anchored_agent PRIVATE
    dobby_static ${android_log} ${android_dl} ${android_egl} ${android_glesv3})
target_compile_features(shotmeter_dobby_v46_player_anchored_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v46_player_anchored_agent PRIVATE
    -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v46_player_anchored_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v46_agent_map}")
set_target_properties(shotmeter_dobby_v46_player_anchored_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v46"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)


# v47 replaces the small circular strip with a tall tapered crescent meter.
# Class 3 from EVT_UserShotReleased turns the whole crescent green, flashes
# spark accents, and plays a short OpenSL ES confirmation tone. Game state,
# timing, and the original meter remain read-only.
set(_shotmeter_dobby_v47_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v47.map.txt")
file(WRITE "${_shotmeter_dobby_v47_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v47_crescent_agent
    SHARED src/shotmeter_dobby_v47_crescent_perfect_meter.cpp)
target_link_libraries(shotmeter_dobby_v47_crescent_agent PRIVATE
    dobby_static ${android_log} ${android_dl} ${android_egl} ${android_glesv3} ${android_opensles})
target_compile_features(shotmeter_dobby_v47_crescent_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v47_crescent_agent PRIVATE
    -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v47_crescent_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v47_agent_map}")
set_target_properties(shotmeter_dobby_v47_crescent_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v47"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)

# v48 progressive fill and full-green perfect-release feedback.
set(_shotmeter_dobby_v48_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v48.map.txt")
file(WRITE "${_shotmeter_dobby_v48_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v48_progressive_agent SHARED
    src/shotmeter_dobby_v48_progressive_perfect_meter.cpp)
target_link_libraries(shotmeter_dobby_v48_progressive_agent PRIVATE
    dobby_static ${android_log} ${android_dl} ${android_egl} ${android_glesv3} ${android_opensles})
target_compile_features(shotmeter_dobby_v48_progressive_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v48_progressive_agent PRIVATE
    -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v48_progressive_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v48_agent_map}")
set_target_properties(shotmeter_dobby_v48_progressive_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v48"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)

 
# v49 combines the validated player anchor with the shader-rendered horn and
# the embedded Green Giant feedback sound. It reuses the single EGL swap hook
# installed by this agent; game state and stock timing remain read-only.
set(_shotmeter_dobby_v49_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v49.map.txt")
file(WRITE "${_shotmeter_dobby_v49_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v49_smooth_horn_agent
    SHARED src/shotmeter_dobby_v49_smooth_opengles_horn.cpp)
target_link_libraries(shotmeter_dobby_v49_smooth_horn_agent PRIVATE
    dobby_static ${android_log} ${android_dl} ${android_egl} ${android_glesv3} ${android_opensles})
target_compile_features(shotmeter_dobby_v49_smooth_horn_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v49_smooth_horn_agent PRIVATE
    -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v49_smooth_horn_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v49_agent_map}")
set_target_properties(shotmeter_dobby_v49_smooth_horn_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v49"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)


# v50.1 fixes the copied v49 diagnostic identity and samples EGL/GL state on
# ordinary swap frames, not only while the meter is active. Probability hooks are
# deliberately disabled until function ABI and outcome flags are verified by disassembly.
set(_shotmeter_dobby_v50r1_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v50r1.map.txt")
file(WRITE "${_shotmeter_dobby_v50r1_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v50r1_audited_agent
    SHARED src/shotmeter_dobby_v50_render_probability.cpp)
target_link_libraries(shotmeter_dobby_v50r1_audited_agent PRIVATE
    dobby_static ${android_log} ${android_dl} ${android_egl} ${android_glesv3} ${android_opensles})
target_compile_features(shotmeter_dobby_v50r1_audited_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v50r1_audited_agent PRIVATE
    -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v50r1_audited_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v50r1_agent_map}")
set_target_properties(shotmeter_dobby_v50r1_audited_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v50r1"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)


# v51 applies grade-aware probability remapping only at the balanced-dice
# simulation entry. The game's stored chance and HUD grade feedback remain intact.
set(_shotmeter_dobby_v51_agent_map
    "${CMAKE_CURRENT_BINARY_DIR}/shotmeter-agent-dobby-v51.map.txt")
file(WRITE "${_shotmeter_dobby_v51_agent_map}"
    "{ global: Agent_OnAttach; Agent_OnUnload; local: *; };")
add_library(shotmeter_dobby_v51_grade_aware_agent
    SHARED src/shotmeter_dobby_v51_grade_aware_probability.cpp)
target_link_libraries(shotmeter_dobby_v51_grade_aware_agent PRIVATE
    dobby_static ${android_log} ${android_dl} ${android_egl} ${android_glesv3} ${android_opensles})
target_compile_features(shotmeter_dobby_v51_grade_aware_agent PRIVATE cxx_std_17)
target_compile_options(shotmeter_dobby_v51_grade_aware_agent PRIVATE
    -Wall -Wextra -Werror -fvisibility=hidden)
target_link_options(shotmeter_dobby_v51_grade_aware_agent PRIVATE
    ${_shadowhook_arch_link_flags}
    -Wl,--exclude-libs,ALL
    -Wl,--gc-sections
    "-Wl,--version-script=${_shotmeter_dobby_v51_agent_map}")
set_target_properties(shotmeter_dobby_v51_grade_aware_agent PROPERTIES
    OUTPUT_NAME "shotmeter-v51"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)

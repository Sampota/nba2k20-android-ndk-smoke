cmake_minimum_required(VERSION 3.22.1)
project(shotmeter_ndk_smoke LANGUAGES C CXX)

add_library(shotmeter_ndk_smoke SHARED src/smoke.cpp)

find_library(android_log log REQUIRED)
target_link_libraries(shotmeter_ndk_smoke PRIVATE ${android_log})

target_compile_features(shotmeter_ndk_smoke PRIVATE cxx_std_17)
target_compile_options(shotmeter_ndk_smoke PRIVATE -Wall -Wextra -Werror)

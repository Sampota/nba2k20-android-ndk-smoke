# CMakeLists.txt : builds libhornmeter.so (horn shot meter + shot feedback banner)
#
# Folder layout expected:
#   CMakeLists.txt
#   horn_meter.cpp
#   horn_font_atlas.h
#   Dobby/               <- git clone https://github.com/jmpews/Dobby
#
# Build (arm64 phone):
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
#   cmake --build build
# Result: build/libhornmeter.so
#
# If your Dobby version names its target or include folder differently, adjust the
# add_subdirectory / target_include_directories lines below (see Dobby's README).

cmake_minimum_required(VERSION 3.18)
project(hornmeter CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# build Dobby as a static library and link it into our .so
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(Dobby dobby)

add_library(hornmeter SHARED horn_meter.cpp)
target_include_directories(hornmeter PRIVATE Dobby/include)
target_link_libraries(hornmeter dobby EGL GLESv3 log)

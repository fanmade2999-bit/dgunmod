cmake_minimum_required(VERSION 3.18)
project(netmod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Add your UDP transport sources here (the code that implements
# netmod_net_start / netmod_update_local_transform / netmod_get_remote_transform).
add_library(netmod SHARED
    netmod_boot.cpp
    netmod_hook.cpp
    netmod_udp.cpp
)

target_compile_options(netmod PRIVATE -O3 -Wall -Wextra -fvisibility=hidden)

# 16 KB page-size compatibility (required for newer Android devices), arm64 only.
target_link_options(netmod PRIVATE -Wl,-z,max-page-size=16384 -Wl,--gc-sections)

target_link_libraries(netmod PRIVATE log dl)

# Build:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release
#   cmake --build build        # -> build/libnetmod.so

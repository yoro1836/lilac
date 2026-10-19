cmake_minimum_required(VERSION 3.22.1)

project("vine_runtime" VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Disk images are far bigger than 2 GB, so off_t must be 64 bit on the 32 bit ABIs too
add_compile_definitions(_FILE_OFFSET_BITS=64)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(
        -Wall -Wextra -Wpedantic -Wshadow
        -Wno-unused-parameter
        -Wno-gnu-zero-variadic-macro-arguments
        -fstack-protector-strong
        -DVINE_DEBUG=1
    )
else()
    add_compile_options(
        -Wall -Wextra
        -Wno-unused-parameter
        -O2
        -fstack-protector-strong
    )
endif()

if(ANDROID_PLATFORM_LEVEL LESS 26)
    message(FATAL_ERROR
        "VineOS requires Android API 26+ (minSdk=26). "
        "Current ANDROID_PLATFORM_LEVEL=${ANDROID_PLATFORM_LEVEL}")
endif()

find_library(log-lib log)
find_library(android-lib android)
find_library(z-lib z)

# Engine, helper protocol and display reader, the host tests build the same sources (tests/native/run_tests.sh)
add_library(vine_core STATIC
    utils/vine_utils.cpp
    container/boot_props.cpp
    container/guest_exec.cpp
    container/loop_mount.cpp
    container/namespace_manager.cpp
    container/net_setup.cpp
    container/ramdisk.cpp
    container/sparse_image.cpp
    diagnostics/host_probe.cpp
    diagnostics/namespace_probe.cpp
    display/pixel_convert.cpp
    display/vinefb_reader.cpp
    helper/agent_link.cpp
    helper/helper_server.cpp
    qemu_bridge/qemu_launcher.cpp
)
target_include_directories(vine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vine_core PUBLIC ${log-lib} ${z-lib})

# Loaded into the app process: device probing and putting guest frames on a Surface
add_library(vine_runtime SHARED
    vine_runtime.cpp
    display/display_bridge.cpp
)
target_link_libraries(vine_runtime PRIVATE vine_core ${android-lib})
target_link_options(vine_runtime PRIVATE -Wl,-z,max-page-size=16384)

# Run as root via su, named lib*.so so Android extracts it to the native library directory
add_executable(vine_helper helper/helper_main.cpp)
target_link_libraries(vine_helper PRIVATE vine_core)
target_link_options(vine_helper PRIVATE -pie -Wl,-z,max-page-size=16384)
set_target_properties(vine_helper PROPERTIES PREFIX "lib" SUFFIX ".so" OUTPUT_NAME "vine_helper")
if(CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    # Gradle only packages what lands next to the libraries
    set_target_properties(vine_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
endif()

if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    target_compile_definitions(vine_core PUBLIC VINE_HOST_ARM64=1)
elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
    target_compile_definitions(vine_core PUBLIC VINE_HOST_ARM32=1)
elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
    target_compile_definitions(vine_core PUBLIC VINE_HOST_X86_64=1)
elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86")
    target_compile_definitions(vine_core PUBLIC VINE_HOST_X86=1)
endif()

message(STATUS "VineOS native: ABI=${CMAKE_ANDROID_ARCH_ABI} API=${ANDROID_PLATFORM_LEVEL} BUILD=${CMAKE_BUILD_TYPE}")

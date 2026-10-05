cmake_minimum_required(VERSION 3.16)
project(WALnut VERSION 0.1.0 LANGUAGES CXX)

# =============================================================================
# WALnut – Storage / Buffer Module (Vinayak Suyal)
# Team: The Acid Mechanics (T038) — OSDBMS-V-2026-T038
#
# This CMakeLists.txt builds:
#   1. walnut_storage  – static library (storage + buffer layer)
#   2. walnut_demo     – demo executable (src/main.cpp)
#   3. walnut_tests    – GoogleTest executable for all storage/buffer tests
#
# Usage:
#   mkdir -p build && cd build
#   cmake ..
#   cmake --build . -j
#   ctest --output-on-failure
# =============================================================================

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ---------------------------------------------------------------------------
# Default build type
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
add_compile_options(
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wshadow>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-unused-parameter>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
set(WALNUT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Source files for the walnut_storage library
# ---------------------------------------------------------------------------
set(WALNUT_SOURCES
    src/common/utils.cpp
    src/common/types.cpp
    src/storage/page.cpp
    src/storage/metadata.cpp
    src/storage/file_manager.cpp
    src/storage/storage_manager.cpp
    src/buffer/frame.cpp
    src/buffer/buffer_pool.cpp
)

# ---------------------------------------------------------------------------
# walnut_storage – static library
# ---------------------------------------------------------------------------
add_library(walnut_storage STATIC ${WALNUT_SOURCES})

target_include_directories(walnut_storage
    PUBLIC  ${WALNUT_INCLUDE_DIR}
    PRIVATE ${WALNUT_INCLUDE_DIR}
)

# Link filesystem library on older GCC/Clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    target_link_libraries(walnut_storage PUBLIC stdc++fs)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(walnut_storage PUBLIC c++fs)
endif()

# Threads (for BufferPool mutex)
find_package(Threads REQUIRED)
target_link_libraries(walnut_storage PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# walnut_demo – demo executable
# ---------------------------------------------------------------------------
add_executable(walnut_demo src/main.cpp)
target_link_libraries(walnut_demo PRIVATE walnut_storage)

# Ensure data/ directory exists next to the demo binary.
add_custom_command(TARGET walnut_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:walnut_demo>/data"
    COMMENT "Creating data/ directory for walnut_demo"
)

# ---------------------------------------------------------------------------
# GoogleTest
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Suppress installing GTest with the project.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
# On Windows, avoid forcing shared CRT.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------------------------
# walnut_tests – test executable
# ---------------------------------------------------------------------------
set(WALNUT_TEST_SOURCES
    tests/storage/test_page.cpp
    tests/storage/test_file_manager.cpp
    tests/storage/test_storage_manager.cpp
    tests/buffer/test_frame.cpp
    tests/buffer/test_buffer_pool.cpp
)

add_executable(walnut_tests ${WALNUT_TEST_SOURCES})
target_include_directories(walnut_tests PRIVATE ${WALNUT_INCLUDE_DIR})
target_link_libraries(walnut_tests
    PRIVATE
        walnut_storage
        GTest::gtest
        GTest::gtest_main
)

# Ensure data/ directory exists next to tests binary (for file_manager tests).
add_custom_command(TARGET walnut_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:walnut_tests>/data"
    COMMENT "Creating data/ directory for walnut_tests"
)

# ---------------------------------------------------------------------------
# CTest integration
# ---------------------------------------------------------------------------
enable_testing()
include(GoogleTest)
gtest_discover_tests(walnut_tests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:walnut_tests>
    PROPERTIES        TIMEOUT 60
)

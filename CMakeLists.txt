cmake_minimum_required(VERSION 3.22)
project(papaya VERSION 0.0.1 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_papaya_top ON)
else()
    set(_papaya_top OFF)
endif()

option(PAPAYA_SHARED_LIBS "Build our libraries and the third-party ones as shared libraries" ON)
option(PAPAYA_TESTS "Build the tests" ${_papaya_top})
option(PAPAYA_RUST "Build the Rust crates (needs cargo)" ON)
option(PAPAYA_SPIKES "Build the phase 0 spike programs" OFF)
option(PAPAYA_WERROR "Treat warnings in our own code as errors" ON)
set(PAPAYA_SANITIZE "" CACHE STRING "Sanitizers for our code, e.g. address;undefined or thread")

# TECH-STACK.md step 1: clang everywhere (clang-cl on Windows).
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "PapayaPlayer builds with clang only (clang-cl on Windows); found "
        "${CMAKE_C_COMPILER_ID}. Use build.ps1 / build.sh or set CC/CXX.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(_papaya_top AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(BUILD_SHARED_LIBS ${PAPAYA_SHARED_LIBS})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/../lib")
elseif(UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PapayaWarnings)
include(GenerateExportHeader)

# Reaktor builds itself and SDL as static libraries whatever BUILD_SHARED_LIBS says.
add_subdirectory(third_party/reaktor EXCLUDE_FROM_ALL)

if(PAPAYA_RUST)
    add_subdirectory(third_party/corrosion EXCLUDE_FROM_ALL)
    if(PAPAYA_SHARED_LIBS)
        set(_papaya_crate_type cdylib)
    else()
        set(_papaya_crate_type staticlib)
    endif()
    corrosion_import_crate(MANIFEST_PATH rust/Cargo.toml CRATE_TYPES ${_papaya_crate_type})
endif()

add_subdirectory(papaya)
add_subdirectory(app)

if(PAPAYA_SPIKES)
    reaktor_add_app(spike_video_texture MODULES none SOURCES spikes/video_texture.c)
endif()

if(PAPAYA_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

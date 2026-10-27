# Third-party libraries, each from its git submodule, built with its own CMake
# and configured from here (TECH-STACK.md wrapper-first policy: settings live in
# our CMake, never in their files). Options are cache values because several of
# these projects predate CMP0077 and ignore plain variables.
macro(papaya_off)
    foreach(_opt ${ARGN})
        set(${_opt} OFF CACHE BOOL "" FORCE)
    endforeach()
endmacro()

# Code from libraries without DLL support (libwebm, miniaudio) is linked into
# our shared wrappers, so everything is position independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(_tp "${PROJECT_SOURCE_DIR}/third_party")

# Writes a generated file only when its content changes, always with Unix line
# endings: libvpx's rtcd.pl reads "yes<CR>" from a CRLF config as not "yes".
# Called as papaya_generate(OUTPUT path CONTENT text); the arguments are read
# by position so semicolons and quotes in text survive.
function(papaya_generate)
    file(CONFIGURE OUTPUT "${ARGV1}" CONTENT "${ARGV3}" NEWLINE_STYLE UNIX)
endfunction()

# libogg
papaya_off(INSTALL_DOCS INSTALL_PKG_CONFIG_MODULE INSTALL_CMAKE_PACKAGE_MODULE)
add_subdirectory("${_tp}/ogg" EXCLUDE_FROM_ALL)
if(NOT TARGET Ogg::ogg)
    add_library(Ogg::ogg ALIAS ogg)
endif()

# libvorbis (finds Ogg through cmake/FindOgg.cmake). Its cmake_minimum_required
# of 2.8.12 is refused by CMake 4 unless a minimum policy version is given.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory("${_tp}/vorbis" EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# libopus
papaya_off(OPUS_BUILD_PROGRAMS OPUS_BUILD_TESTING OPUS_INSTALL_PKG_CONFIG_MODULE
           OPUS_INSTALL_CMAKE_CONFIG_MODULE)
set(OPUS_BUILD_SHARED_LIBRARY ${PAPAYA_SHARED_LIBS} CACHE BOOL "" FORCE)
add_subdirectory("${_tp}/opus" EXCLUDE_FROM_ALL)
# Opus gives its SIMD files their -m flags only for non-MSVC compilers; MSVC
# itself needs none, but clang-cl does.
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    foreach(_simd sse:-msse sse2:-msse2 sse4_1:-msse4.1 avx2:-mavx2;-mfma)
        string(REGEX MATCH "^[^:]+" _suffix "${_simd}")
        string(REGEX REPLACE "^[^:]+:" "" _flags "${_simd}")
        file(GLOB _files "${_tp}/opus/*/x86/*_${_suffix}.c" "${_tp}/opus/dnn/x86/*_${_suffix}.c")
        set_source_files_properties(${_files} DIRECTORY "${_tp}/opus"
                                    PROPERTIES COMPILE_OPTIONS "${_flags}")
    endforeach()
endif()

# libFLAC only: its programs are GPL and never built.
papaya_off(BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_TESTING BUILD_DOCS
           INSTALL_MANPAGES INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE
           ENABLE_MULTITHREADING WITH_FORTIFY_SOURCE WITH_STACK_PROTECTOR)
add_subdirectory("${_tp}/flac" EXCLUDE_FROM_ALL)
# FLAC sends its DLL to objs/ on Windows; keep it beside the programs.
set_target_properties(FLAC PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# libwebm: mkvparser only, as an object library compiled into our demuxer.
papaya_off(ENABLE_WEBMTS ENABLE_WEBMINFO ENABLE_TESTS ENABLE_SAMPLE_PROGRAMS)
add_subdirectory("${_tp}/libwebm" EXCLUDE_FROM_ALL)

# miniaudio: one C file; its DLL mode is unsupported upstream, so it is always
# compiled into our audio library.
add_library(papaya_miniaudio OBJECT "${_tp}/miniaudio/miniaudio.c")
target_include_directories(papaya_miniaudio PUBLIC "${_tp}/miniaudio")
target_compile_definitions(papaya_miniaudio PUBLIC MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION)
# Link it as a target (not $<TARGET_OBJECTS>) so these come along: the BSDs
# keep pthreads and libm out of libc.
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(papaya_miniaudio PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})
endif()
if(APPLE)
    target_link_libraries(papaya_miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio"
                                                  "-framework AudioToolbox")
endif()

# minimp3 (MP3, CC0): a single header; its implementation goes in a file of
# its own, generated here, with float output for our decoder.
file(WRITE "${CMAKE_BINARY_DIR}/minimp3/minimp3.c"
     "#define MINIMP3_IMPLEMENTATION
#include \"minimp3.h\"
")
add_library(papaya_minimp3 STATIC "${CMAKE_BINARY_DIR}/minimp3/minimp3.c")
target_include_directories(papaya_minimp3 PUBLIC "${_tp}/minimp3")
target_compile_definitions(papaya_minimp3 PUBLIC MINIMP3_FLOAT_OUTPUT)
set_target_properties(papaya_minimp3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(papaya_minimp3 PRIVATE /W0)
else()
    target_compile_options(papaya_minimp3 PRIVATE -w)
endif()

# dav1d (AV1): our own CMake, since dav1d builds with Meson.
if(UNIX)
    find_package(Threads REQUIRED)
endif()
include(dav1d)

# libvpx (VP8 / VP9): our own CMake, since libvpx uses its own configure.
include(libvpx)

# libavc (H.264) and libhevc (HEVC): our own targets over their source lists.
include(ittiam)

# libxaac (AAC): our own targets over its source lists.
include(xaac)

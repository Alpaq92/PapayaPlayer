# libavc (H.264) and libhevc (HEVC) decoders, built from their own source lists
# (decoder/*.cmake, common/*.cmake) but with our targets and flags:
#  - per-file -m flags for the SIMD files instead of AVX2 on every file, so
#    the plain C stays runnable on any x86-64 CPU;
#  - one shared ithread layer (papaya's Win32 one on Windows), no libm there;
#  - plain C selectors on non-x86 CPUs until their ARM asm is wired up.
set(_itt_root "${PROJECT_SOURCE_DIR}/third_party")
set(_itt_shim "${PROJECT_SOURCE_DIR}/papaya/decode/sw/ittiam")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(_itt_x86 ON)
else()
    set(_itt_x86 OFF)
endif()

# Reads <lib>'s source lists; their include_directories() calls are undone.
function(_itt_sources root out)
    get_directory_property(_saved INCLUDE_DIRECTORIES)
    set(SYSTEM_PROCESSOR x86_64)
    set(AVC_ROOT "${root}")
    set(HEVC_ROOT "${root}")
    set(_srcs "")
    foreach(f ${ARGN})
        # Only the source lists: the decoder files go on to add_library().
        file(READ "${root}/${f}" _text)
        string(FIND "${_text}" "add_library(" _at)
        if(_at GREATER -1)
            string(SUBSTRING "${_text}" 0 ${_at} _text)
        endif()
        get_filename_component(_lib "${root}" NAME)
        string(MAKE_C_IDENTIFIER "${_lib}_${f}" _copy)
        file(WRITE "${CMAKE_BINARY_DIR}/ittiam/${_copy}.cmake" "${_text}")
        include("${CMAKE_BINARY_DIR}/ittiam/${_copy}.cmake")
    endforeach()
    set_directory_properties(PROPERTIES INCLUDE_DIRECTORIES "${_saved}")
    foreach(v LIBAVC_COMMON_SRCS LIBAVCDEC_SRCS LIBHEVC_COMMON_SRCS LIBHEVCDEC_SRCS)
        list(APPEND _srcs ${${v}})
    endforeach()
    list(REMOVE_DUPLICATES _srcs)
    if(NOT _itt_x86)
        list(FILTER _srcs EXCLUDE REGEX "/x86/")
    endif()
    # Every Ittiam library defines the same ithread_* functions; papaya_ithread
    # provides them once.
    list(FILTER _srcs EXCLUDE REGEX "/common/ithread\\.c$")
    set(${out} ${_srcs} PARENT_SCOPE)
endfunction()

# The threading layer, shared by both libraries and the same on every OS:
# ithread_papaya.c over papaya's own threads. It carries its own copy of
# thread.c so the decoders do not link back into papaya_core; in any link
# papaya_core's thread.c comes first, so the copy is never pulled in.
add_library(papaya_ithread STATIC "${_itt_shim}/ithread_papaya.c" "${PROJECT_SOURCE_DIR}/papaya/core/thread.c")
target_include_directories(papaya_ithread PRIVATE "${_itt_root}/libavc/common" "${PROJECT_SOURCE_DIR}/papaya/include"
                           "${CMAKE_BINARY_DIR}/papaya/include")
target_compile_definitions(papaya_ithread PRIVATE PAPAYA_STATIC_DEFINE)
if(UNIX)
    target_link_libraries(papaya_ithread PUBLIC Threads::Threads)
endif()
set_target_properties(papaya_ithread PROPERTIES POSITION_INDEPENDENT_CODE ON)

function(_itt_library name root)
    cmake_parse_arguments(A "" "" "SOURCES;INCLUDES" ${ARGN})
    add_library(${name} STATIC ${A_SOURCES})
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    target_include_directories(${name} PUBLIC "${root}/common" "${root}/decoder")
    target_include_directories(${name} PRIVATE ${A_INCLUDES})
    if(_itt_x86)
        target_include_directories(${name} PRIVATE "${root}/common/x86" "${root}/decoder/x86")
        target_compile_definitions(${name} PRIVATE X86 DEFAULT_ARCH=D_ARCH_X86_SSE42)
        foreach(src ${A_SOURCES})
            if(src MATCHES "_avx2")
                set_property(SOURCE ${src} APPEND PROPERTY COMPILE_OPTIONS -mavx2 -mfma -msse4.2)
            elseif(src MATCHES "/x86/")
                # Their "ssse3" files use SSE4.1 too; SSE4.2 is their own baseline.
                set_property(SOURCE ${src} APPEND PROPERTY COMPILE_OPTIONS -msse4.2)
            endif()
        endforeach()
    else()
        target_include_directories(${name} PRIVATE "${root}/common/arm")
        target_compile_definitions(${name} PRIVATE ARMV8 DEFAULT_ARCH=D_ARCH_NA)
    endif()
    if(MSVC)
        target_compile_options(${name} PRIVATE /W0)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${name} PRIVATE -w)
    endif()
    target_link_libraries(${name} PUBLIC papaya_ithread)
    if(UNIX)
        target_link_libraries(${name} PUBLIC m)
    endif()
endfunction()

_itt_sources("${_itt_root}/libavc" _avc_srcs common/common.cmake decoder/libavcdec.cmake)
if(NOT _itt_x86)
    list(APPEND _avc_srcs "${_itt_shim}/avc_generic_arch.c")
endif()
_itt_library(papaya_avcdec "${_itt_root}/libavc" SOURCES ${_avc_srcs}
             INCLUDES "${_itt_root}/libavc/common/mvc")

_itt_sources("${_itt_root}/libhevc" _hevc_srcs common/common.cmake decoder/libhevcdec.cmake)
if(NOT _itt_x86)
    list(APPEND _hevc_srcs "${_itt_shim}/hevc_generic_arch.c")
endif()
_itt_library(papaya_hevcdec "${_itt_root}/libhevc" SOURCES ${_hevc_srcs})
target_compile_definitions(papaya_hevcdec PRIVATE ENABLE_MAIN_REXT_PROFILE DISABLE_AVX2)

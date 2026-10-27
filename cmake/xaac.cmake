# libxaac's AAC decoder (LC, HE v1/v2, xHE/USAC, LD/ELD), built from its own
# source lists (common/common.cmake, decoder/libxaacdec.cmake) with our target:
#  - its "x86" function selector on every CPU: it only picks plain C
#    functions, while the generic one no longer matches the decoder's
#    prototypes; the ARM asm can come later, as for libavc/libhevc;
#  - the encoder (for the round-trip test) the same way, from
#    encoder/libxaacenc.cmake.
set(_xaac_root "${PROJECT_SOURCE_DIR}/third_party/libxaac")

# Reads the given source lists; their include_directories() calls are undone.
function(_xaac_sources out)
    get_directory_property(_saved INCLUDE_DIRECTORIES)
    set(XAAC_ROOT "${_xaac_root}")
    # Takes the x86 branch, whose files are C (see above).
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
    foreach(f ${ARGN})
        file(READ "${_xaac_root}/${f}" _text)
        string(FIND "${_text}" "add_library(" _at)
        if(_at GREATER -1)
            string(SUBSTRING "${_text}" 0 ${_at} _text)
        endif()
        string(MAKE_C_IDENTIFIER "${f}" _copy)
        file(WRITE "${CMAKE_BINARY_DIR}/xaac/${_copy}.cmake" "${_text}")
        include("${CMAKE_BINARY_DIR}/xaac/${_copy}.cmake")
    endforeach()
    set_directory_properties(PROPERTIES INCLUDE_DIRECTORIES "${_saved}")
    set(_srcs ${LIBXAACDEC_SRCS} ${LIBXAACENC_SRCS})
    list(REMOVE_DUPLICATES _srcs)
    set(${out} ${_srcs} PARENT_SCOPE)
    set(${out}_common ${LIBXAAC_COMMON_SRCS} PARENT_SCOPE)
endfunction()

function(_xaac_library name)
    cmake_parse_arguments(A "" "" "SOURCES;INCLUDES" ${ARGN})
    add_library(${name} STATIC ${A_SOURCES})
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    target_include_directories(${name} PUBLIC "${_xaac_root}/common" ${A_INCLUDES})
    target_compile_definitions(${name} PRIVATE LOUDNESS_LEVELING_SUPPORT)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W0)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${name} PRIVATE -w -fwrapv)
    endif()
    if(UNIX)
        target_link_libraries(${name} PUBLIC m)
    endif()
endfunction()

# The decoder and the encoder share common/; one library holds it, so the
# tests can link both.
_xaac_sources(_xaacdec_srcs common/common.cmake decoder/libxaacdec.cmake)
_xaac_library(papaya_xaaccommon SOURCES ${_xaacdec_srcs_common})
# Its plain C QMF functions are left out when the compiler defines
# __ARM_NEON__ (Apple's does), on the assumption that the ARM asm is linked.
if(NOT MSVC)
    set_property(SOURCE "${_xaac_root}/decoder/generic/ixheaacd_qmf_dec_generic.c"
                 APPEND PROPERTY COMPILE_OPTIONS -U__ARM_NEON__)
endif()
_xaac_library(papaya_xaacdec SOURCES ${_xaacdec_srcs}
              INCLUDES "${_xaac_root}/decoder" "${_xaac_root}/decoder/drc_src")
target_link_libraries(papaya_xaacdec PUBLIC papaya_xaaccommon)

if(PAPAYA_TESTS)
    _xaac_sources(_xaacenc_srcs common/common.cmake encoder/libxaacenc.cmake)
    _xaac_library(papaya_xaacenc SOURCES ${_xaacenc_srcs}
                  INCLUDES "${_xaac_root}/encoder" "${_xaac_root}/encoder/drc_src")
    target_link_libraries(papaya_xaacenc PUBLIC papaya_xaaccommon)
endif()

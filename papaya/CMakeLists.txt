configure_file(include/papaya/version.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/include/papaya/version.h" @ONLY)

# One library for the player logic; the third-party libraries stay separate.
add_library(papaya_core
    core/clock.c
    core/media.c
    core/queue.c
    core/thread.c
    core/version.c
    audio/audio_out.c
    io/io.c
    player/player.c
    demux/demux.c
    demux/ivf/ivf.c
    demux/mp4/mp4.c
    demux/wav/wav.c
    demux/mp3/mp3.c
    demux/adts/adts.c
    demux/webm/webm.cc
    decode/decode.c
    decode/sw/dav1d/dav1d.c
    decode/sw/flac/flac.c
    decode/sw/opus/opus.c
    decode/sw/pcm/pcm.c
    decode/sw/mp3/mp3.c
    decode/sw/vorbis/vorbis.c
    decode/sw/vpx/vpx.c
    decode/sw/ittiam/avc.c
    decode/sw/ittiam/hevc.c
    decode/sw/xaac/aac.c
    $<TARGET_OBJECTS:mkvparser>)
generate_export_header(papaya_core
    BASE_NAME papaya
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/papaya/export.h")
target_include_directories(papaya_core PUBLIC
    include
    "${CMAKE_CURRENT_BINARY_DIR}/include")
target_include_directories(papaya_core PRIVATE demux decode "${PROJECT_SOURCE_DIR}/third_party/libwebm")
target_link_libraries(papaya_core PRIVATE opus vorbis FLAC Ogg::ogg papaya_miniaudio dav1d vpx)
# The Ittiam headers clash with each other, so each backend sees only its own.
set_source_files_properties(decode/sw/ittiam/avc.c PROPERTIES INCLUDE_DIRECTORIES
    "${PROJECT_SOURCE_DIR}/third_party/libavc/common;${PROJECT_SOURCE_DIR}/third_party/libavc/decoder")
set_source_files_properties(decode/sw/ittiam/hevc.c PROPERTIES INCLUDE_DIRECTORIES
    "${PROJECT_SOURCE_DIR}/third_party/libhevc/common;${PROJECT_SOURCE_DIR}/third_party/libhevc/decoder")
target_link_libraries(papaya_core PRIVATE $<LINK_ONLY:papaya_avcdec> $<LINK_ONLY:papaya_hevcdec>)
set_source_files_properties(decode/sw/xaac/aac.c PROPERTIES INCLUDE_DIRECTORIES
    "${PROJECT_SOURCE_DIR}/third_party/libxaac/common;${PROJECT_SOURCE_DIR}/third_party/libxaac/decoder")
target_link_libraries(papaya_core PRIVATE $<LINK_ONLY:papaya_xaacdec> papaya_minimp3)
if(NOT PAPAYA_SHARED_LIBS)
    target_compile_definitions(papaya_core PUBLIC PAPAYA_STATIC_DEFINE)
endif()
set_target_properties(papaya_core PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
papaya_warnings(papaya_core)
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(papaya_core PRIVATE Threads::Threads)
endif()

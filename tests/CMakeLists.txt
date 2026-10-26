add_executable(core_test unit/core_test.c)
target_include_directories(core_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/greatest")
target_link_libraries(core_test PRIVATE papaya_core)
papaya_warnings(core_test)
add_test(NAME core_test COMMAND core_test)

if(PAPAYA_RUST)
    add_executable(rust_test unit/rust_test.c)
    target_include_directories(rust_test PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/greatest"
        "${PROJECT_SOURCE_DIR}/rust/papaya_hello/include")
    target_link_libraries(rust_test PRIVATE papaya_hello)
    papaya_warnings(rust_test)
    add_test(NAME rust_test COMMAND rust_test)
endif()

add_executable(deps_test unit/deps_test.c)
target_include_directories(deps_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/greatest")
target_link_libraries(deps_test PRIVATE Ogg::ogg vorbis opus FLAC papaya_miniaudio dav1d vpx)
papaya_warnings(deps_test)
add_test(NAME deps_test COMMAND deps_test)

add_executable(demux_test unit/demux_test.c)
target_include_directories(demux_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/greatest")
target_compile_definitions(demux_test PRIVATE
    PAPAYA_LIBWEBM_TESTDATA="${PROJECT_SOURCE_DIR}/third_party/libwebm/testing/testdata")
target_link_libraries(demux_test PRIVATE papaya_core)
papaya_warnings(demux_test)
add_test(NAME demux_test COMMAND demux_test)

add_executable(decode_test unit/decode_test.c)
target_include_directories(decode_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/greatest")
target_compile_definitions(decode_test PRIVATE
    PAPAYA_LIBWEBM_TESTDATA="${PROJECT_SOURCE_DIR}/third_party/libwebm/testing/testdata")
# The encoders are only for building test input.
target_link_libraries(decode_test PRIVATE papaya_core vorbisenc vorbis FLAC)
if(UNIX)
    target_link_libraries(decode_test PRIVATE m)
endif()
papaya_warnings(decode_test)
add_test(NAME decode_test COMMAND decode_test)

add_executable(audio_test unit/audio_test.c)
target_include_directories(audio_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/greatest")
target_link_libraries(audio_test PRIVATE papaya_core)
papaya_warnings(audio_test)
add_test(NAME audio_test COMMAND audio_test)

add_executable(video_test unit/video_test.c)
target_include_directories(video_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/greatest")
target_compile_definitions(video_test PRIVATE PAPAYA_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(video_test PRIVATE papaya_core)
papaya_warnings(video_test)
add_test(NAME video_test COMMAND video_test)

add_executable(player_test unit/player_test.c)
target_include_directories(player_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/greatest")
target_compile_definitions(player_test PRIVATE
    PAPAYA_LIBWEBM_TESTDATA="${PROJECT_SOURCE_DIR}/third_party/libwebm/testing/testdata")
target_link_libraries(player_test PRIVATE papaya_core)
papaya_warnings(player_test)
add_test(NAME player_test COMMAND player_test)

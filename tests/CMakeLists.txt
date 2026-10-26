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

add_executable(deps_test unit/deps_test.c $<TARGET_OBJECTS:papaya_miniaudio>)
target_include_directories(deps_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/greatest")
target_link_libraries(deps_test PRIVATE Ogg::ogg vorbis opus FLAC)
target_include_directories(deps_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/miniaudio")
target_compile_definitions(deps_test PRIVATE MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION)
papaya_warnings(deps_test)
add_test(NAME deps_test COMMAND deps_test)

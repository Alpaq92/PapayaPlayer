configure_file(include/papaya/version.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/include/papaya/version.h" @ONLY)

add_library(papaya_core
    core/version.c)
generate_export_header(papaya_core
    BASE_NAME papaya
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/papaya/export.h")
target_include_directories(papaya_core PUBLIC
    include
    "${CMAKE_CURRENT_BINARY_DIR}/include")
if(NOT PAPAYA_SHARED_LIBS)
    target_compile_definitions(papaya_core PUBLIC PAPAYA_STATIC_DEFINE)
endif()
set_target_properties(papaya_core PROPERTIES
    C_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
papaya_warnings(papaya_core)

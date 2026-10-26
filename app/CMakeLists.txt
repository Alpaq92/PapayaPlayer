reaktor_add_app(papayaplayer MODULES a11y locale text settings SOURCES main.c)
target_link_libraries(papayaplayer PRIVATE papaya_core)
# Ionicons from Reaktor's submodule until packaging ships the icons.
target_compile_definitions(papayaplayer PRIVATE
    PAPAYA_ICON_DIR="${PROJECT_SOURCE_DIR}/third_party/reaktor/external/ionicons/src/svg")
# reaktor_add_app compiles Reaktor's module sources in this target too, so our
# warnings go on our own files only.
papaya_warning_flags(_papaya_flags)
set_source_files_properties(main.c PROPERTIES COMPILE_OPTIONS "${_papaya_flags}")

add_executable(papaya-probe "${PROJECT_SOURCE_DIR}/tools/probe/probe.c")
target_link_libraries(papaya-probe PRIVATE papaya_core)
papaya_warnings(papaya-probe)

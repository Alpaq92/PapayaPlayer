# Injected with -DCMAKE_PROJECT_INCLUDE into the Ittiam libraries' own CMake
# (spike S2). At the end of their top directory it swaps the pthread
# common/ithread.c for papaya's Win32 version and links a program against the
# decoder, without touching their files.
#
# Historical: ithread_win32.c was replaced by ithread_papaya.c (one layer for
# every OS, over papaya's threads), which needs papaya's build. To rerun this
# spike, check out a revision from before that change.
set(PAPAYA_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL papaya_spike_fix)

function(papaya_spike_fix)
    foreach(pair libavcdec:ih264d_api_function libhevcdec:ihevcd_cxa_api_function
                 libmpeg2dec:impeg2d_api_function)
        string(REPLACE ":" ";" pair "${pair}")
        list(GET pair 0 lib)
        list(GET pair 1 entry)
        if(NOT TARGET ${lib})
            continue()
        endif()
        get_target_property(srcs ${lib} SOURCES)
        list(FILTER srcs EXCLUDE REGEX "common/ithread\.c$")
        list(APPEND srcs "${PAPAYA_ROOT}/papaya/decode/sw/ittiam/ithread_win32.c")
        set_property(TARGET ${lib} PROPERTY SOURCES ${srcs})
        # Their link_libraries(Threads::Threads m) asks for a libm Windows lacks.
        foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(libs ${lib} ${prop})
            if(libs)
                list(REMOVE_ITEM libs m "$<LINK_ONLY:m>")
                set_property(TARGET ${lib} PROPERTY ${prop} ${libs})
            endif()
        endforeach()
        add_executable(link_${lib} "${PAPAYA_ROOT}/spikes/ittiam-link.c")
        target_compile_definitions(link_${lib} PRIVATE PAPAYA_ENTRY=${entry})
        # Their link_libraries(Threads::Threads m) asks for a libm Windows lacks.
        set_property(TARGET link_${lib} PROPERTY LINK_LIBRARIES ${lib})
    endforeach()
endfunction()

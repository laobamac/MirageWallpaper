# Shared helper that defines a public C library plus its static twin and
# install/export rules. The public headers live in include/ and are installed
# verbatim, so the installed ABI is identical for every variant.
#
# mirage_add_library(<name>
#     SOURCES <c sources...>
#     HEADERS <public headers...>
#     LINKS_SHARED <link items for the shared library>
#     LINKS_STATIC <link items for the static library>
#     PRIVATE_INCLUDE <private include dirs>)
function(mirage_add_library target_name)
    set(options "")
    set(one_value_args "")
    set(multi_value_args SOURCES HEADERS LINKS_SHARED LINKS_STATIC PRIVATE_INCLUDE)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    add_library(${target_name} SHARED ${ARG_SOURCES})
    add_library(${target_name}_static STATIC ${ARG_SOURCES})
    set_target_properties(${target_name}_static PROPERTIES OUTPUT_NAME ${target_name})
    set_target_properties(${target_name} PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})

    foreach(target ${target_name} ${target_name}_static)
        # Library implementations are C++20, while installed headers remain C ABI only.
        target_compile_features(${target} PRIVATE cxx_std_20)
        set_target_properties(${target} PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            LINKER_LANGUAGE CXX)
        target_include_directories(${target}
            PUBLIC
                $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
            PRIVATE ${ARG_PRIVATE_INCLUDE})
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endforeach()
    # The shared twin links the shared dependency set; the static twin links
    # only static dependencies so it can be embedded in other binaries and
    # shared modules without a runtime dependency on the shared libraries.
    if(ARG_LINKS_SHARED)
        target_link_libraries(${target_name} PUBLIC ${ARG_LINKS_SHARED})
    endif()
    if(ARG_LINKS_STATIC)
        target_link_libraries(${target_name}_static PUBLIC ${ARG_LINKS_STATIC})
    endif()

    install(TARGETS ${target_name} ${target_name}_static
        EXPORT MirageDisplayTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    if(ARG_HEADERS)
        install(FILES ${ARG_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    endif()
endfunction()

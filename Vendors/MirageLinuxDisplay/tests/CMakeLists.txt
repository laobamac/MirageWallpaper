# Registers a test executable wired to the static library. TEST_NAME defaults
# to the executable name, which keeps the existing ctest names stable.
function(mirage_add_test name)
    set(options "")
    set(one_value_args TEST_NAME)
    set(multi_value_args SOURCES LINKS INCLUDES)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LINKS})
    if(ARG_INCLUDES)
        target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    endif()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(ARG_TEST_NAME)
        add_test(NAME ${ARG_TEST_NAME} COMMAND ${name})
    else()
        add_test(NAME ${name} COMMAND ${name})
    endif()
endfunction()

find_package(Threads REQUIRED)

mirage_add_test(test_codec SOURCES test_codec.c LINKS mirage_display_static
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
mirage_add_test(test_protocol SOURCES test_protocol.c LINKS mirage_display_static
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
mirage_add_test(test_session SOURCES test_session.c LINKS mirage_display_static Threads::Threads
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
mirage_add_test(test_broker TEST_NAME broker SOURCES test_broker.c
    LINKS mirage_display_static Threads::Threads
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
set_tests_properties(broker PROPERTIES SKIP_RETURN_CODE 77)
mirage_add_test(test_producer SOURCES test_producer.c LINKS mirage_display_static Threads::Threads
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
mirage_add_test(test_sync SOURCES test_sync.c LINKS mirage_display_static
    INCLUDES ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/common)
if(MIRAGE_DISPLAY_WITH_VULKAN)
    mirage_add_test(test_vulkan SOURCES test_vulkan.c LINKS mirage_display_vulkan_static)
endif()
if(MIRAGE_DISPLAY_WITH_EGL)
    mirage_add_test(test_egl SOURCES test_egl.c LINKS mirage_display_egl_static)
endif()

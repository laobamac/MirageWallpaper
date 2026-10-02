# Registers a test executable wired to the static library. TEST_NAME defaults
# to the executable name, which keeps the existing ctest names stable.
function(mirage_add_test name)
    set(options "")
    set(one_value_args TEST_NAME)
    set(multi_value_args SOURCES LINKS INCLUDES)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_compile_features(${name} PRIVATE cxx_std_20)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
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

mirage_add_test(test_codec SOURCES test_codec.cpp LINKS mirage_display_static
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
mirage_add_test(test_protocol SOURCES test_protocol.cpp LINKS mirage_display_static
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
mirage_add_test(test_session SOURCES test_session.cpp LINKS mirage_display_static Threads::Threads
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
mirage_add_test(test_broker TEST_NAME broker SOURCES test_broker.cpp
    LINKS mirage_display_static Threads::Threads
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
set_tests_properties(broker PROPERTIES SKIP_RETURN_CODE 77)
mirage_add_test(test_producer SOURCES test_producer.cpp LINKS mirage_display_static Threads::Threads
    INCLUDES ${PROJECT_SOURCE_DIR}/src)
mirage_add_test(test_sync SOURCES test_sync.cpp LINKS mirage_display_static
    INCLUDES ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/common)
if(MIRAGE_DISPLAY_WITH_VULKAN)
    mirage_add_test(test_vulkan SOURCES test_vulkan.cpp LINKS mirage_display_vulkan_static)

    set(_mirage_c_abi_vulkan_source "${CMAKE_CURRENT_BINARY_DIR}/c_abi_vulkan.c")
    file(WRITE "${_mirage_c_abi_vulkan_source}" [=[
#include <stdlib.h>
#include "mirage_display_vulkan.h"
#include "mirage_display_vulkan_blit.h"
#include "mirage_display_vulkan_export.h"

int main(void) {
    md_vk_importer_t* (*const importer_new)(const md_vk_context_t*) = md_vk_importer_new;
    md_vk_blitter_t* (*const blitter_new)(const md_vk_blit_context_t*) = md_vk_blitter_new;
    md_vk_exporter_t* (*const exporter_new)(const md_vk_export_context_t*) = md_vk_exporter_new;
    return importer_new != NULL && blitter_new != NULL && exporter_new != NULL
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
]=])
    add_executable(test_c_abi_vulkan "${_mirage_c_abi_vulkan_source}")
    target_link_libraries(test_c_abi_vulkan PRIVATE mirage_display_vulkan_static)
    set_target_properties(test_c_abi_vulkan PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_options(test_c_abi_vulkan PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME c_abi_vulkan COMMAND test_c_abi_vulkan)
endif()
if(MIRAGE_DISPLAY_WITH_EGL)
    mirage_add_test(test_egl SOURCES test_egl.cpp LINKS mirage_display_egl_static)

    set(_mirage_c_abi_egl_source "${CMAKE_CURRENT_BINARY_DIR}/c_abi_egl.c")
    file(WRITE "${_mirage_c_abi_egl_source}" [=[
#include <stdlib.h>
#include "mirage_display_egl.h"

int main(void) {
    md_egl_importer_t* (*const importer_new)(const md_egl_context_t*) = md_egl_importer_new;
    return importer_new != NULL ? EXIT_SUCCESS : EXIT_FAILURE;
}
]=])
    add_executable(test_c_abi_egl "${_mirage_c_abi_egl_source}")
    target_link_libraries(test_c_abi_egl PRIVATE mirage_display_egl_static)
    set_target_properties(test_c_abi_egl PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_options(test_c_abi_egl PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME c_abi_egl COMMAND test_c_abi_egl)
endif()

# This source exists only in the build tree so repository code remains C++ while
# every installed public declaration is continuously compiled by a C11 client.
set(_mirage_c_abi_core_source "${CMAKE_CURRENT_BINARY_DIR}/c_abi_core.c")
file(WRITE "${_mirage_c_abi_core_source}" [=[
#include <stdlib.h>
#include "mirage_display.h"
#include "mirage_display_broker.h"
#include "mirage_display_producer.h"

int main(void) {
    md_display_t* (*const display_new)(const md_display_callbacks_t*) = md_display_new;
    void (*const display_free)(md_display_t*) = md_display_free;
    md_broker_t* (*const broker_new)(const md_broker_options_t*) = md_broker_new;
    int32_t (*const broker_dispatch)(md_broker_t*, int32_t) = md_broker_dispatch;
    md_producer_t* (*const producer_new)(const md_producer_callbacks_t*) = md_producer_new;
    return display_new != NULL && display_free != NULL && broker_new != NULL &&
                   broker_dispatch != NULL && producer_new != NULL
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
]=])
add_executable(test_c_abi_core "${_mirage_c_abi_core_source}")
target_link_libraries(test_c_abi_core PRIVATE mirage_display_static)
set_target_properties(test_c_abi_core PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_c_abi_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME c_abi_core COMMAND test_c_abi_core)

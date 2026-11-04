cmake_minimum_required(VERSION 3.20)

project(mirage_display_qml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Qml Quick)

if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
endif()

set(MIRAGE_DISPLAY_QML_URI "Mirage.Display"
    CACHE STRING "QML module URI for the Mirage display item")
string(REPLACE "." "/" MIRAGE_DISPLAY_QML_PATH "${MIRAGE_DISPLAY_QML_URI}")
set(MIRAGE_DISPLAY_QML_OUTPUT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/${MIRAGE_DISPLAY_QML_PATH}")

qt_add_qml_module(mirage-display-qml
    URI ${MIRAGE_DISPLAY_QML_URI}
    VERSION 1.0
    PLUGIN_TARGET mirage-display-qml
    SOURCES
        MirageDisplayItem.hpp
        MirageDisplayItem.cpp
        MiragePointerForwarder.hpp
        MiragePointerForwarder.cpp
    OUTPUT_DIRECTORY
        ${MIRAGE_DISPLAY_QML_OUTPUT_DIR})

target_link_libraries(mirage-display-qml PRIVATE
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    mirage_display_egl_static)
if(TARGET mirage_display_vulkan_static)
    target_link_libraries(mirage-display-qml PRIVATE mirage_display_vulkan_static)
    target_compile_definitions(mirage-display-qml PRIVATE MIRAGE_DISPLAY_QML_WITH_VULKAN=1)
endif()
target_compile_options(mirage-display-qml PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
target_link_options(mirage-display-qml PRIVATE -Wl,--as-needed)

if(MIRAGE_DISPLAY_BUILD_TESTS)
    add_executable(test_qml_pointer
        tests/test_pointer.cpp
        MiragePointerForwarder.cpp)
    target_include_directories(test_qml_pointer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(test_qml_pointer PRIVATE Qt6::Gui)
    target_compile_options(test_qml_pointer PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_test(NAME qml_pointer COMMAND test_qml_pointer)
endif()

set(MIRAGE_DISPLAY_QML_INSTALL_DIR
    "${CMAKE_INSTALL_LIBDIR}/qt6/qml"
    CACHE PATH "Qt 6 QML module install directory")
install(DIRECTORY ${MIRAGE_DISPLAY_QML_OUTPUT_DIR}/
    DESTINATION ${MIRAGE_DISPLAY_QML_INSTALL_DIR}/${MIRAGE_DISPLAY_QML_PATH})

if(MIRAGE_DISPLAY_QML_URI MATCHES "Embed")
    install(DIRECTORY ${MIRAGE_DISPLAY_QML_OUTPUT_DIR}/
        DESTINATION org.mirage.wallpaper/contents/ui/MirageDisplayEmbed
        COMPONENT kde_extension
        EXCLUDE_FROM_ALL)
endif()

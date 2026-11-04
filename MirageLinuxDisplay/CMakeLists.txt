cmake_minimum_required(VERSION 3.20)

project(mirage_linux_display
    VERSION 0.2.0
    DESCRIPTION "Desktop-environment display integration for MirageWallpaper"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(cmake/MirageLibrary.cmake)

find_package(Vulkan QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(EGL QUIET IMPORTED_TARGET egl)
    pkg_check_modules(GLESV2 QUIET IMPORTED_TARGET glesv2)
endif()

set(_mirage_egl_default OFF)
if(EGL_FOUND AND GLESV2_FOUND)
    set(_mirage_egl_default ON)
endif()
option(MIRAGE_DISPLAY_WITH_VULKAN "Build the Vulkan DMA-BUF helper" ${Vulkan_FOUND})
option(MIRAGE_DISPLAY_WITH_EGL "Build the EGL DMA-BUF helper" ${_mirage_egl_default})
option(MIRAGE_DISPLAY_BUILD_TESTS "Build protocol and session tests" ON)
option(MIRAGE_DISPLAY_BUILD_EXAMPLES "Build protocol simulator examples" ON)
option(MIRAGE_DISPLAY_PLUGIN_QML "Build the Plasma Qt Quick display adapter" OFF)

if(MIRAGE_DISPLAY_WITH_VULKAN AND NOT Vulkan_FOUND)
    message(FATAL_ERROR "MIRAGE_DISPLAY_WITH_VULKAN=ON but Vulkan was not found")
endif()
if(MIRAGE_DISPLAY_WITH_EGL AND (NOT EGL_FOUND OR NOT GLESV2_FOUND))
    message(FATAL_ERROR "MIRAGE_DISPLAY_WITH_EGL=ON but EGL/GLESv2 were not found")
endif()
if(MIRAGE_DISPLAY_PLUGIN_QML AND NOT MIRAGE_DISPLAY_WITH_EGL)
    message(FATAL_ERROR "MIRAGE_DISPLAY_PLUGIN_QML requires MIRAGE_DISPLAY_WITH_EGL=ON")
endif()

add_subdirectory(src)
if(MIRAGE_DISPLAY_WITH_VULKAN)
    add_subdirectory(gpu/vulkan)
endif()
if(MIRAGE_DISPLAY_WITH_EGL)
    add_subdirectory(gpu/egl)
endif()
if(MIRAGE_DISPLAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(MIRAGE_DISPLAY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(MIRAGE_DISPLAY_PLUGIN_QML)
    add_subdirectory(adapters/kde)
endif()

install(EXPORT MirageDisplayTargets
    FILE MirageDisplayTargets.cmake
    NAMESPACE Mirage::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MirageDisplay)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "mirage-linux-display")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_PACKAGING_INSTALL_PREFIX "")
include(CPack)

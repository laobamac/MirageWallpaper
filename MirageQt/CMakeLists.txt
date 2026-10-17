cmake_minimum_required(VERSION 3.24)

project(MirageQt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Quick
    QuickControls2
    Network
    Concurrent
    Qml
    WebEngineCore
    WebEngineWidgets
    WebEngineQuick
    DBus)
qt_standard_project_setup(REQUIRES 6.8)

# Keep the application-owned QML module under Qt's canonical resource prefix.
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

# MirageLinuxDisplay is maintained as a top-level repository component.
set(MIRAGE_DISPLAY_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../MirageLinuxDisplay")
set(MIRAGE_DISPLAY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_PLUGIN_QML OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_WITH_VULKAN OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_WITH_EGL OFF CACHE BOOL "" FORCE)
add_subdirectory("${MIRAGE_DISPLAY_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/MirageLinuxDisplay")

# Embed Resources/resources.qrc (app icon) into the binary.
set(CMAKE_AUTORCC ON)

set(MIRAGEQT_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
# Package builds place private renderer executables and shared application data
# outside /usr/bin. These absolute paths are part of the installed package
# contract; development builds still retain the repository lookup paths below.
set(MIRAGEQT_RUNTIME_DIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}/miragewallpaper"
    CACHE PATH "Installed MirageWallpaper private runtime directory")
set(MIRAGEQT_DATA_DIR "${CMAKE_INSTALL_FULL_DATADIR}/miragewallpaper"
    CACHE PATH "Installed MirageWallpaper shared data directory")
set(MIRAGE_STEAM_WEB_API_KEY "$ENV{MIRAGE_STEAM_WEB_API_KEY}" CACHE STRING
    "Optional built-in Steam Web API key (32 hexadecimal characters)")
if(MIRAGE_STEAM_WEB_API_KEY AND NOT MIRAGE_STEAM_WEB_API_KEY MATCHES "^[A-Fa-f0-9]{32}$")
    message(FATAL_ERROR "MIRAGE_STEAM_WEB_API_KEY must contain exactly 32 hexadecimal characters")
endif()

add_subdirectory(Sources)
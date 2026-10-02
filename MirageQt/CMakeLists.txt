cmake_minimum_required(VERSION 3.24)

# FluentUI passes an intentionally empty PLUGIN_TARGET to its Qt 6.8 module.
# CMake 3.31 made empty single-value arguments observable; define the policy
# default before any nested project calls cmake_minimum_required(), so a clean
# configure uses the older parsing semantics without changing Vendor/FluentUI.
set(CMAKE_POLICY_DEFAULT_CMP0174 OLD CACHE STRING
    "Keep vendored FluentUI on its pre-CMake-3.31 argument parsing")
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 OLD)
endif()

project(MirageQt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
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
find_package(X11 REQUIRED)

# Fix the QML module resource/import policies before adding bundled QML
# subprojects.  This also keeps the vendored module configuration deterministic
# without modifying files under Vendor/FluentUI.
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

set(FLUENTUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLUENTUI_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
add_subdirectory(Vendor/FluentUI/src)

# MirageLinuxDisplay is maintained as a top-level repository component.
set(MIRAGE_DISPLAY_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../MirageLinuxDisplay")
set(MIRAGE_DISPLAY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_PLUGIN_QML OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_WITH_VULKAN OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_WITH_EGL OFF CACHE BOOL "" FORCE)
add_subdirectory("${MIRAGE_DISPLAY_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/MirageLinuxDisplay")

qt_standard_project_setup()
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

add_library(MirageQtCore STATIC
    Sources/Services/FavoritesManager.cpp
    Sources/Services/FavoritesManager.h
    Sources/Services/DisplayBrokerService.cpp
    Sources/Services/DisplayBrokerService.h
    Sources/Services/DisplayOutputModel.cpp
    Sources/Services/DisplayOutputModel.h
    Sources/Services/GlobalSettingsService.cpp
    Sources/Services/GlobalSettingsService.h
    Sources/Services/LinuxSystemIntegration.cpp
    Sources/Services/LinuxSystemIntegration.h
    Sources/Services/MirageController.cpp
    Sources/Services/MirageController.h
    Sources/Services/MirageControllerMapping.cpp
    Sources/Services/Paths.cpp
    Sources/Services/Paths.h
    Sources/Services/PlaybackController.cpp
    Sources/Services/PlaybackController.h
    Sources/Services/PlaylistModels.cpp
    Sources/Services/PlaylistModels.h
    Sources/Services/PlaylistManager.cpp
    Sources/Services/PlaylistManager.h
    Sources/Services/PlaylistRotator.cpp
    Sources/Services/PlaylistRotator.h
    Sources/Services/PlaylistTransitionOverlay.cpp
    Sources/Services/PlaylistTransitionOverlay.h
    Sources/Services/RendererController.cpp
    Sources/Services/RendererController.h
    Sources/Services/SecretService.cpp
    Sources/Services/SecretService.h
    Sources/Services/SteamServiceManager.cpp
    Sources/Services/SteamServiceManager.h
    Sources/Services/SteamSetupViewModel.cpp
    Sources/Services/SteamSetupViewModel.h
    Sources/Services/SteamWebAPI.cpp
    Sources/Services/SteamWebAPI.h
    Sources/Services/ThumbnailGenerator.cpp
    Sources/Services/ThumbnailGenerator.h
    Sources/Services/TrustedWallpaperService.cpp
    Sources/Services/TrustedWallpaperService.h
    Sources/Services/WallpaperLibrary.cpp
    Sources/Services/WallpaperLibrary.h
    Sources/Services/WallpaperRuntimeStore.cpp
    Sources/Services/WallpaperRuntimeStore.h
    Sources/Services/WEProject.cpp
    Sources/Services/WEProject.h
    Sources/Services/WorkshopModels.cpp
    Sources/Services/WorkshopModels.h
    Sources/Services/WorkshopViewModel.cpp
    Sources/Services/WorkshopViewModel.h)

target_include_directories(MirageQtCore PUBLIC Sources)
target_include_directories(MirageQtCore PRIVATE "${MIRAGE_DISPLAY_SOURCE_DIR}/include")
target_compile_definitions(MirageQtCore PUBLIC
    MIRAGEQT_REPO_ROOT="${MIRAGEQT_REPO_ROOT}"
    MIRAGEQT_RUNTIME_DIR="${MIRAGEQT_RUNTIME_DIR}"
    MIRAGEQT_DATA_DIR="${MIRAGEQT_DATA_DIR}"
    MIRAGE_STEAM_WEB_API_KEY="${MIRAGE_STEAM_WEB_API_KEY}")
target_link_libraries(MirageQtCore PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::Qml
    Qt6::DBus
    mirage_display_static
    X11::X11)

if (UNIX AND NOT APPLE)
    find_library(UTIL_LIBRARY util)
    if (UTIL_LIBRARY)
        target_link_libraries(MirageQtCore PUBLIC "${UTIL_LIBRARY}")
    endif()
endif()

qt_add_executable(MirageQt
    Sources/App/main.cpp
    Sources/App/StatusBar.cpp
    Sources/App/StatusBar.h
    Resources/resources.qrc)

set_source_files_properties(Sources/Qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
set_source_files_properties(Sources/Qml/PropertyLocalization.js PROPERTIES QT_RESOURCE_ALIAS PropertyLocalization.js)
# These scripts are imported by explicit relative paths.  They are not QML
# component types, so keeping them out of the generated qmldir prevents Qt
# from treating each as an implicitly imported JavaScript singleton.
set_source_files_properties(
    Sources/Qml/ContentView/ContentViewLogic.js
    Sources/Qml/ContentView/OptionData.js
    PROPERTIES QT_QML_SKIP_QMLDIR_ENTRY TRUE)
file(GLOB_RECURSE MIRAGE_QML_FILES CONFIGURE_DEPENDS
    Sources/Qml/*.qml
    Sources/Qml/*.js)
foreach(qml_file IN LISTS MIRAGE_QML_FILES)
    file(RELATIVE_PATH qml_alias "${CMAKE_CURRENT_LIST_DIR}/Sources/Qml" "${qml_file}")
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_alias}")
endforeach()
qt_add_qml_module(MirageQt
    URI MirageQt
    VERSION 1.0
    QML_FILES
        ${MIRAGE_QML_FILES})

target_link_libraries(MirageQt PRIVATE
    MirageQtCore
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebEngineWidgets
    Qt6::WebEngineQuick)

target_include_directories(MirageQt PRIVATE Vendor/FluentUI/src)
target_link_libraries(MirageQt PRIVATE fluentuiplugin fluentuipluginplugin)

option(MIRAGEQT_BUILD_TESTS "Build MirageQt tests" OFF)
if(MIRAGEQT_BUILD_TESTS)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    enable_testing()
    add_executable(MirageQtGlobalSettingsTests
        Tests/GlobalSettingsServiceTests.cpp)
    target_link_libraries(MirageQtGlobalSettingsTests PRIVATE
        MirageQtCore
        Qt6::Test)
    add_test(NAME MirageQtGlobalSettings COMMAND MirageQtGlobalSettingsTests)
    add_executable(MirageQtPlaylistManagerPersistenceTests
        Tests/PlaylistManagerPersistenceTests.cpp)
    target_link_libraries(MirageQtPlaylistManagerPersistenceTests PRIVATE
        MirageQtCore
        Qt6::Test)
    add_test(NAME MirageQtPlaylistManagerPersistence
        COMMAND MirageQtPlaylistManagerPersistenceTests)
    set_tests_properties(MirageQtPlaylistManagerPersistence PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(MirageQtWallpaperRuntimeStoreTests
        Tests/WallpaperRuntimeStoreTests.cpp)
    target_link_libraries(MirageQtWallpaperRuntimeStoreTests PRIVATE
        MirageQtCore
        Qt6::Test)
    add_test(NAME MirageQtWallpaperRuntimeStore COMMAND MirageQtWallpaperRuntimeStoreTests)
endif()

set_target_properties(MirageQt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS MirageQt
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- Desktop integration ---
# Desktop environments resolve the installed launcher through PATH, so the
# desktop entry intentionally names the binary instead of an absolute path.
set(MIRAGEQT_EXECUTABLE "MirageQt")
configure_file(Resources/mirageqt.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/mirageqt.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mirageqt.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES Resources/mirage.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME mirageqt.png)

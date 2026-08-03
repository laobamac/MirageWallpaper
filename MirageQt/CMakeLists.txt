cmake_minimum_required(VERSION 3.24)

project(MirageQt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Qml
    WebEngineCore
    WebEngineWidgets
    DBus)
find_package(X11 REQUIRED)

# MirageLinuxDisplay is vendored in-tree (see Vendors/MirageLinuxDisplay/README.md).
set(MIRAGE_DISPLAY_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../Vendors/MirageLinuxDisplay")
set(MIRAGE_DISPLAY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_PLUGIN_QML OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_WITH_VULKAN OFF CACHE BOOL "" FORCE)
set(MIRAGE_DISPLAY_WITH_EGL OFF CACHE BOOL "" FORCE)
add_subdirectory("${MIRAGE_DISPLAY_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/MirageLinuxDisplay")

qt_standard_project_setup()

set(MIRAGEQT_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
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
    Sources/Services/GlobalSettingsService.cpp
    Sources/Services/GlobalSettingsService.h
    Sources/Services/LinuxSystemIntegration.cpp
    Sources/Services/LinuxSystemIntegration.h
    Sources/Services/Paths.cpp
    Sources/Services/Paths.h
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
    Sources/Services/SteamCMDManager.cpp
    Sources/Services/SteamCMDManager.h
    Sources/Services/SteamWebAPI.cpp
    Sources/Services/SteamWebAPI.h
    Sources/Services/ThumbnailGenerator.cpp
    Sources/Services/ThumbnailGenerator.h
    Sources/Services/WallpaperLibrary.cpp
    Sources/Services/WallpaperLibrary.h
    Sources/Services/WallpaperRuntimeStore.cpp
    Sources/Services/WallpaperRuntimeStore.h
    Sources/Services/WEConditionEvaluator.cpp
    Sources/Services/WEConditionEvaluator.h
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
    MIRAGE_STEAM_WEB_API_KEY="${MIRAGE_STEAM_WEB_API_KEY}")
target_link_libraries(MirageQtCore PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Qml
    Qt6::DBus
    mirage_display_static
    X11::X11)

qt_add_executable(MirageQt
    Sources/App/main.cpp
    Sources/App/MirageStyle.cpp
    Sources/App/MirageStyle.h
    Sources/ContentView/ContentView.cpp
    Sources/ContentView/ContentView.h
    Sources/ContentView/FirstLaunchView.cpp
    Sources/ContentView/FirstLaunchView.h
    Sources/ContentView/ImportPanels.cpp
    Sources/ContentView/ImportPanels.h
    Sources/ContentView/MainWindow.cpp
    Sources/ContentView/MainWindow.h
    Sources/ContentView/Components/ExplorerBottomBarWidget.cpp
    Sources/ContentView/Components/ExplorerBottomBarWidget.h
    Sources/ContentView/Components/Playlist/PlaylistOpenDialog.cpp
    Sources/ContentView/Components/Playlist/PlaylistOpenDialog.h
    Sources/ContentView/Components/Playlist/PlaylistSaveDialog.cpp
    Sources/ContentView/Components/Playlist/PlaylistSaveDialog.h
    Sources/ContentView/Components/Playlist/PlaylistSettingsDialog.cpp
    Sources/ContentView/Components/Playlist/PlaylistSettingsDialog.h
    Sources/ContentView/Components/ExplorerTopBarWidget.cpp
    Sources/ContentView/Components/ExplorerTopBarWidget.h
    Sources/ContentView/Components/FilterResultsWidget.cpp
    Sources/ContentView/Components/FilterResultsWidget.h
    Sources/ContentView/Components/PropertyEditorWidget.cpp
    Sources/ContentView/Components/PropertyEditorWidget.h
    Sources/ContentView/Components/ProjectFeedbackBannerWidget.cpp
    Sources/ContentView/Components/ProjectFeedbackBannerWidget.h
    Sources/ContentView/Components/TopTabBarWidget.cpp
    Sources/ContentView/Components/TopTabBarWidget.h
    Sources/ContentView/Components/WallpaperListWidget.cpp
    Sources/ContentView/Components/WallpaperListWidget.h
    Sources/ContentView/Components/WallpaperPreviewWidget.cpp
    Sources/ContentView/Components/WallpaperPreviewWidget.h
    Sources/ContentView/Components/Discover/DiscoverBannerView.cpp
    Sources/ContentView/Components/Discover/DiscoverBannerView.h
    Sources/ContentView/Components/Discover/DiscoverSectionView.cpp
    Sources/ContentView/Components/Discover/DiscoverSectionView.h
    Sources/ContentView/Components/Discover/DiscoverView.cpp
    Sources/ContentView/Components/Discover/DiscoverView.h
    Sources/ContentView/Components/Workshop/DownloadPopover.cpp
    Sources/ContentView/Components/Workshop/DownloadPopover.h
    Sources/ContentView/Components/Workshop/WorkshopFilterSidebar.cpp
    Sources/ContentView/Components/Workshop/WorkshopFilterSidebar.h
    Sources/ContentView/Components/Workshop/WorkshopItemCard.cpp
    Sources/ContentView/Components/Workshop/WorkshopItemCard.h
    Sources/ContentView/Components/Workshop/WorkshopItemDetail.cpp
    Sources/ContentView/Components/Workshop/WorkshopItemDetail.h
    Sources/ContentView/Components/Workshop/WorkshopSearchBar.cpp
    Sources/ContentView/Components/Workshop/WorkshopSearchBar.h
    Sources/ContentView/Components/Workshop/WorkshopTagBar.cpp
    Sources/ContentView/Components/Workshop/WorkshopTagBar.h
    Sources/ContentView/Components/Workshop/WorkshopView.cpp
    Sources/ContentView/Components/Workshop/WorkshopView.h
    Sources/SettingsView/AboutUsView.cpp
    Sources/SettingsView/AboutUsView.h
    Sources/SettingsView/GeneralPage.cpp
    Sources/SettingsView/GeneralPage.h
    Sources/SettingsView/PerformancePage.cpp
    Sources/SettingsView/PerformancePage.h
    Sources/SettingsView/PluginsPage.cpp
    Sources/SettingsView/PluginsPage.h
    Sources/SettingsView/ScreenSaverPage.cpp
    Sources/SettingsView/ScreenSaverPage.h
    Sources/SettingsView/SettingsView.cpp
    Sources/SettingsView/SettingsView.h
    Sources/SteamSetup/SteamSetupDialog.cpp
    Sources/SteamSetup/SteamSetupDialog.h)

target_link_libraries(MirageQt PRIVATE
    MirageQtCore
    Qt6::Widgets
    Qt6::WebEngineWidgets)

set_target_properties(MirageQt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

install(TARGETS MirageQt
    RUNTIME DESTINATION bin)

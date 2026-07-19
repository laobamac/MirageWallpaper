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
    WebEngineCore
    WebEngineWidgets
    Multimedia
    MultimediaWidgets
    DBus)
find_package(X11 REQUIRED)

qt_standard_project_setup()

set(MIRAGEQT_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_library(MirageQtCore STATIC
    Sources/Services/FavoritesManager.cpp
    Sources/Services/FavoritesManager.h
    Sources/Services/GlobalSettingsService.cpp
    Sources/Services/GlobalSettingsService.h
    Sources/Services/LinuxSystemIntegration.cpp
    Sources/Services/LinuxSystemIntegration.h
    Sources/Services/Paths.cpp
    Sources/Services/Paths.h
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
    Sources/Services/WEProject.cpp
    Sources/Services/WEProject.h)

target_include_directories(MirageQtCore PUBLIC Sources)
target_compile_definitions(MirageQtCore PUBLIC
    MIRAGEQT_REPO_ROOT="${MIRAGEQT_REPO_ROOT}")
target_link_libraries(MirageQtCore PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::DBus
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
    Sources/ContentView/Components/WorkshopWidget.cpp
    Sources/ContentView/Components/WorkshopWidget.h
    Sources/SettingsView/SettingsWidget.cpp
    Sources/SettingsView/SettingsWidget.h
    Sources/SteamSetup/SteamSetupDialog.cpp
    Sources/SteamSetup/SteamSetupDialog.h)

target_link_libraries(MirageQt PRIVATE
    MirageQtCore
    Qt6::Widgets
    Qt6::WebEngineWidgets)

qt_add_executable(WebWallpaperQt
    Sources/Renderers/Common/ControlReader.cpp
    Sources/Renderers/Common/ControlReader.h
    Sources/Renderers/Common/DesktopWindowX11.cpp
    Sources/Renderers/Common/DesktopWindowX11.h
    Sources/Renderers/WebWallpaperQt/WebWallpaperMain.cpp
    Sources/Renderers/WebWallpaperQt/WebWallpaperWindow.cpp
    Sources/Renderers/WebWallpaperQt/WebWallpaperWindow.h
    Sources/Renderers/WebWallpaperQt/WEUrlSchemeHandler.cpp
    Sources/Renderers/WebWallpaperQt/WEUrlSchemeHandler.h)

target_link_libraries(WebWallpaperQt PRIVATE
    MirageQtCore
    Qt6::Widgets
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    X11::X11)

qt_add_executable(VideoWallpaperQt
    Sources/Renderers/Common/ControlReader.cpp
    Sources/Renderers/Common/ControlReader.h
    Sources/Renderers/Common/DesktopWindowX11.cpp
    Sources/Renderers/Common/DesktopWindowX11.h
    Sources/Renderers/VideoWallpaperQt/VideoWallpaperMain.cpp
    Sources/Renderers/VideoWallpaperQt/VideoWallpaperWindow.cpp
    Sources/Renderers/VideoWallpaperQt/VideoWallpaperWindow.h)

target_link_libraries(VideoWallpaperQt PRIVATE
    MirageQtCore
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    X11::X11)

foreach(target MirageQt WebWallpaperQt VideoWallpaperQt)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

install(TARGETS MirageQt WebWallpaperQt VideoWallpaperQt
    RUNTIME DESTINATION bin)

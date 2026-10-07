# Copyright © 2026 王孝慈. All rights reserved.
cmake_minimum_required(VERSION 3.24)
project(MirageLinuxUI VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Quick QuickControls2)
qt_add_executable(Mirage Sources/main.cpp Sources/UiBridge.cpp Sources/UiBridge.h)
file(GLOB_RECURSE UI_FILES CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" Qml/*.qml Qml/*.js Qml/qmldir Resources/*)
qt_add_resources(Mirage interface PREFIX "/" FILES ${UI_FILES})
set(MAC_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../Mirage/Mirage Wallpaper/Resources")
qt_add_resources(Mirage translations PREFIX "/translations" BASE "${MAC_RESOURCES}" FILES
    "${MAC_RESOURCES}/en.lproj/Localizable.strings"
    "${MAC_RESOURCES}/zh-Hans.lproj/Localizable.strings"
    "${MAC_RESOURCES}/zh-Hant.lproj/Localizable.strings"
    "${MAC_RESOURCES}/ui_zh-chs.json"
    "${MAC_RESOURCES}/ui_zh-cht.json")
target_link_libraries(Mirage PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::QuickControls2)
target_compile_options(Mirage PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS Mirage RUNTIME DESTINATION bin)
install(FILES Resources/mirage.desktop DESTINATION share/applications)

set_source_files_properties("${MAC_RESOURCES}/Assets.xcassets/AppIcon.appiconset/icon_1024.png" PROPERTIES QT_RESOURCE_ALIAS "AppIcon.png")
qt_add_resources(Mirage artwork PREFIX "/art" BASE "${MAC_RESOURCES}/Sponsorship" FILES
    "${MAC_RESOURCES}/Assets.xcassets/AppIcon.appiconset/icon_1024.png"
    "${MAC_RESOURCES}/Sponsorship/afdian.jpg"
    "${MAC_RESOURCES}/Sponsorship/alipay.jpg"
    "${MAC_RESOURCES}/Sponsorship/wechat-pay.png")

install(FILES "${MAC_RESOURCES}/Assets.xcassets/AppIcon.appiconset/icon_256.png" DESTINATION share/icons/hicolor/256x256/apps RENAME mirage.png)

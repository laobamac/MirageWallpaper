#遍历所有Cpp文件
file(GLOB_RECURSE CPP_FILES *.cpp)
foreach (filepath ${CPP_FILES})
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" filename ${filepath})
    list(APPEND sources_files ${filename})
endforeach (filepath)

#遍历所有h文件
file(GLOB_RECURSE H_FILES *.h)
foreach (filepath ${H_FILES})
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" filename ${filepath})
    list(APPEND sources_files ${filename})
endforeach (filepath)

#遍历所有qml文件
set(SINGLETON_FILES "Theme.qml" "FilterData.qml" "Store.qml")
file(GLOB_RECURSE QML_PATHS Qml/*.qml qmldir)
foreach (filepath ${QML_PATHS})
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" filename ${filepath})
    if (${filename} MATCHES "qmldir")
        list(APPEND resource_files ${filename})
    else ()
        list(APPEND qml_files ${filename})
        # Check for singleton
        foreach (singletion_name ${SINGLETON_FILES})
            if (${filename} MATCHES ${singletion_name})
                set_source_files_properties(${filename} PROPERTIES QT_QML_SINGLETON_TYPE true)
            endif ()
        endforeach (singletion_name)
    endif ()
endforeach (filepath)

#遍历所有资源文件
set(RES_SUFFIX "")
set(RES_PATHS "")
list(APPEND RES_SUFFIX *.png *.jpg *.svg *.ico *.ttf *.webp *.js *.qm *.qsb *.mp4)
list(APPEND RES_PATHS Resources)
foreach (respath ${RES_PATHS})
    foreach (filesuffix ${RES_SUFFIX})
        file(GLOB_RECURSE RES_FOUND ${respath}/${filesuffix})
        foreach (filepath ${RES_FOUND})
            string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" filename ${filepath})
            list(APPEND resource_files ${filename})
        endforeach()
    endforeach ()
endforeach (respath)

# Store.qml imports this sibling script from the QML module resource namespace.
list(APPEND resource_files "Qml/ConditionEvaluator.js")

qt_add_executable(MirageQt)

qt_add_qml_module(MirageQt
        OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/temp/MirageQt
        URI MirageQt
        TYPEINFO "plugins.qmltypes"
        SOURCES ${sources_files}
        QML_FILES ${qml_files}
        RESOURCES ${resource_files}
        RESOURCE_PREFIX "/"
)

# Keep QML localization assets owned by MirageQt so its resource bundle does not
# depend on sibling application directories at configure or build time.
qt_add_resources(MirageQt mirageqt_translations
        PREFIX "/MirageQt/Resources"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/Resources"
        FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/en.lproj/Localizable.strings"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/zh-Hans.lproj/Localizable.strings"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/zh-Hant.lproj/Localizable.strings"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/ui_zh-chs.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/ui_zh-cht.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Translations.json")

# For MirageLinuxDisplay
target_include_directories(MirageQt PRIVATE "${MIRAGE_DISPLAY_SOURCE_DIR}/include")

target_link_libraries(MirageQt PRIVATE
        Qt6::Widgets
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::WebEngineWidgets
        Qt6::WebEngineQuick
        Qt6::Gui
        Qt6::Network
        Qt6::Concurrent
        Qt6::Qml
        Qt6::DBus
        mirage_display_static)

target_compile_definitions(MirageQt PUBLIC
        MIRAGEQT_REPO_ROOT="${MIRAGEQT_REPO_ROOT}"
        MIRAGEQT_RUNTIME_DIR="${MIRAGEQT_RUNTIME_DIR}"
        MIRAGEQT_DATA_DIR="${MIRAGEQT_DATA_DIR}"
        MIRAGE_STEAM_WEB_API_KEY="${MIRAGE_STEAM_WEB_API_KEY}")

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

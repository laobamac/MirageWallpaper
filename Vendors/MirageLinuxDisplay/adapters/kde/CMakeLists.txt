add_subdirectory(qml)

install(DIRECTORY wallpaper/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/plasma/wallpapers/org.mirage.wallpaper)

if(MIRAGE_DISPLAY_QML_URI MATCHES "Embed")
    install(DIRECTORY wallpaper/
        DESTINATION org.mirage.wallpaper
        COMPONENT kde_extension
        EXCLUDE_FROM_ALL
        PATTERN "MirageSurface.qml" EXCLUDE
        PATTERN "MirageSurfaceEmbed.qml" EXCLUDE)
    install(FILES wallpaper/contents/ui/MirageSurfaceEmbed.qml
        DESTINATION org.mirage.wallpaper/contents/ui
        RENAME MirageSurface.qml
        COMPONENT kde_extension
        EXCLUDE_FROM_ALL)
    list(APPEND CPACK_COMPONENTS_ALL kde_extension)
    set(CPACK_ARCHIVE_KDE_EXTENSION_FILE_NAME
        "mirage-kde-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}-embed")
endif()

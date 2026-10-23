add_subdirectory(qml)

# ---------------------------------------------------------------------------
# Installable Plasma/Wallpaper kpackage archive
#
# Produces a ZIP that can be installed directly with
#     kpackagetool6 -t Plasma/Wallpaper -i mirage-wallpaper-<version>.zip
# The archive has the KPackage layout: metadata.json at the archive root plus
# a contents/ tree (ui/main.qml is the Plasma/Wallpaper mainscript). This is
# a different artifact from the CPack library distribution
# (mirage-linux-display-<version>-Linux.zip), which kpackagetool6 cannot
# consume.
#
# The kpackage is self-contained: the native QML module is copied into
# contents/ui/MirageDisplayEmbed and the surface component imports it with a
# relative import, so installing the ZIP with kpackagetool6 needs no
# system-wide QML module or library install.
# ---------------------------------------------------------------------------
set(_wallpaper_pkg_name "org.mirage.wallpaper")
set(_wallpaper_pkg_stage
    "${CMAKE_CURRENT_BINARY_DIR}/wallpaper-pkg/${_wallpaper_pkg_name}")
set(_wallpaper_pkg_archive
    "${CMAKE_CURRENT_BINARY_DIR}/mirage-wallpaper-${PROJECT_VERSION}.zip")
string(REPLACE "." "/" _wallpaper_qml_uri_path "${MIRAGE_DISPLAY_QML_URI}")
set(_wallpaper_pkg_qml_module
    "${CMAKE_CURRENT_BINARY_DIR}/qml/${_wallpaper_qml_uri_path}")

set(_wallpaper_pkg_script
    "${CMAKE_CURRENT_BINARY_DIR}/package-wallpaper.cmake")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/package-wallpaper.cmake.in"
    "${_wallpaper_pkg_script}"
    @ONLY)

file(GLOB _wallpaper_pkg_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/wallpaper/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/wallpaper/contents/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/wallpaper/contents/config/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/wallpaper/contents/ui/*")

add_custom_command(
    OUTPUT "${_wallpaper_pkg_archive}"
    COMMAND "${CMAKE_COMMAND}" -P "${_wallpaper_pkg_script}"
    DEPENDS
        "${_wallpaper_pkg_script}"
        ${_wallpaper_pkg_files}
        "$<TARGET_FILE:mirage-display-qml>"
    COMMENT "Creating self-contained Plasma wallpaper package ${_wallpaper_pkg_archive}")

add_custom_target(mirage-wallpaper-package ALL
    DEPENDS "${_wallpaper_pkg_archive}")

# Keep the staged layout available to the normal project install rules. The
# package target above remains independent of those rules.
install(DIRECTORY "${_wallpaper_pkg_stage}/"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/plasma/wallpapers/org.mirage.wallpaper)

if(MIRAGE_DISPLAY_QML_URI MATCHES "Embed")
    list(APPEND CPACK_COMPONENTS_ALL kde_extension)
    set(CPACK_ARCHIVE_KDE_EXTENSION_FILE_NAME
        "mirage-wallpaper-${PROJECT_VERSION}")
endif()

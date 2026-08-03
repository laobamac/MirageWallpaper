mirage_add_library(mirage_display_egl
    SOURCES egl_import.c
    HEADERS ${PROJECT_SOURCE_DIR}/include/mirage_display_egl.h
    LINKS_SHARED mirage_display PkgConfig::EGL PkgConfig::GLESV2
    LINKS_STATIC mirage_display_static PkgConfig::EGL PkgConfig::GLESV2
    PRIVATE_INCLUDE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/common)

mirage_add_library(mirage_display_vulkan
    SOURCES
        vulkan_import.c
        vulkan_export.c
        vulkan_blit.c
        vulkan_util.c
    HEADERS
        ${PROJECT_SOURCE_DIR}/include/mirage_display_vulkan.h
        ${PROJECT_SOURCE_DIR}/include/mirage_display_vulkan_blit.h
        ${PROJECT_SOURCE_DIR}/include/mirage_display_vulkan_export.h
    LINKS_SHARED mirage_display Vulkan::Vulkan
    LINKS_STATIC mirage_display_static Vulkan::Vulkan
    PRIVATE_INCLUDE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/common)

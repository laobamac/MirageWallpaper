# WebWallpaper — desktop wallpaper host.

if(APPLE)
    add_executable(WebWallpaper WebWallpaper.mm ControlChannel.m)
else()
    add_executable(WebWallpaper Linux/WebWallpaper.cpp Linux/ControlChannel.cpp
        Linux/ProtocolWebRenderer.h Linux/ProtocolWebRenderer.cpp)
endif()

target_link_libraries(WebWallpaper PRIVATE
    WebRendererEngine
    ${WR_PLATFORM_LIBS})

target_compile_features(WebWallpaper PRIVATE cxx_std_20)
if(APPLE)
    set_target_properties(WebWallpaper PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_compile_options(WebWallpaper PRIVATE -Wno-deprecated-declarations)
else()
    target_link_libraries(WebWallpaper PRIVATE mirage_display_vulkan_static Qt6::Core Qt6::Gui
        Qt6::Widgets Qt6::WebEngineCore Qt6::WebEngineWidgets Threads::Threads)
    # Vulkan's C ABI requires setting only sType before each structure is filled
    # member-by-member; GCC otherwise reports each intentionally zeroed member.
    target_compile_options(WebWallpaper PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -Wno-missing-field-initializers)
endif()

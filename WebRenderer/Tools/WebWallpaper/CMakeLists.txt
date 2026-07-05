# WebWallpaper — desktop wallpaper host.

add_executable(WebWallpaper WebWallpaper.mm ControlChannel.m)

target_link_libraries(WebWallpaper PRIVATE
    WebRendererEngine
    ${WR_FRAMEWORKS})

target_compile_features(WebWallpaper PRIVATE cxx_std_20)
set_target_properties(WebWallpaper PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_compile_options(WebWallpaper PRIVATE -Wno-deprecated-declarations)

# WebWallpaper — desktop wallpaper host.

if(WIN32)
    add_executable(WebWallpaper Windows/WebWallpaper.cpp Windows/ControlChannel.h Windows/ControlChannel.cpp
        Windows/WRDesktopHost.h Windows/WRDesktopHost.cpp)
    target_link_libraries(WebWallpaper PRIVATE WebRendererEngine user32)
    target_include_directories(WebWallpaper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../Platform/Windows")
    target_compile_definitions(WebWallpaper PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(WebWallpaper PRIVATE /W4 /utf-8)
    endif()
    install(TARGETS WebWallpaper RUNTIME DESTINATION .)
    return()
endif()

add_executable(WebWallpaper WebWallpaper.mm ControlChannel.m)

target_link_libraries(WebWallpaper PRIVATE
    WebRendererEngine
    ${WR_FRAMEWORKS})

target_compile_features(WebWallpaper PRIVATE cxx_std_20)
set_target_properties(WebWallpaper PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_compile_options(WebWallpaper PRIVATE -Wno-deprecated-declarations)

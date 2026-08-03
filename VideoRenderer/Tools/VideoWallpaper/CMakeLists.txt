if(APPLE)
    add_executable(VideoWallpaper macOS/VideoWallpaper.mm macOS/ControlChannel.m)
    target_link_libraries(VideoWallpaper PRIVATE VideoRendererEngine ${VR_FRAMEWORKS})
    target_compile_features(VideoWallpaper PRIVATE cxx_std_20)
    set_target_properties(VideoWallpaper PROPERTIES
        OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED ON)
    target_compile_options(VideoWallpaper PRIVATE -Wno-deprecated-declarations)
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MIRAGE_FFMPEG REQUIRED IMPORTED_TARGET
        libavformat>=58 libavcodec>=58 libavutil>=56 libswscale>=5 libswresample>=3)
    pkg_check_modules(MIRAGE_PULSE REQUIRED IMPORTED_TARGET libpulse-simple>=1.0)

    # Linux displays through the mirage-display protocol only (no X11 host).
    add_executable(VideoWallpaper
        Linux/VideoWallpaper.cpp
        Linux/ControlChannel.h
        Linux/ControlChannel.cpp
        Linux/ProtocolVideoRenderer.h
        Linux/ProtocolVideoRenderer.cpp)
    target_compile_definitions(VideoWallpaper PRIVATE
        VIDEORENDERER_MIRAGE_DISPLAY=1)
    # Public headers of the vendored MirageLinuxDisplay library (see Vendors/README).
    target_include_directories(VideoWallpaper PRIVATE "${MIRAGE_DISPLAY_SOURCE_DIR}/include")
    target_link_libraries(VideoWallpaper PRIVATE
        mirage_display_vulkan_static
        PkgConfig::MIRAGE_FFMPEG
        PkgConfig::MIRAGE_PULSE
        Threads::Threads
        VideoRendererEngine)
    target_compile_features(VideoWallpaper PRIVATE cxx_std_20)
else()
    message(FATAL_ERROR "VideoWallpaper has no platform backend for this system")
endif()

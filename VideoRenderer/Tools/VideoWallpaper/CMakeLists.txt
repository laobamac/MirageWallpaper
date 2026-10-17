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
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core)
    find_package(Vulkan REQUIRED)
    # FFmpeg 探测保留：macOS 后端仍在使用，Linux 合成路径依赖 libswscale。
    pkg_check_modules(MIRAGE_FFMPEG REQUIRED IMPORTED_TARGET
        libavformat>=58 libavcodec>=58 libavutil>=56 libswscale>=5 libswresample>=3)
    # libmpv 取代 SDL 音频与 FFmpeg 原始解码（解码/音频/同步/循环交由 libmpv）。
    pkg_check_modules(MIRAGE_MPV REQUIRED IMPORTED_TARGET mpv>=2.0)
    # headless EGL/GLES3：mpv OpenGL render API 的宿主（GPU 硬件缩放取帧）。
    pkg_check_modules(MIRAGE_EGL REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(MIRAGE_GLES REQUIRED IMPORTED_TARGET glesv2)
    pkg_check_modules(MIRAGE_GBM REQUIRED IMPORTED_TARGET gbm)
    pkg_check_modules(MIRAGE_DRM REQUIRED IMPORTED_TARGET libdrm)

    # Linux displays through the mirage-display protocol only (no X11 host).
    add_executable(VideoWallpaper
        Linux/VideoWallpaper.cpp
        Linux/ControlChannel.h
        Linux/ControlChannel.cpp
        Linux/ProtocolVideoRenderer.h
        Linux/ProtocolVideoRenderer.cpp)
    target_compile_definitions(VideoWallpaper PRIVATE
        VIDEORENDERER_MIRAGE_DISPLAY=1)
    # Public headers of the repository-maintained MirageLinuxDisplay library.
    target_include_directories(VideoWallpaper PRIVATE "${MIRAGE_DISPLAY_SOURCE_DIR}/include")
    target_link_libraries(VideoWallpaper PRIVATE
        mirage_display_static
        PkgConfig::MIRAGE_FFMPEG
        PkgConfig::MIRAGE_MPV
        PkgConfig::MIRAGE_EGL
        PkgConfig::MIRAGE_GLES
        PkgConfig::MIRAGE_GBM
        PkgConfig::MIRAGE_DRM
        Vulkan::Vulkan
        Threads::Threads
        Qt6::Core
        VideoRendererEngine)
    target_compile_features(VideoWallpaper PRIVATE cxx_std_20)
else()
    message(FATAL_ERROR "VideoWallpaper has no platform backend for this system")
endif()

if(WIN32)
    add_executable(VideoWallpaper Windows/VideoWallpaper.cpp Windows/ControlChannel.cpp)
    target_link_libraries(VideoWallpaper PRIVATE VideoRendererEngine user32)
    target_include_directories(VideoWallpaper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../Platform/Windows")
    if(MPV_RUNTIME)
        add_custom_command(TARGET VideoWallpaper POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MPV_RUNTIME}" "$<TARGET_FILE_DIR:VideoWallpaper>")
    endif()
    install(TARGETS VideoWallpaper RUNTIME DESTINATION .)
    if(MPV_RUNTIME)
        install(FILES "${MPV_RUNTIME}" DESTINATION .)
    endif()
    return()
endif()

add_executable(VideoWallpaper VideoWallpaper.mm ControlChannel.m)

target_link_libraries(VideoWallpaper PRIVATE
    VideoRendererEngine
    ${VR_FRAMEWORKS})

target_compile_features(VideoWallpaper PRIVATE cxx_std_20)
set_target_properties(VideoWallpaper PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON)
target_compile_options(VideoWallpaper PRIVATE -Wno-deprecated-declarations)

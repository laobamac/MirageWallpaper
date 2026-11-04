add_executable(VideoManifestTests WindowsVideoManifestTests.cpp)
target_link_libraries(VideoManifestTests PRIVATE VideoRendererEngine)
add_test(NAME VideoManifestTests COMMAND VideoManifestTests)

find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(VR_FFMPEG_EXECUTABLE ffmpeg)
if(Python3_Interpreter_FOUND AND VR_FFMPEG_EXECUTABLE AND TARGET VideoWallpaper)
    add_test(NAME WindowsVideoProtocolIntegration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/WindowsVideoProtocolIntegration.py"
        "$<TARGET_FILE:VideoWallpaper>" "${VR_FFMPEG_EXECUTABLE}")
    set_tests_properties(WindowsVideoProtocolIntegration PROPERTIES TIMEOUT 90)
endif()

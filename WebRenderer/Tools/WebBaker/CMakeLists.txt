#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

set(BAKE_SUPPORT "${CMAKE_CURRENT_SOURCE_DIR}/../../../VideoRenderer/Sources/Baking")
add_executable(WebBaker WebBaker.mm "${BAKE_SUPPORT}/BakeSupport.mm")
target_include_directories(WebBaker PRIVATE "${BAKE_SUPPORT}")
target_link_libraries(WebBaker PRIVATE WebRendererEngine ${WR_FRAMEWORKS} "-framework ScreenCaptureKit"
    "-framework AVFoundation" "-framework CoreImage" "-framework CoreMedia" "-framework CoreVideo")
target_compile_options(WebBaker PRIVATE -Wno-deprecated-declarations)

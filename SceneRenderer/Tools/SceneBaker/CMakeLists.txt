#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

enable_language(OBJCXX)
set(BAKE_SUPPORT "${CMAKE_CURRENT_SOURCE_DIR}/../../../VideoRenderer/Sources/Baking")
add_executable(SceneBaker SceneBaker.cpp SceneBakerMain.mm "${BAKE_SUPPORT}/BakeSupport.mm")
target_include_directories(SceneBaker PRIVATE "${BAKE_SUPPORT}")
target_compile_features(SceneBaker PRIVATE cxx_std_20)
target_compile_options(SceneBaker PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc;-Wno-deprecated-declarations>)
target_link_libraries(SceneBaker PRIVATE SceneRenderer::Engine "-framework Foundation" "-framework AVFoundation"
    "-framework CoreImage" "-framework CoreMedia" "-framework CoreVideo" "-framework AudioToolbox" "-framework CoreGraphics")
set_target_properties(SceneBaker PROPERTIES CXX_SCAN_FOR_MODULES ON)

#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

add_executable(VideoBaker VideoBaker.mm ../../Sources/Baking/BakeSupport.mm)
target_include_directories(VideoBaker PRIVATE ../../Sources/Baking)
target_link_libraries(VideoBaker PRIVATE VideoRendererEngine ${VR_FRAMEWORKS})
target_compile_options(VideoBaker PRIVATE -Wno-deprecated-declarations)

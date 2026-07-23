# RmskinRenderer engine library — Rainmeter skin model on CoreGraphics/CoreText.

add_library(RmskinRendererEngine STATIC
    RMLog.h
    RMIniFile.h
    RMIniFile.mm
    RMMathParser.h
    RMMathParser.mm
    RMConfigParser.h
    RMConfigParser.mm
    RMFontManager.h
    RMFontManager.mm
    RMMeasure.h
    RMMeasure.mm
    RMMeter.h
    RMMeter.mm
    RMBangs.h
    RMBangs.mm
    RMSkin.h
    RMSkin.mm
    RMSkinView.h
    RMSkinView.mm
    RMLayout.h
    RMLayout.mm)

target_compile_features(RmskinRendererEngine PUBLIC cxx_std_20)
set_target_properties(RmskinRendererEngine PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_include_directories(RmskinRendererEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(RmskinRendererEngine PUBLIC ${RM_FRAMEWORKS})

target_compile_options(RmskinRendererEngine PRIVATE
    -Wno-deprecated-declarations
    -Wno-unused-function)

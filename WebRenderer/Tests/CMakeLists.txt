# Copyright © 2026 王孝慈. All rights reserved.

if(APPLE)
    add_executable(WebRendererCompatibilityTests Live2DCompatibilityTests.mm)
    target_link_libraries(WebRendererCompatibilityTests PRIVATE WebRendererEngine)
    add_test(NAME WebRendererCompatibility COMMAND WebRendererCompatibilityTests)
endif()

if(WIN32)
    add_executable(WindowsWebRendererTests WindowsWebRendererTests.cpp)
    target_link_libraries(WindowsWebRendererTests PRIVATE WebRendererEngine winmm)
    target_compile_definitions(WindowsWebRendererTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(WindowsWebRendererTests PRIVATE /W4 /utf-8)
    endif()
    if(TARGET WebWallpaper)
        add_test(NAME WindowsWebRendererIntegration
            COMMAND WindowsWebRendererTests $<TARGET_FILE:WebWallpaper>)
    else()
        add_test(NAME WindowsWebRendererIntegration COMMAND WindowsWebRendererTests)
    endif()
    set_tests_properties(WindowsWebRendererIntegration PROPERTIES TIMEOUT 120)
endif()

find_program(WEBRENDERER_NODE_EXECUTABLE node)
if(WEBRENDERER_NODE_EXECUTABLE)
    add_test(NAME WebRendererLive2DRuntime
        COMMAND "${WEBRENDERER_NODE_EXECUTABLE}" --test
        "${CMAKE_CURRENT_SOURCE_DIR}/live2d-compatibility.test.mjs")
endif()

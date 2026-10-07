# Copyright © 2026 王孝慈. All rights reserved.

add_executable(WebRendererCompatibilityTests Live2DCompatibilityTests.mm)
target_link_libraries(WebRendererCompatibilityTests PRIVATE WebRendererEngine)
add_test(NAME WebRendererCompatibility COMMAND WebRendererCompatibilityTests)

find_program(WEBRENDERER_NODE_EXECUTABLE node)
if(WEBRENDERER_NODE_EXECUTABLE)
    add_test(NAME WebRendererLive2DRuntime
        COMMAND "${WEBRENDERER_NODE_EXECUTABLE}" --test
        "${CMAKE_CURRENT_SOURCE_DIR}/live2d-compatibility.test.mjs")
endif()

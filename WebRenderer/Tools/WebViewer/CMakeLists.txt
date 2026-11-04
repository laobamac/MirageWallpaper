# WebViewer — standalone debug window (NSWindow + WKViewer).

if(WIN32)
    add_executable(WebViewer Linux/WebViewer.cpp)
    target_link_libraries(WebViewer PRIVATE WebRendererEngine)
    return()
endif()

add_executable(WebViewer WebViewer.mm)

target_link_libraries(WebViewer PRIVATE
    WebRendererEngine
    ${WR_FRAMEWORKS})

target_compile_features(WebViewer PRIVATE cxx_std_20)
set_target_properties(WebViewer PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_compile_options(WebViewer PRIVATE -Wno-deprecated-declarations)

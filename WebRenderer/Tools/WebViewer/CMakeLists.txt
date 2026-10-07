# WebViewer — standalone debug window (NSWindow + WKViewer).

if(APPLE)
    add_executable(WebViewer WebViewer.mm)
else()
    add_executable(WebViewer Linux/WebViewer.cpp)
endif()

target_link_libraries(WebViewer PRIVATE
    WebRendererEngine
    ${WR_PLATFORM_LIBS})

target_compile_features(WebViewer PRIVATE cxx_std_20)
if(APPLE)
    set_target_properties(WebViewer PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_compile_options(WebViewer PRIVATE -Wno-deprecated-declarations)
else()
    target_compile_options(WebViewer PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

cmake_minimum_required(VERSION 3.28)

if(CMAKE_VERSION VERSION_LESS_EQUAL "4.3.0")
    message(FATAL_ERROR
        "CMake ${CMAKE_VERSION} is not supported. "
        "CMake 4.3.0 and older can generate broken C++20 module BMI rules with Clang 22. "
        "Use CMake 4.3.1 or newer.")
endif()

set(SCENERENDERER_VERSION 0.1.8)

project(SceneRenderer VERSION ${SCENERENDERER_VERSION})

# SceneRenderer is an embeddable engine with platform-specific standalone hosts.
# macOS uses MoltenVK and Core Audio; Windows uses Vulkan Win32 surfaces and WASAPI.
# The repository is intentionally laid out as an embeddable engine plus a small
# standalone tool, not as the final app shell.
if(APPLE)
    set(WAVSEN_AUDIO_BACKEND "coreaudio" CACHE STRING "Core Audio backend on macOS" FORCE)
    # MoltenVK as the Vulkan ICD. Hint Vulkan::Vulkan to the brew loader.
    if(NOT DEFINED ENV{VULKAN_SDK} AND NOT DEFINED CMAKE_PREFIX_PATH)
        # Prefer the Homebrew prefix matching the host architecture.
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
        else()
            list(APPEND CMAKE_PREFIX_PATH /usr/local /opt/homebrew)
        endif()
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ELF $ORIGIN doesn't exist on mach-o; use @loader_path so install rpath
# stays explicit and the build doesn't bake the literal "$ORIGIN" string.
set(CMAKE_INSTALL_RPATH "@loader_path")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_INSTALL_REMOVE_ENVIRONMENT_RPATH ON)

# Restrict to Clang.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT SR_ALLOW_NONCLANG)
    message(FATAL_ERROR
        "SceneRenderer requires Clang (got ${CMAKE_CXX_COMPILER_ID}). "
        "Re-run cmake with `-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`, "
        "or pass -DSR_ALLOW_NONCLANG=ON to bypass this check.")
endif()


if(WIN32)
    set(WAVSEN_AUDIO_BACKEND "wasapi" CACHE STRING "Windows WASAPI 音频后端" FORCE)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang>:/clang:-fsized-deallocation>")
else()
    find_package(PkgConfig REQUIRED)
endif()
find_package(Threads REQUIRED)

if(CMAKE_USE_PTHREADS_INIT)
    # Clang records pthread support in C++ module BMIs; keep all module targets compatible.
    add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-pthread>")
    add_link_options("$<$<LINK_LANG_AND_ID:CXX,Clang,GNU>:-pthread>")
endif()

include_directories(SYSTEM Vendor)
add_subdirectory(Vendor)

option(SCENERENDERER_BUILD_ENGINE "Build the SceneRenderer engine libraries" ON)

add_subdirectory(Sources/SceneRenderer)

if(NOT WIN32)
    pkg_check_modules(GLFW IMPORTED_TARGET glfw3)
endif()
option(SCENERENDERER_BUILD_VIEWER "Build the standalone SceneViewer tool" ${GLFW_FOUND})
if(SCENERENDERER_BUILD_ENGINE AND SCENERENDERER_BUILD_VIEWER)
    if(NOT GLFW_FOUND)
        message(FATAL_ERROR
            "Need glfw3 for the standalone viewer. Install glfw3 "
            "development headers or set SCENERENDERER_BUILD_VIEWER=OFF.")
    endif()
    add_subdirectory(Tools/SceneViewer)
endif()

option(SCENERENDERER_BUILD_WALLPAPER_HOST "Build the native desktop wallpaper host" ON)
if(SCENERENDERER_BUILD_ENGINE AND SCENERENDERER_BUILD_WALLPAPER_HOST)
    add_subdirectory(Tools/SceneWallpaper)
    if(APPLE)
        add_subdirectory(Tools/SceneScreenSaver)
        add_subdirectory(Tools/SceneBaker)
    endif()
endif()

option(SCENERENDERER_BUILD_TESTS "Build SceneRenderer regression tests" OFF)
if(SCENERENDERER_BUILD_ENGINE AND SCENERENDERER_BUILD_TESTS)
    enable_testing()
    if(WIN32)
        add_executable(SceneRendererWindowsPortabilityTests Tests/WindowsPortabilityRegression.cpp)
        target_link_libraries(SceneRendererWindowsPortabilityTests PRIVATE SceneRenderer::Types)
        set_property(TARGET SceneRendererWindowsPortabilityTests PROPERTY CXX_SCAN_FOR_MODULES ON)
        add_test(NAME SceneRendererWindowsPortability COMMAND SceneRendererWindowsPortabilityTests)
        set_tests_properties(SceneRendererWindowsPortability PROPERTIES TIMEOUT 15)
        add_executable(SceneRendererWasapiTests Tests/WindowsWasapiRegression.cpp
            Vendor/wavsen/src/audio/audio_wasapi.cpp)
        target_link_libraries(SceneRendererWasapiTests PRIVATE ole32 uuid)
        add_test(NAME SceneRendererWasapi COMMAND SceneRendererWasapiTests)
        set_tests_properties(SceneRendererWasapi PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)
    endif()
    add_executable(SceneRendererPerformanceTests Tests/PerformanceRegression.cpp
        Tools/SceneWallpaper/ControlChannel.cpp)
    target_link_libraries(SceneRendererPerformanceTests PRIVATE SceneRenderer::Engine)
    set_property(TARGET SceneRendererPerformanceTests PROPERTY CXX_SCAN_FOR_MODULES ON)
    add_test(NAME SceneRendererPerformance COMMAND SceneRendererPerformanceTests)
    set_tests_properties(SceneRendererPerformance PROPERTIES TIMEOUT 30)
    add_executable(SceneRendererTextGeometryTests Tests/TextGeometryRegression.cpp)
    target_link_libraries(SceneRendererTextGeometryTests PRIVATE SceneRenderer::Import)
    set_property(TARGET SceneRendererTextGeometryTests PROPERTY CXX_SCAN_FOR_MODULES ON)
    add_test(NAME SceneRendererTextGeometry COMMAND SceneRendererTextGeometryTests)

    find_package(Freetype REQUIRED)
    if(WIN32)
        find_package(Fontconfig REQUIRED)
    else()
        pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
    endif()
    add_executable(SceneRendererTextFontFallbackTests Tests/TextFontFallbackRegression.cpp)
    target_link_libraries(SceneRendererTextFontFallbackTests
        PRIVATE SceneRenderer::Import Freetype::Freetype PkgConfig::FONTCONFIG)
    set_property(TARGET SceneRendererTextFontFallbackTests PROPERTY CXX_SCAN_FOR_MODULES ON)
    add_test(NAME SceneRendererTextFontFallback COMMAND SceneRendererTextFontFallbackTests
        "${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts")
    set_tests_properties(SceneRendererTextFontFallback PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(SceneRendererLayerVisibilityTests Tests/LayerVisibilityRegression.cpp)
    target_link_libraries(SceneRendererLayerVisibilityTests PRIVATE SceneRenderer::Import)
    set_property(TARGET SceneRendererLayerVisibilityTests PROPERTY CXX_SCAN_FOR_MODULES ON)
    add_test(NAME SceneRendererLayerVisibility COMMAND SceneRendererLayerVisibilityTests)

    add_executable(SceneRendererRuntimeCompatibilityTests Tests/RuntimeCompatibilityRegression.cpp)
    target_link_libraries(SceneRendererRuntimeCompatibilityTests PRIVATE SceneRenderer::Render)
    set_property(TARGET SceneRendererRuntimeCompatibilityTests PROPERTY CXX_SCAN_FOR_MODULES ON)
    add_test(NAME SceneRendererRuntimeCompatibility COMMAND SceneRendererRuntimeCompatibilityTests)

    add_executable(SceneRendererWallpaperPositionTests Tests/WallpaperPositionRenderRegression.cpp)
    target_link_libraries(SceneRendererWallpaperPositionTests PRIVATE SceneRenderer::Engine)
    set_property(TARGET SceneRendererWallpaperPositionTests PROPERTY CXX_SCAN_FOR_MODULES ON)
    add_test(NAME SceneRendererWallpaperPosition COMMAND SceneRendererWallpaperPositionTests)
    set_tests_properties(SceneRendererWallpaperPosition PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 75)

    add_executable(SceneRendererScriptCompatibilityTests Tests/ScriptCompatibilityRegression.cpp)
    target_link_libraries(SceneRendererScriptCompatibilityTests PRIVATE SceneRenderer::Import)
    set_property(TARGET SceneRendererScriptCompatibilityTests PROPERTY CXX_SCAN_FOR_MODULES ON)
    add_test(NAME SceneRendererScriptCompatibility COMMAND SceneRendererScriptCompatibilityTests)

    add_executable(SceneRendererPuppetAlphaTests Tests/PuppetAlphaRegression.cpp)
    target_link_libraries(SceneRendererPuppetAlphaTests PRIVATE SceneRenderer::Import)
    set_property(TARGET SceneRendererPuppetAlphaTests PROPERTY CXX_SCAN_FOR_MODULES ON)
    add_test(NAME SceneRendererPuppetAlpha COMMAND SceneRendererPuppetAlphaTests)
endif()

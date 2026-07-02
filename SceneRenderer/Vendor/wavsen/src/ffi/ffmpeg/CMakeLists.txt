# wavsen::ffi::ffmpeg — C++20 module wrappers for libav* + swscale (+ swresample).
# Consumers `import avutil; import avcodec; import avformat; import swscale; [import swresample;]`.

add_library(wavsen_ffi_ffmpeg STATIC)
add_library(wavsen::ffi::ffmpeg ALIAS wavsen_ffi_ffmpeg)

target_compile_features(wavsen_ffi_ffmpeg PUBLIC cxx_std_20)
target_compile_options(wavsen_ffi_ffmpeg PRIVATE
    -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
)

target_sources(wavsen_ffi_ffmpeg
    PUBLIC FILE_SET CXX_MODULES FILES
        avutil.cppm
        avcodec.cppm
        avformat.cppm
        swscale.cppm
)

target_link_libraries(wavsen_ffi_ffmpeg
    PUBLIC
        PkgConfig::WAVSEN_AVFORMAT
        PkgConfig::WAVSEN_AVCODEC
        PkgConfig::WAVSEN_AVUTIL
        PkgConfig::WAVSEN_SWSCALE
    PRIVATE
        # avutil.cppm GMF #includes <vulkan/vulkan.h> for hwcontext_vulkan.h.
        # Consumers that touch AVVulkanDeviceContext-bearing fields need to
        # link wavsen::ffi::vulkan separately (or include vulkan.h themselves).
        Vulkan::Vulkan
)

# swresample is optional at the workspace level (only audio targets need it).
# Add the swresample module + link only when the parent scope found it.
if(WAVSEN_SWRESAMPLE_FOUND)
    target_sources(wavsen_ffi_ffmpeg
        PUBLIC FILE_SET CXX_MODULES FILES
            swresample.cppm
    )
    target_link_libraries(wavsen_ffi_ffmpeg PUBLIC PkgConfig::WAVSEN_SWRESAMPLE)
endif()

# VAAPI: avutil.cppm conditionally exposes hwcontext_vaapi.h when both
# libva is present (so <va/va.h> resolves) and the consumer compiles the
# avutil partition with WAVSEN_HAS_VAAPI=1. The flag is set on the FFI
# module itself (so module BMI matches) AND propagated to consumers via
# INTERFACE_COMPILE_DEFINITIONS.
if(WAVSEN_LIBVA_FOUND)
    target_compile_definitions(wavsen_ffi_ffmpeg
        PUBLIC WAVSEN_HAS_VAAPI=1)
    target_link_libraries(wavsen_ffi_ffmpeg PUBLIC PkgConfig::WAVSEN_LIBVA)
endif()

set_property(TARGET wavsen_ffi_ffmpeg PROPERTY POSITION_INDEPENDENT_CODE ON)
set_target_properties(wavsen_ffi_ffmpeg PROPERTIES CXX_SCAN_FOR_MODULES ON)

# wavsen::ffi::vulkan — C++20 module wrapper for <vulkan/vulkan.h>.
# Consumers `import vulkan;` instead of `#include <vulkan/vulkan.h>`.
# Macros that resist tokenisation (VK_NULL_HANDLE, VK_TRUE, queue
# sentinels, extension-name strings) are captured + #undef'd + re-exported
# as constexpr alternates of the same name.

add_library(wavsen_ffi_vulkan STATIC)
add_library(wavsen::ffi::vulkan ALIAS wavsen_ffi_vulkan)

target_compile_features(wavsen_ffi_vulkan PUBLIC cxx_std_20)
target_compile_options(wavsen_ffi_vulkan PRIVATE
    -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
)

target_sources(wavsen_ffi_vulkan
    PUBLIC FILE_SET CXX_MODULES FILES
        vulkan.cppm
)

target_link_libraries(wavsen_ffi_vulkan
    PUBLIC
        Vulkan::Vulkan
)

set_property(TARGET wavsen_ffi_vulkan PROPERTY POSITION_INDEPENDENT_CODE ON)
set_target_properties(wavsen_ffi_vulkan PROPERTIES CXX_SCAN_FOR_MODULES ON)

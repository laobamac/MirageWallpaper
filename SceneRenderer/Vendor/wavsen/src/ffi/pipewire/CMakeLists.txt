# wavsen::ffi::pipewire — C++20 module wrapper around libpipewire-0.3.
# Consumers `import pipewire;` instead of `#include <pipewire/pipewire.h>`.

add_library(wavsen_ffi_pipewire STATIC)
add_library(wavsen::ffi::pipewire ALIAS wavsen_ffi_pipewire)

target_compile_features(wavsen_ffi_pipewire PUBLIC cxx_std_20)
target_compile_options(wavsen_ffi_pipewire PRIVATE
    -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
)

target_sources(wavsen_ffi_pipewire
    PUBLIC FILE_SET CXX_MODULES FILES
        pipewire.cppm
)

target_link_libraries(wavsen_ffi_pipewire
    PUBLIC
        PkgConfig::WAVSEN_LIBPIPEWIRE
)

set_property(TARGET wavsen_ffi_pipewire PROPERTY POSITION_INDEPENDENT_CODE ON)
set_target_properties(wavsen_ffi_pipewire PROPERTIES CXX_SCAN_FOR_MODULES ON)

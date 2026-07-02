# wavsen::ffi::pulse — C++20 module wrapper around libpulse.
# Consumers `import pulse;` instead of `#include <pulse/pulseaudio.h>`.

add_library(wavsen_ffi_pulse STATIC)
add_library(wavsen::ffi::pulse ALIAS wavsen_ffi_pulse)

target_compile_features(wavsen_ffi_pulse PUBLIC cxx_std_20)
target_compile_options(wavsen_ffi_pulse PRIVATE
    -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
)

target_sources(wavsen_ffi_pulse
    PUBLIC FILE_SET CXX_MODULES FILES
        pulse.cppm
)

target_link_libraries(wavsen_ffi_pulse
    PUBLIC
        PkgConfig::WAVSEN_LIBPULSE
)

set_property(TARGET wavsen_ffi_pulse PROPERTY POSITION_INDEPENDENT_CODE ON)
set_target_properties(wavsen_ffi_pulse PROPERTIES CXX_SCAN_FOR_MODULES ON)

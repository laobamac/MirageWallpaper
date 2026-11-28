//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_SHADER_REWRITER_HPP
#define MIRAGE_SCENE_MOBILE_SHADER_REWRITER_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mirage::mobile {

bool IsShaderPath(std::string_view rel_path);

// Rewrites a desktop Wallpaper Engine shader into mobile GLSL ES form:
// - zcompat Workshop overrides (external assets/zcompat/scene/shaders + built-ins)
// - `float mod(` -> `float _deprFn_0(`
// - Byte-0 `#include "common_blur.h"` relocation after `uniform sampler2D g_Texture0;`
// - Stem-specific patches (`foliagesway`, `blend`, `shift_hue`, `xray`, `perspective`, `skew`)
// - Type-aware GLSL ES token normalization (`sample` -> `_sample`, `sampleCount`, float literal promotion
//   that preserves comments, directives, array indices, and integer declarations/overloads)
// - `BlendOpacity(a, b, f, o)` -> `mix(a,f(a,b),o)` macro expansion
// - CRLF line endings matching mobile Wallpaper Engine expectations.
// Returns `{rewritten_source, changed}`.
std::pair<std::string, bool> RewriteShaderForMobile(
    std::string_view rel_path,
    std::string_view source,
    uint64_t workshop_id,
    const std::optional<std::filesystem::path>& external_zcompat_dir = std::nullopt);

} // namespace mirage::mobile

#endif /* MIRAGE_SCENE_MOBILE_SHADER_REWRITER_HPP */

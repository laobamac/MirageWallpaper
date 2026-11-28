//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_JSON_COMPAT_HPP
#define MIRAGE_SCENE_MOBILE_JSON_COMPAT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <nlohmann/json.hpp>
#pragma clang diagnostic pop

namespace mirage::mobile {

using Json = nlohmann::json;

// Parses JSON with Wallpaper Engine JsonCpp-compatible leniency:
// strips optional UTF-8 BOM, `//` line comments, `/* ... */` block comments,
// and trailing commas before `}` or `]` outside quoted string literals.
std::optional<Json> ParseJsonLenient(const uint8_t* data, size_t size);
std::optional<Json> ParseJsonLenient(const std::vector<uint8_t>& bytes);
std::optional<Json> ParseJsonLenient(std::string_view text);

// Formats a nlohmann::json value using exact JsonCpp::StyledWriter formatting
// (`\t` indent, `"key" : value`, `{`/`[` on a new line after `"key" : \r\n`,
// inline scalar arrays up to 74 UTF-8 bytes, sorted keys, `\r\n` newlines,
// `%.8g` float formatting, and no trailing newline).
std::vector<uint8_t> ToJsonCppStyledBytes(const Json& value);
std::string ToJsonCppStyledString(const Json& value);

} // namespace mirage::mobile

#endif /* MIRAGE_SCENE_MOBILE_JSON_COMPAT_HPP */

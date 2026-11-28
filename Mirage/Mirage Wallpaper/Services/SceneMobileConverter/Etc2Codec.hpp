//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_ETC2_CODEC_HPP
#define MIRAGE_SCENE_MOBILE_ETC2_CODEC_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mirage::mobile {

// Encodes an RGBA8888 image into GL_COMPRESSED_RGB8_ETC2 (8 bytes per 4x4 block).
std::vector<uint8_t> EncodeEtc2Rgb8(
    const uint8_t* rgba,
    size_t width,
    size_t height,
    size_t max_threads = 1,
    const volatile int32_t* cancel_flag = nullptr);

// Encodes an RGBA8888 image into GL_COMPRESSED_RGBA8_ETC2_EAC (16 bytes per 4x4 block:
// 8-byte EAC alpha block followed by 8-byte alpha-aware ETC2 RGB8 block).
std::vector<uint8_t> EncodeEtc2Rgba8(
    const uint8_t* rgba,
    size_t width,
    size_t height,
    size_t max_threads = 1,
    const volatile int32_t* cancel_flag = nullptr);

// Decodes GL_COMPRESSED_RGB8_ETC2 (8 bytes per 4x4 block) into RGBA8888 (A = 255).
std::vector<uint8_t> DecodeEtc2Rgb8(
    const uint8_t* src,
    size_t src_size,
    size_t width,
    size_t height,
    const std::string& label);

// Decodes GL_COMPRESSED_RGBA8_ETC2_EAC (16 bytes per 4x4 block) into RGBA8888.
std::vector<uint8_t> DecodeEtc2Rgba8(
    const uint8_t* src,
    size_t src_size,
    size_t width,
    size_t height,
    const std::string& label);

// Low-level block primitives exposed for unit testing.
uint64_t EncodeEacAlphaBlock(const std::array<uint8_t, 16>& alpha) noexcept;
std::array<uint8_t, 16> DecodeEacAlphaBlock(uint64_t word) noexcept;
uint64_t EncodeEtc2RgbBlock(
    const std::array<std::array<uint8_t, 3>, 16>& rgb,
    uint16_t visible_mask = 0xFFFFU) noexcept;
std::array<std::array<uint8_t, 3>, 16> DecodeEtc2RgbBlock(uint64_t word) noexcept;

} // namespace mirage::mobile

#endif /* MIRAGE_SCENE_MOBILE_ETC2_CODEC_HPP */

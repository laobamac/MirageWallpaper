//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_DXT_DECODER_HPP
#define MIRAGE_SCENE_MOBILE_DXT_DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mirage::mobile {

std::vector<uint8_t> DecodeRgba8888(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label);

std::vector<uint8_t> DecodeRgb888(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label);

std::vector<uint8_t> DecodeRg88(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label);

std::vector<uint8_t> DecodeR8(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label);

std::vector<uint8_t> DecodeBc1(
    const uint8_t* src, size_t src_size, size_t width, size_t height,
    bool one_bit_alpha, const std::string& label);

std::vector<uint8_t> DecodeBc2(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label);

std::vector<uint8_t> DecodeBc3(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label);

} // namespace mirage::mobile

#endif /* MIRAGE_SCENE_MOBILE_DXT_DECODER_HPP */

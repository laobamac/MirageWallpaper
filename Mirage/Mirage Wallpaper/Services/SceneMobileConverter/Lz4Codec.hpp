//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_LZ4_CODEC_HPP
#define MIRAGE_SCENE_MOBILE_LZ4_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mirage::mobile {

constexpr size_t kWeLz4MinSize = 16384;
constexpr float kWeLz4MaxRatio = 0.95f;
constexpr uint64_t kMaxLz4DecompressedSize = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxLz4ExpansionRatio = 256ULL;

std::vector<uint8_t> Lz4DecompressBlock(
    const uint8_t* src,
    size_t src_size,
    size_t uncompressed_size,
    const std::string& label);

std::vector<uint8_t> Lz4CompressBlock(
    const uint8_t* src,
    size_t src_size,
    const std::string& label);

// Wallpaper Engine resourcecompiler64.exe LZ4 threshold gate:
// returns `(payload, is_lz4)`. Only compresses when `raw.size() >= 16384`
// and `compressed.size() / raw.size() < 0.95f`.
std::pair<std::vector<uint8_t>, bool> CompressTexMipIfBeneficial(
    const std::vector<uint8_t>& raw,
    const std::string& label);

} // namespace mirage::mobile

#endif /* MIRAGE_SCENE_MOBILE_LZ4_CODEC_HPP */

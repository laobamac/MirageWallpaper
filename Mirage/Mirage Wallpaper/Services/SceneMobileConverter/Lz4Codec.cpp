//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "Lz4Codec.hpp"
#include "MobileInternal.hpp"

#include <compression.h>

namespace mirage::mobile {

std::vector<uint8_t> Lz4DecompressBlock(
    const uint8_t* src,
    size_t src_size,
    size_t uncompressed_size,
    const std::string& label) {
    if (!src || src_size == 0 || uncompressed_size == 0 ||
        static_cast<uint64_t>(uncompressed_size) > kMaxLz4DecompressedSize ||
        static_cast<uint64_t>(uncompressed_size) > static_cast<uint64_t>(src_size) * kMaxLz4ExpansionRatio) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }

    std::vector<uint8_t> dst(uncompressed_size);
    size_t decoded = compression_decode_buffer(
        dst.data(),
        dst.size(),
        src,
        src_size,
        nullptr,
        COMPRESSION_LZ4_RAW);
    if (decoded != uncompressed_size) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    return dst;
}

std::vector<uint8_t> Lz4CompressBlock(
    const uint8_t* src,
    size_t src_size,
    const std::string& label) {
    if (!src || src_size == 0 || static_cast<uint64_t>(src_size) > kMaxLz4DecompressedSize) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }

    size_t capacity = src_size + src_size / 255 + 64;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<uint8_t> dst(capacity);
        size_t encoded = compression_encode_buffer(
            dst.data(),
            dst.size(),
            src,
            src_size,
            nullptr,
            COMPRESSION_LZ4_RAW);
        if (encoded > 0) {
            dst.resize(encoded);
            return dst;
        }
        capacity *= 2;
    }
    throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
}

std::pair<std::vector<uint8_t>, bool> CompressTexMipIfBeneficial(
    const std::vector<uint8_t>& raw,
    const std::string& label) {
    if (raw.size() < kWeLz4MinSize) {
        return {raw, false};
    }
    auto compressed = Lz4CompressBlock(raw.data(), raw.size(), label);
    float ratio = static_cast<float>(compressed.size()) / static_cast<float>(raw.size());
    if (ratio < kWeLz4MaxRatio) {
        return {std::move(compressed), true};
    }
    return {raw, false};
}

} // namespace mirage::mobile

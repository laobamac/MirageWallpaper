//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "DxtDecoder.hpp"
#include "MobileInternal.hpp"

#include <algorithm>
#include <array>

namespace mirage::mobile {

namespace {

constexpr uint64_t kMaxRgbaBytes = 512ULL * 1024ULL * 1024ULL;

size_t CheckedPixels(size_t width, size_t height, const std::string& label) {
    if (width == 0 || height == 0) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    uint64_t px = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (px / static_cast<uint64_t>(width) != static_cast<uint64_t>(height) ||
        px > kMaxRgbaBytes / 4) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    return static_cast<size_t>(px);
}

inline std::array<uint8_t, 3> Rgb565ToRgb8(uint16_t c) noexcept {
    uint8_t r5 = static_cast<uint8_t>((c >> 11) & 0x1fU);
    uint8_t g6 = static_cast<uint8_t>((c >> 5) & 0x3fU);
    uint8_t b5 = static_cast<uint8_t>(c & 0x1fU);
    return {
        static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
        static_cast<uint8_t>((g6 << 2) | (g6 >> 4)),
        static_cast<uint8_t>((b5 << 3) | (b5 >> 2)),
    };
}

void DecodeBc1ColorBlock(
    const uint8_t* bytes,
    bool force_four_color,
    bool one_bit_alpha,
    uint8_t out[64]) noexcept {
    uint16_t c0 = ReadU16LE(bytes);
    uint16_t c1 = ReadU16LE(bytes + 2);
    uint32_t idx = ReadU32LE(bytes + 4);

    auto rgb0 = Rgb565ToRgb8(c0);
    auto rgb1 = Rgb565ToRgb8(c1);

    uint8_t palette[4][4];
    palette[0][0] = rgb0[0]; palette[0][1] = rgb0[1]; palette[0][2] = rgb0[2]; palette[0][3] = 255;
    palette[1][0] = rgb1[0]; palette[1][1] = rgb1[1]; palette[1][2] = rgb1[2]; palette[1][3] = 255;

    if (force_four_color || c0 > c1) {
        palette[2][0] = static_cast<uint8_t>((2 * rgb0[0] + rgb1[0] + 1) / 3);
        palette[2][1] = static_cast<uint8_t>((2 * rgb0[1] + rgb1[1] + 1) / 3);
        palette[2][2] = static_cast<uint8_t>((2 * rgb0[2] + rgb1[2] + 1) / 3);
        palette[2][3] = 255;

        palette[3][0] = static_cast<uint8_t>((rgb0[0] + 2 * rgb1[0] + 1) / 3);
        palette[3][1] = static_cast<uint8_t>((rgb0[1] + 2 * rgb1[1] + 1) / 3);
        palette[3][2] = static_cast<uint8_t>((rgb0[2] + 2 * rgb1[2] + 1) / 3);
        palette[3][3] = 255;
    } else {
        palette[2][0] = static_cast<uint8_t>((rgb0[0] + rgb1[0]) / 2);
        palette[2][1] = static_cast<uint8_t>((rgb0[1] + rgb1[1]) / 2);
        palette[2][2] = static_cast<uint8_t>((rgb0[2] + rgb1[2]) / 2);
        palette[2][3] = 255;

        palette[3][0] = 0;
        palette[3][1] = 0;
        palette[3][2] = 0;
        palette[3][3] = one_bit_alpha ? 0 : 255;
    }

    for (size_t i = 0; i < 16; ++i) {
        uint32_t code = (idx >> (i * 2)) & 0x03U;
        std::memcpy(out + i * 4, palette[code], 4);
    }
}

inline void WriteBlockRowMajor(
    uint8_t* out,
    size_t width,
    size_t height,
    size_t bx,
    size_t by,
    const uint8_t block[64]) noexcept {
    size_t x0 = bx * 4;
    size_t y0 = by * 4;
    if (x0 + 4 <= width && y0 + 4 <= height) {
        size_t stride = width * 4;
        uint8_t* dst = out + (y0 * width + x0) * 4;
        for (size_t y = 0; y < 4; ++y) {
            std::memcpy(dst, block + y * 16, 16);
            dst += stride;
        }
        return;
    }
    for (size_t y = 0; y < 4; ++y) {
        size_t py = y0 + y;
        if (py >= height) break;
        size_t copy_px = std::min<size_t>(width - x0, 4);
        std::memcpy(out + (py * width + x0) * 4, block + y * 16, copy_px * 4);
    }
}

} // namespace

std::vector<uint8_t> DecodeRgba8888(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label) {
    size_t px = CheckedPixels(width, height, label);
    size_t expected = px * 4;
    if (!src || src_size < expected) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    return std::vector<uint8_t>(src, src + expected);
}

std::vector<uint8_t> DecodeRgb888(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label) {
    size_t px = CheckedPixels(width, height, label);
    size_t expected = px * 3;
    if (!src || src_size < expected) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    std::vector<uint8_t> out(px * 4);
    for (size_t i = 0; i < px; ++i) {
        out[i * 4] = src[i * 3];
        out[i * 4 + 1] = src[i * 3 + 1];
        out[i * 4 + 2] = src[i * 3 + 2];
        out[i * 4 + 3] = 255;
    }
    return out;
}

std::vector<uint8_t> DecodeRg88(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label) {
    size_t px = CheckedPixels(width, height, label);
    size_t expected = px * 2;
    if (!src || src_size < expected) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    std::vector<uint8_t> out(px * 4);
    for (size_t i = 0; i < px; ++i) {
        out[i * 4] = src[i * 2];
        out[i * 4 + 1] = src[i * 2 + 1];
        out[i * 4 + 2] = 0;
        out[i * 4 + 3] = 255;
    }
    return out;
}

std::vector<uint8_t> DecodeR8(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label) {
    size_t px = CheckedPixels(width, height, label);
    if (!src || src_size < px) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    std::vector<uint8_t> out(px * 4);
    for (size_t i = 0; i < px; ++i) {
        uint8_t v = src[i];
        out[i * 4] = v;
        out[i * 4 + 1] = v;
        out[i * 4 + 2] = v;
        out[i * 4 + 3] = 255;
    }
    return out;
}

std::vector<uint8_t> DecodeBc1(
    const uint8_t* src, size_t src_size, size_t width, size_t height,
    bool one_bit_alpha, const std::string& label) {
    size_t px = CheckedPixels(width, height, label);
    size_t bw = (width + 3) / 4;
    size_t bh = (height + 3) / 4;
    size_t needed = bw * bh * 8;
    if (!src || src_size < needed) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }

    std::vector<uint8_t> out(px * 4);
    const uint8_t* p = src;
    uint8_t block[64];
    for (size_t by = 0; by < bh; ++by) {
        for (size_t bx = 0; bx < bw; ++bx) {
            DecodeBc1ColorBlock(p, false, one_bit_alpha, block);
            p += 8;
            WriteBlockRowMajor(out.data(), width, height, bx, by, block);
        }
    }
    return out;
}

std::vector<uint8_t> DecodeBc2(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label) {
    size_t px = CheckedPixels(width, height, label);
    size_t bw = (width + 3) / 4;
    size_t bh = (height + 3) / 4;
    size_t needed = bw * bh * 16;
    if (!src || src_size < needed) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }

    std::vector<uint8_t> out(px * 4);
    const uint8_t* p = src;
    uint8_t block[64];
    for (size_t by = 0; by < bh; ++by) {
        for (size_t bx = 0; bx < bw; ++bx) {
            uint64_t alpha_word = ReadU64LE(p);
            DecodeBc1ColorBlock(p + 8, true, false, block);
            for (size_t i = 0; i < 16; ++i) {
                uint8_t a4 = static_cast<uint8_t>((alpha_word >> (i * 4)) & 0x0fU);
                block[i * 4 + 3] = static_cast<uint8_t>((a4 << 4) | a4);
            }
            p += 16;
            WriteBlockRowMajor(out.data(), width, height, bx, by, block);
        }
    }
    return out;
}

std::vector<uint8_t> DecodeBc3(
    const uint8_t* src, size_t src_size, size_t width, size_t height, const std::string& label) {
    size_t px = CheckedPixels(width, height, label);
    size_t bw = (width + 3) / 4;
    size_t bh = (height + 3) / 4;
    size_t needed = bw * bh * 16;
    if (!src || src_size < needed) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }

    std::vector<uint8_t> out(px * 4);
    const uint8_t* p = src;
    uint8_t block[64];
    for (size_t by = 0; by < bh; ++by) {
        for (size_t bx = 0; bx < bw; ++bx) {
            uint32_t a0 = p[0];
            uint32_t a1 = p[1];
            uint8_t lut[8];
            lut[0] = static_cast<uint8_t>(a0);
            lut[1] = static_cast<uint8_t>(a1);
            if (a0 > a1) {
                lut[2] = static_cast<uint8_t>((6 * a0 + a1 + 3) / 7);
                lut[3] = static_cast<uint8_t>((5 * a0 + 2 * a1 + 3) / 7);
                lut[4] = static_cast<uint8_t>((4 * a0 + 3 * a1 + 3) / 7);
                lut[5] = static_cast<uint8_t>((3 * a0 + 4 * a1 + 3) / 7);
                lut[6] = static_cast<uint8_t>((2 * a0 + 5 * a1 + 3) / 7);
                lut[7] = static_cast<uint8_t>((a0 + 6 * a1 + 3) / 7);
            } else {
                lut[2] = static_cast<uint8_t>((4 * a0 + a1 + 2) / 5);
                lut[3] = static_cast<uint8_t>((3 * a0 + 2 * a1 + 2) / 5);
                lut[4] = static_cast<uint8_t>((2 * a0 + 3 * a1 + 2) / 5);
                lut[5] = static_cast<uint8_t>((a0 + 4 * a1 + 2) / 5);
                lut[6] = 0;
                lut[7] = 255;
            }

            uint64_t idx_bits = 0;
            for (size_t j = 0; j < 6; ++j) {
                idx_bits |= static_cast<uint64_t>(p[2 + j]) << (j * 8);
            }

            DecodeBc1ColorBlock(p + 8, true, false, block);
            for (size_t i = 0; i < 16; ++i) {
                size_t a_idx = static_cast<size_t>((idx_bits >> (i * 3)) & 0x07U);
                block[i * 4 + 3] = lut[a_idx];
            }
            p += 16;
            WriteBlockRowMajor(out.data(), width, height, bx, by, block);
        }
    }
    return out;
}

} // namespace mirage::mobile

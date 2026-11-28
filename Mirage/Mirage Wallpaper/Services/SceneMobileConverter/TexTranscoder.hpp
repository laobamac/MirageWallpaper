//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_TEX_TRANSCODER_HPP
#define MIRAGE_SCENE_MOBILE_TEX_TRANSCODER_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mirage::mobile {

constexpr uint32_t kTexFlagNoInterpolation = 0x00000001U;
constexpr uint32_t kTexFlagClampUvs = 0x00000002U;
constexpr uint32_t kTexFlagIsGif = 0x00000004U;
constexpr uint32_t kTexFlagNoMipmaps = 0x00000008U;
constexpr uint32_t kTexFlagIsVideoTexture = 0x00000020U;
constexpr uint32_t kTexFlagIs3D = 0x00000040U;
constexpr uint32_t kTexFlagClearOnMobile = 0x40000000U;

enum class TexFormat : uint32_t {
    Rgba8888 = 0,
    Rgb888 = 1,
    Rgba16161616f = 2,
    Etc2Rgb8 = 3,
    Dxt5 = 4,
    Etc2Rgba8 = 5,
    Dxt3 = 6,
    Dxt1 = 7,
    Rg88 = 8,
    R8 = 9,
    Rg1616f = 10,
    R16f = 11,
    Dxt1a = 12,
};

inline bool IsKnownTexFormat(uint32_t raw) noexcept {
    return raw <= 12U;
}

inline bool IsDxtFormat(TexFormat fmt) noexcept {
    return fmt == TexFormat::Dxt5 || fmt == TexFormat::Dxt3 ||
           fmt == TexFormat::Dxt1 || fmt == TexFormat::Dxt1a;
}

struct TexHeader {
    uint32_t texv_version = 5;
    uint32_t texi_version = 1;
    uint32_t format_raw = 0;
    uint32_t flags = 0;
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    std::optional<uint32_t> depth;
    uint32_t unk_int0 = 0;
};

struct TexMip {
    uint32_t width = 0;
    uint32_t height = 0;
    std::optional<uint32_t> depth;
    bool is_lz4 = false;
    uint32_t uncompressed_size = 0;
    std::vector<uint8_t> data;
};

struct TexImage {
    std::vector<TexMip> mips;
};

struct TexSpriteFrame {
    uint32_t image_id = 0;
    float frametime = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float width_y = 0.0f;
    float height_x = 0.0f;
    float height = 0.0f;
};

struct TexSpriteSection {
    uint32_t version = 3;
    uint32_t gif_width = 0;
    uint32_t gif_height = 0;
    std::vector<TexSpriteFrame> frames;
};

struct TexFile {
    TexHeader header;
    uint32_t texb_version = 4;
    int32_t free_image_format = -1;
    std::vector<TexImage> images;
    std::optional<TexSpriteSection> sprites;

    static TexHeader ParseHeader(const uint8_t* data, size_t size, const std::string& label);
    static TexFile FromBytes(const uint8_t* data, size_t size, const std::string& label);
    std::vector<uint8_t> ToBytes(const std::string& label) const;
};

struct TexTranscodeOptions {
    bool force_etc2 = true;
    uint32_t shrink = 1;
    bool pixel_art = false;
    bool prefer_rgb8_when_opaque = false;
    size_t inner_threads = 1;
    const volatile int32_t* cancel_flag = nullptr;
};

struct TexTranscodeResult {
    bool transcoded = false;
    uint32_t from_format = 0;
    uint32_t to_format = 0;
    uint32_t actual_shrink = 1;
    bool lz4_compressed = false;
    std::string skip_reason;
    std::vector<uint8_t> bytes; // Empty when transcoded == false (caller streams original zero-copy)
};

// Transcodes a `.tex` byte buffer for mobile OpenGL ES 3.0.
// When the texture should pass through untouched (masks, 3D LUTs, video textures, HDR floats,
// already-ETC2, or raw RGB888 without force_etc2), returns `transcoded = false` with empty `bytes`
// so the caller can stream the original slice zero-copy.
TexTranscodeResult TranscodeTexBytesForMobile(
    const uint8_t* data,
    size_t size,
    const std::string& label,
    const TexTranscodeOptions& opts);

// Decodes image 0 mip 0 of a TexFile to RGBA8888 (useful for inspection & regression tests).
struct DecodedTexImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

DecodedTexImage DecodeTexMip0Rgba(
    const TexFile& tex,
    size_t image_index,
    bool crop_to_image_dims,
    const std::string& label);

} // namespace mirage::mobile

#endif /* MIRAGE_SCENE_MOBILE_TEX_TRANSCODER_HPP */

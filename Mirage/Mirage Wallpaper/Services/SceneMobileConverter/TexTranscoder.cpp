//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "TexTranscoder.hpp"
#include "DxtDecoder.hpp"
#include "Etc2Codec.hpp"
#include "Lz4Codec.hpp"
#include "MobileInternal.hpp"

#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#pragma clang diagnostic pop

namespace mirage::mobile {

namespace {

constexpr uint64_t kMaxRgbaImageBytes = 512ULL * 1024ULL * 1024ULL;

uint32_t ReadMagic9(const uint8_t* bytes, size_t len, size_t& cursor, const char prefix[4], const std::string& label) {
    if (cursor + 9 > len) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    const uint8_t* s = bytes + cursor;
    if (s[8] != 0 || std::memcmp(s, prefix, 4) != 0) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    uint32_t ver = 0;
    for (int i = 4; i < 8; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
        }
        ver = ver * 10U + static_cast<uint32_t>(s[i] - '0');
    }
    cursor += 9;
    return ver;
}

uint32_t ReadU32(const uint8_t* bytes, size_t len, size_t& cursor, const std::string& label) {
    if (cursor + 4 > len) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    uint32_t v = ReadU32LE(bytes + cursor);
    cursor += 4;
    return v;
}

int32_t ReadI32(const uint8_t* bytes, size_t len, size_t& cursor, const std::string& label) {
    if (cursor + 4 > len) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    int32_t v = ReadI32LE(bytes + cursor);
    cursor += 4;
    return v;
}

float ReadF32(const uint8_t* bytes, size_t len, size_t& cursor, const std::string& label) {
    if (cursor + 4 > len) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    float v = ReadF32LE(bytes + cursor);
    cursor += 4;
    return v;
}

void SkipCString(const uint8_t* bytes, size_t len, size_t& cursor, const std::string& label) {
    while (cursor < len && bytes[cursor] != 0) ++cursor;
    if (cursor >= len) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    ++cursor;
}

enum class SniffedContainer {
    None,
    Image,
    Video,
};

// Magic-bytes sniffer matching Mirage's TextureDecoder.cpp to handle PKGV0022+ assets
// where free_image_format is declared as -1 even though the mip payload is an embedded image/video.
SniffedContainer SniffEmbeddedContainer(const uint8_t* data, size_t size) noexcept {
    if (!data || size < 4) return SniffedContainer::None;
    if (size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return SniffedContainer::Image;
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return SniffedContainer::Image;
    if (size >= 6 && (std::memcmp(data, "GIF87a", 6) == 0 || std::memcmp(data, "GIF89a", 6) == 0)) {
        return SniffedContainer::Image;
    }
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return SniffedContainer::Image;
    if (size >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0) return SniffedContainer::Video;
    if (size >= 4 && data[0] == 0x1a && data[1] == 0x45 && data[2] == 0xdf && data[3] == 0xa3) {
        return SniffedContainer::Video;
    }
    return SniffedContainer::None;
}

std::vector<uint8_t> CropRgbaTopLeft(
    const uint8_t* src,
    size_t src_w,
    size_t dst_w,
    size_t dst_h) {
    std::vector<uint8_t> out(dst_w * dst_h * 4);
    for (size_t y = 0; y < dst_h; ++y) {
        std::memcpy(out.data() + y * dst_w * 4, src + y * src_w * 4, dst_w * 4);
    }
    return out;
}

std::vector<uint8_t> ResizeRgbaNearest(
    const uint8_t* src,
    uint32_t src_w,
    uint32_t src_h,
    uint32_t dst_w,
    uint32_t dst_h) {
    std::vector<uint8_t> out(static_cast<size_t>(dst_w) * dst_h * 4);
    for (uint32_t y = 0; y < dst_h; ++y) {
        uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * src_h) / dst_h);
        if (sy >= src_h) sy = src_h - 1;
        const uint8_t* srow = src + static_cast<size_t>(sy) * src_w * 4;
        uint8_t* drow = out.data() + static_cast<size_t>(y) * dst_w * 4;
        for (uint32_t x = 0; x < dst_w; ++x) {
            uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * src_w) / dst_w);
            if (sx >= src_w) sx = src_w - 1;
            std::memcpy(drow + static_cast<size_t>(x) * 4, srow + static_cast<size_t>(sx) * 4, 4);
        }
    }
    return out;
}

std::vector<uint8_t> ResizeRgbaBilinearFallback(
    const uint8_t* src,
    uint32_t src_w,
    uint32_t src_h,
    uint32_t dst_w,
    uint32_t dst_h) {
    std::vector<uint8_t> out(static_cast<size_t>(dst_w) * dst_h * 4);
    float scale_x = static_cast<float>(src_w) / static_cast<float>(dst_w);
    float scale_y = static_cast<float>(src_h) / static_cast<float>(dst_h);
    for (uint32_t y = 0; y < dst_h; ++y) {
        float fy = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
        int32_t y0 = static_cast<int32_t>(std::floor(fy));
        float wy = fy - static_cast<float>(y0);
        int32_t y1 = std::min<int32_t>(y0 + 1, static_cast<int32_t>(src_h) - 1);
        y0 = std::clamp<int32_t>(y0, 0, static_cast<int32_t>(src_h) - 1);

        for (uint32_t x = 0; x < dst_w; ++x) {
            float fx = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
            int32_t x0 = static_cast<int32_t>(std::floor(fx));
            float wx = fx - static_cast<float>(x0);
            int32_t x1 = std::min<int32_t>(x0 + 1, static_cast<int32_t>(src_w) - 1);
            x0 = std::clamp<int32_t>(x0, 0, static_cast<int32_t>(src_w) - 1);

            const uint8_t* p00 = src + (static_cast<size_t>(y0) * src_w + static_cast<size_t>(x0)) * 4;
            const uint8_t* p10 = src + (static_cast<size_t>(y0) * src_w + static_cast<size_t>(x1)) * 4;
            const uint8_t* p01 = src + (static_cast<size_t>(y1) * src_w + static_cast<size_t>(x0)) * 4;
            const uint8_t* p11 = src + (static_cast<size_t>(y1) * src_w + static_cast<size_t>(x1)) * 4;
            uint8_t* dst = out.data() + (static_cast<size_t>(y) * dst_w + x) * 4;

            float w00 = (1.0f - wx) * (1.0f - wy);
            float w10 = wx * (1.0f - wy);
            float w01 = (1.0f - wx) * wy;
            float w11 = wx * wy;
            for (int c = 0; c < 4; ++c) {
                float v = p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11;
                dst[c] = static_cast<uint8_t>(std::clamp(static_cast<int>(v + 0.5f), 0, 255));
            }
        }
    }
    return out;
}

std::vector<uint8_t> ResizeRgba(
    const uint8_t* src,
    uint32_t src_w,
    uint32_t src_h,
    uint32_t dst_w,
    uint32_t dst_h,
    bool nearest_neighbor) {
    if (src_w == dst_w && src_h == dst_h) {
        return std::vector<uint8_t>(src, src + static_cast<size_t>(src_w) * src_h * 4);
    }
    if (nearest_neighbor) {
        return ResizeRgbaNearest(src, src_w, src_h, dst_w, dst_h);
    }
    // Premultiply alpha before Lanczos5 scaling when the image has transparency so alpha==0
    // background pixels cannot bleed dark halos into sharp sprite/cutout edges.
    bool has_alpha = false;
    size_t src_bytes = static_cast<size_t>(src_w) * src_h * 4;
    for (size_t i = 3; i < src_bytes; i += 4) {
        if (src[i] != 255) {
            has_alpha = true;
            break;
        }
    }

    std::vector<uint8_t> premul_src;
    const uint8_t* scale_src_ptr = src;
    if (has_alpha) {
        premul_src.resize(src_bytes);
        vImage_Buffer in_buf{
            .data = const_cast<uint8_t*>(src),
            .height = static_cast<vImagePixelCount>(src_h),
            .width = static_cast<vImagePixelCount>(src_w),
            .rowBytes = static_cast<size_t>(src_w) * 4,
        };
        vImage_Buffer pm_buf{
            .data = premul_src.data(),
            .height = static_cast<vImagePixelCount>(src_h),
            .width = static_cast<vImagePixelCount>(src_w),
            .rowBytes = static_cast<size_t>(src_w) * 4,
        };
        if (vImagePremultiplyData_RGBA8888(&in_buf, &pm_buf, kvImageDoNotTile) == kvImageNoError) {
            scale_src_ptr = premul_src.data();
        } else {
            has_alpha = false;
            premul_src.clear();
        }
    }

    std::vector<uint8_t> out(static_cast<size_t>(dst_w) * dst_h * 4);
    vImage_Buffer src_buf{
        .data = const_cast<uint8_t*>(scale_src_ptr),
        .height = static_cast<vImagePixelCount>(src_h),
        .width = static_cast<vImagePixelCount>(src_w),
        .rowBytes = static_cast<size_t>(src_w) * 4,
    };
    vImage_Buffer dst_buf{
        .data = out.data(),
        .height = static_cast<vImagePixelCount>(dst_h),
        .width = static_cast<vImagePixelCount>(dst_w),
        .rowBytes = static_cast<size_t>(dst_w) * 4,
    };
    vImage_Error err = vImageScale_ARGB8888(
        &src_buf,
        &dst_buf,
        nullptr,
        static_cast<vImage_Flags>(kvImageHighQualityResampling | kvImageDoNotTile));
    if (err == kvImageNoError) {
        if (has_alpha) {
            vImageUnpremultiplyData_RGBA8888(&dst_buf, &dst_buf, kvImageDoNotTile);
        }
        return out;
    }
    return ResizeRgbaBilinearFallback(src, src_w, src_h, dst_w, dst_h);
}

// Pads w x h RGBA8888 up to 4x4 block alignment matching resourcecompiler64.exe:
// - Right border (x in w..pw): replicates (w - 1, y) RGB with A = 0.
// - Bottom border (y in h..ph): memcpy's the entire padded row h - 1 verbatim.
struct PaddedRgba {
    size_t width = 0;
    size_t height = 0;
    const uint8_t* ptr = nullptr;
    std::vector<uint8_t> owned;
};

PaddedRgba PadRgbaForBlockCompression(const uint8_t* src, size_t w, size_t h) {
    size_t pw = ((w + 3) / 4) * 4;
    size_t ph = ((h + 3) / 4) * 4;
    if (pw == w && ph == h) {
        return PaddedRgba{w, h, src, {}};
    }
    std::vector<uint8_t> out(pw * ph * 4, 0);
    if (w == 0 || h == 0) {
        const uint8_t* p = out.data();
        return PaddedRgba{pw, ph, p, std::move(out)};
    }
    size_t row_bytes = pw * 4;
    for (size_t y = 0; y < h; ++y) {
        const uint8_t* s_row = src + y * w * 4;
        uint8_t* d_row = out.data() + y * row_bytes;
        std::memcpy(d_row, s_row, w * 4);
        const uint8_t* last_px = s_row + (w - 1) * 4;
        for (size_t x = w; x < pw; ++x) {
            uint8_t* dst = d_row + x * 4;
            dst[0] = last_px[0];
            dst[1] = last_px[1];
            dst[2] = last_px[2];
            dst[3] = 0;
        }
    }
    if (h < ph) {
        const uint8_t* last_valid_row = out.data() + (h - 1) * row_bytes;
        for (size_t y = h; y < ph; ++y) {
            std::memcpy(out.data() + y * row_bytes, last_valid_row, row_bytes);
        }
    }
    PaddedRgba res;
    res.width = pw;
    res.height = ph;
    res.owned = std::move(out);
    res.ptr = res.owned.data();
    return res;
}

DecodedTexImage DecodeSingleMipRgba(
    const TexFile& tex,
    const TexMip& mip,
    const std::string& label,
    bool* out_is_video = nullptr) {
    if (out_is_video) *out_is_video = false;

    std::vector<uint8_t> decompressed;
    const uint8_t* raw_ptr = mip.data.data();
    size_t raw_size = mip.data.size();
    if (mip.is_lz4) {
        decompressed = Lz4DecompressBlock(mip.data.data(), mip.data.size(), mip.uncompressed_size, label);
        raw_ptr = decompressed.data();
        raw_size = decompressed.size();
    }

    SniffedContainer sniffed = SniffEmbeddedContainer(raw_ptr, raw_size);
    if (sniffed == SniffedContainer::Video) {
        if (out_is_video) *out_is_video = true;
        return {};
    }

    if (tex.free_image_format != -1 || sniffed == SniffedContainer::Image) {
        if (raw_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
        }
        int iw = 0, ih = 0, comp = 0;
        stbi_uc* pixels = stbi_load_from_memory(
            raw_ptr,
            static_cast<int>(raw_size),
            &iw,
            &ih,
            &comp,
            4);
        if (!pixels || iw <= 0 || ih <= 0) {
            if (pixels) stbi_image_free(pixels);
            throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
        }
        uint64_t byte_count = static_cast<uint64_t>(iw) * static_cast<uint64_t>(ih) * 4ULL;
        if (byte_count > kMaxRgbaImageBytes) {
            stbi_image_free(pixels);
            throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
        }
        DecodedTexImage res;
        res.width = static_cast<uint32_t>(iw);
        res.height = static_cast<uint32_t>(ih);
        res.rgba.assign(pixels, pixels + static_cast<size_t>(byte_count));
        stbi_image_free(pixels);
        return res;
    }

    if (!IsKnownTexFormat(tex.header.format_raw)) {
        throw ConvertError(
            MIRAGE_MOBILE_UNSUPPORTED_TEXTURE_FORMAT,
            label,
            static_cast<int32_t>(tex.header.format_raw));
    }

    TexFormat fmt = static_cast<TexFormat>(tex.header.format_raw);
    size_t mw = mip.width;
    size_t mh = mip.height;
    std::vector<uint8_t> rgba;
    switch (fmt) {
        case TexFormat::Rgba8888:
            rgba = DecodeRgba8888(raw_ptr, raw_size, mw, mh, label);
            break;
        case TexFormat::Rgb888:
            rgba = DecodeRgb888(raw_ptr, raw_size, mw, mh, label);
            break;
        case TexFormat::Rg88:
            rgba = DecodeRg88(raw_ptr, raw_size, mw, mh, label);
            break;
        case TexFormat::R8:
            rgba = DecodeR8(raw_ptr, raw_size, mw, mh, label);
            break;
        case TexFormat::Dxt1:
            rgba = DecodeBc1(raw_ptr, raw_size, mw, mh, false, label);
            break;
        case TexFormat::Dxt1a:
            rgba = DecodeBc1(raw_ptr, raw_size, mw, mh, true, label);
            break;
        case TexFormat::Dxt3:
            rgba = DecodeBc2(raw_ptr, raw_size, mw, mh, label);
            break;
        case TexFormat::Dxt5:
            rgba = DecodeBc3(raw_ptr, raw_size, mw, mh, label);
            break;
        case TexFormat::Etc2Rgb8:
            rgba = DecodeEtc2Rgb8(raw_ptr, raw_size, mw, mh, label);
            break;
        case TexFormat::Etc2Rgba8:
            rgba = DecodeEtc2Rgba8(raw_ptr, raw_size, mw, mh, label);
            break;
        default:
            throw ConvertError(
                MIRAGE_MOBILE_UNSUPPORTED_TEXTURE_FORMAT,
                label,
                static_cast<int32_t>(tex.header.format_raw));
    }
    return DecodedTexImage{mip.width, mip.height, std::move(rgba)};
}

} // namespace

TexHeader TexFile::ParseHeader(const uint8_t* bytes, size_t size, const std::string& label) {
    size_t cursor = 0;
    TexHeader hdr;
    hdr.texv_version = ReadMagic9(bytes, size, cursor, "TEXV", label);
    if (hdr.texv_version < 1 || hdr.texv_version > 5) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    hdr.texi_version = ReadMagic9(bytes, size, cursor, "TEXI", label);
    if (hdr.texi_version != 1) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    hdr.format_raw = ReadU32(bytes, size, cursor, label);
    hdr.flags = ReadU32(bytes, size, cursor, label);
    hdr.texture_width = ReadU32(bytes, size, cursor, label);
    hdr.texture_height = ReadU32(bytes, size, cursor, label);
    hdr.image_width = ReadU32(bytes, size, cursor, label);
    hdr.image_height = ReadU32(bytes, size, cursor, label);
    if (hdr.flags & kTexFlagIs3D) {
        hdr.depth = ReadU32(bytes, size, cursor, label);
    }
    hdr.unk_int0 = ReadU32(bytes, size, cursor, label);
    return hdr;
}

TexFile TexFile::FromBytes(const uint8_t* bytes, size_t size, const std::string& label) {
    if (!bytes || size < 18) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    size_t cursor = 0;
    TexFile tex;
    tex.header.texv_version = ReadMagic9(bytes, size, cursor, "TEXV", label);
    if (tex.header.texv_version < 1 || tex.header.texv_version > 5) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    tex.header.texi_version = ReadMagic9(bytes, size, cursor, "TEXI", label);
    if (tex.header.texi_version != 1) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    tex.header.format_raw = ReadU32(bytes, size, cursor, label);
    tex.header.flags = ReadU32(bytes, size, cursor, label);
    tex.header.texture_width = ReadU32(bytes, size, cursor, label);
    tex.header.texture_height = ReadU32(bytes, size, cursor, label);
    tex.header.image_width = ReadU32(bytes, size, cursor, label);
    tex.header.image_height = ReadU32(bytes, size, cursor, label);
    if (tex.header.flags & kTexFlagIs3D) {
        tex.header.depth = ReadU32(bytes, size, cursor, label);
    }
    tex.header.unk_int0 = ReadU32(bytes, size, cursor, label);

    tex.texb_version = ReadMagic9(bytes, size, cursor, "TEXB", label);
    if (tex.texb_version < 1 || tex.texb_version > 4) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    uint32_t image_count = ReadU32(bytes, size, cursor, label);
    if (image_count > 4096) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    tex.free_image_format = (tex.texb_version >= 3) ? ReadI32(bytes, size, cursor, label) : -1;
    uint32_t param_count = (tex.texb_version >= 4) ? ReadU32(bytes, size, cursor, label) : 0;
    if (param_count > 4096) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    for (uint32_t p = 0; p < param_count; ++p) {
        (void)ReadU32(bytes, size, cursor, label);
        (void)ReadU32(bytes, size, cursor, label);
        (void)ReadU32(bytes, size, cursor, label);
        SkipCString(bytes, size, cursor, label);
    }

    tex.images.reserve(image_count);
    for (uint32_t img_i = 0; img_i < image_count; ++img_i) {
        uint32_t mip_count = ReadU32(bytes, size, cursor, label);
        if (mip_count == 0 || mip_count > 64) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
        }
        TexImage img;
        img.mips.reserve(mip_count);
        for (uint32_t m = 0; m < mip_count; ++m) {
            TexMip mip;
            mip.width = ReadU32(bytes, size, cursor, label);
            mip.height = ReadU32(bytes, size, cursor, label);
            if (mip.width == 0 || mip.height == 0) {
                throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
            }
            if (tex.header.flags & kTexFlagIs3D) {
                mip.depth = ReadU32(bytes, size, cursor, label);
            }
            if (tex.texb_version >= 2) {
                mip.is_lz4 = ReadU32(bytes, size, cursor, label) != 0;
                mip.uncompressed_size = ReadU32(bytes, size, cursor, label);
            }
            uint32_t byte_count = ReadU32(bytes, size, cursor, label);
            if (cursor + byte_count > size || byte_count == 0) {
                throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
            }
            mip.data.assign(bytes + cursor, bytes + cursor + byte_count);
            cursor += byte_count;
            img.mips.push_back(std::move(mip));
        }
        tex.images.push_back(std::move(img));
    }

    if (tex.header.flags & kTexFlagIsGif) {
        if (cursor + 9 <= size && std::memcmp(bytes + cursor, "TEXS", 4) != 0) {
            const uint8_t needle[4] = {'T', 'E', 'X', 'S'};
            const uint8_t* found = std::search(bytes + cursor, bytes + size, needle, needle + 4);
            if (found != bytes + size) {
                cursor = static_cast<size_t>(found - bytes);
            }
        }
        uint32_t texs_ver = ReadMagic9(bytes, size, cursor, "TEXS", label);
        if (texs_ver < 1 || texs_ver > 3) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
        }
        uint32_t frame_count = ReadU32(bytes, size, cursor, label);
        if (frame_count > 1000000) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
        }
        TexSpriteSection sp;
        sp.version = texs_ver;
        if (texs_ver >= 3) {
            sp.gif_width = ReadU32(bytes, size, cursor, label);
            sp.gif_height = ReadU32(bytes, size, cursor, label);
        } else {
            sp.gif_width = tex.header.image_width;
            sp.gif_height = tex.header.image_height;
        }
        sp.frames.reserve(frame_count);
        for (uint32_t f = 0; f < frame_count; ++f) {
            TexSpriteFrame fr;
            fr.image_id = ReadU32(bytes, size, cursor, label);
            fr.frametime = ReadF32(bytes, size, cursor, label);
            if (texs_ver == 1) {
                fr.x = static_cast<float>(ReadI32(bytes, size, cursor, label));
                fr.y = static_cast<float>(ReadI32(bytes, size, cursor, label));
                fr.width = static_cast<float>(ReadI32(bytes, size, cursor, label));
                fr.width_y = static_cast<float>(ReadI32(bytes, size, cursor, label));
                fr.height_x = static_cast<float>(ReadI32(bytes, size, cursor, label));
                fr.height = static_cast<float>(ReadI32(bytes, size, cursor, label));
            } else {
                fr.x = ReadF32(bytes, size, cursor, label);
                fr.y = ReadF32(bytes, size, cursor, label);
                fr.width = ReadF32(bytes, size, cursor, label);
                fr.width_y = ReadF32(bytes, size, cursor, label);
                fr.height_x = ReadF32(bytes, size, cursor, label);
                fr.height = ReadF32(bytes, size, cursor, label);
            }
            sp.frames.push_back(fr);
        }
        tex.sprites = std::move(sp);
    }

    return tex;
}

std::vector<uint8_t> TexFile::ToBytes(const std::string& label) const {
    std::vector<uint8_t> out;
    out.reserve(1024);
    AppendStamp9(out, "TEXV0005");
    AppendStamp9(out, "TEXI0001");

    AppendU32LE(out, header.format_raw);
    AppendU32LE(out, header.flags);
    AppendU32LE(out, header.texture_width);
    AppendU32LE(out, header.texture_height);
    AppendU32LE(out, header.image_width);
    AppendU32LE(out, header.image_height);
    if (header.flags & kTexFlagIs3D) {
        AppendU32LE(out, header.depth.value_or(1));
    }
    AppendU32LE(out, header.unk_int0);

    AppendStamp9(out, "TEXB0004");
    AppendU32LE(out, static_cast<uint32_t>(images.size()));
    AppendI32LE(out, free_image_format);
    AppendU32LE(out, 0); // param_count = 0

    for (const auto& img : images) {
        AppendU32LE(out, static_cast<uint32_t>(img.mips.size()));
        for (const auto& mip : img.mips) {
            AppendU32LE(out, mip.width);
            AppendU32LE(out, mip.height);
            if (header.flags & kTexFlagIs3D) {
                AppendU32LE(out, mip.depth.value_or(1));
            }
            AppendU32LE(out, mip.is_lz4 ? 1U : 0U);
            AppendU32LE(out, mip.uncompressed_size);
            if (mip.data.size() > std::numeric_limits<uint32_t>::max()) {
                throw ConvertError(MIRAGE_MOBILE_WALLPAPER_TOO_LARGE, label);
            }
            AppendU32LE(out, static_cast<uint32_t>(mip.data.size()));
            out.insert(out.end(), mip.data.begin(), mip.data.end());
        }
    }

    if ((header.flags & kTexFlagIsGif) && sprites.has_value()) {
        const auto& sp = *sprites;
        AppendStamp9(out, "TEXS0003");
        AppendU32LE(out, static_cast<uint32_t>(sp.frames.size()));
        AppendU32LE(out, sp.gif_width);
        AppendU32LE(out, sp.gif_height);
        for (const auto& fr : sp.frames) {
            AppendU32LE(out, fr.image_id);
            AppendF32LE(out, fr.frametime);
            AppendF32LE(out, fr.x);
            AppendF32LE(out, fr.y);
            AppendF32LE(out, fr.width);
            AppendF32LE(out, fr.width_y);
            AppendF32LE(out, fr.height_x);
            AppendF32LE(out, fr.height);
        }
    }

    return out;
}

DecodedTexImage DecodeTexMip0Rgba(
    const TexFile& tex,
    size_t image_index,
    bool crop_to_image_dims,
    const std::string& label) {
    if (image_index >= tex.images.size() || tex.images[image_index].mips.empty()) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    DecodedTexImage res = DecodeSingleMipRgba(tex, tex.images[image_index].mips[0], label);
    if (crop_to_image_dims && tex.header.image_width > 0 && tex.header.image_height > 0) {
        uint32_t cw = res.width;
        uint32_t ch = res.height;
        if (tex.header.texture_width > tex.header.image_width ||
            tex.header.texture_height > tex.header.image_height) {
            uint64_t tw = std::max<uint32_t>(tex.header.texture_width, 1);
            uint64_t th = std::max<uint32_t>(tex.header.texture_height, 1);
            uint32_t est_w = static_cast<uint32_t>(std::max<uint64_t>((static_cast<uint64_t>(res.width) * tex.header.image_width) / tw, 1));
            uint32_t est_h = static_cast<uint32_t>(std::max<uint64_t>((static_cast<uint64_t>(res.height) * tex.header.image_height) / th, 1));
            cw = std::min(est_w, res.width);
            ch = std::min(est_h, res.height);
        } else if (res.width > tex.header.image_width || res.height > tex.header.image_height) {
            cw = std::min(tex.header.image_width, res.width);
            ch = std::min(tex.header.image_height, res.height);
        }
        if (cw < res.width || ch < res.height) {
            res.rgba = CropRgbaTopLeft(res.rgba.data(), res.width, cw, ch);
            res.width = cw;
            res.height = ch;
        }
    }
    return res;
}

TexTranscodeResult TranscodeTexBytesForMobile(
    const uint8_t* data,
    size_t size,
    const std::string& label,
    const TexTranscodeOptions& opts) {
    CheckCancelled(opts.cancel_flag);
    TexFile tex = TexFile::FromBytes(data, size, label);
    uint32_t orig_fmt_raw = tex.header.format_raw;

    auto skip = [&](const char* reason) {
        TexTranscodeResult r;
        r.transcoded = false;
        r.from_format = orig_fmt_raw;
        r.to_format = orig_fmt_raw;
        r.skip_reason = reason;
        return r;
    };

    // 1. Exact resourcecompiler64.exe skip rules.
    if (tex.header.flags & kTexFlagIsVideoTexture) {
        return skip("video_texture");
    }
    if (tex.header.depth.value_or(1) > 1 || (tex.header.flags & kTexFlagIs3D)) {
        return skip("volume_3d_lut");
    }
    if (!IsKnownTexFormat(orig_fmt_raw)) {
        return skip("unknown_format");
    }

    TexFormat fmt = static_cast<TexFormat>(orig_fmt_raw);
    switch (fmt) {
        case TexFormat::Rg88:
        case TexFormat::R8:
            return skip("single_or_dual_channel_mask");
        case TexFormat::Rgba16161616f:
        case TexFormat::Rg1616f:
        case TexFormat::R16f:
            return skip("hdr_float_texture");
        case TexFormat::Rgb888:
            if (!opts.force_etc2 && tex.free_image_format == -1) {
                return skip("rgb888_without_force");
            }
            break;
        case TexFormat::Etc2Rgb8:
        case TexFormat::Etc2Rgba8:
            return skip("already_etc2");
        default:
            break;
    }

    if (tex.images.empty()) {
        return skip("empty_images");
    }

    // 2. Clear bit 30 on mobile transcode and set NO_MIPMAPS when primary image has < 2 mips.
    tex.header.flags &= ~kTexFlagClearOnMobile;
    if (tex.images[0].mips.size() < 2) {
        tex.header.flags |= kTexFlagNoMipmaps;
    }

    bool was_dxt = IsDxtFormat(fmt);
    uint32_t shrink_req = std::clamp(opts.shrink, 1U, 8U);
    bool has_sprites = tex.sprites.has_value() && !tex.sprites->frames.empty();
    bool use_etc2 = opts.force_etc2 || was_dxt;

    uint32_t orig_img_w = tex.header.image_width;
    uint32_t orig_img_h = tex.header.image_height;
    uint32_t final_tex_w = tex.header.texture_width;
    uint32_t final_tex_h = tex.header.texture_height;
    if (tex.free_image_format != -1 && orig_img_w > 0 && orig_img_h > 0) {
        final_tex_w = orig_img_w;
        final_tex_h = orig_img_h;
    }

    uint32_t actual_shrink = 1;
    std::vector<uint32_t> frame_shrinks;
    frame_shrinks.reserve(tex.images.size());
    bool any_lz4 = false;
    bool all_opaque = true;

    struct PreparedFrame {
        uint32_t w = 0;
        uint32_t h = 0;
        std::vector<uint8_t> rgba;
    };
    const bool two_pass_opacity = (opts.prefer_rgb8_when_opaque && use_etc2);
    TexFormat target_fmt = use_etc2 ? TexFormat::Etc2Rgba8 : TexFormat::Rgba8888;
    std::vector<PreparedFrame> prepared;
    if (two_pass_opacity) prepared.reserve(tex.images.size());
    std::vector<TexImage> new_images;
    new_images.reserve(tex.images.size());

    auto encode_and_store_frame = [&](size_t idx, uint32_t shrunk_w, uint32_t shrunk_h,
                                      std::vector<uint8_t>& frame_rgba, TexFormat fmt_out) {
        CheckCancelled(opts.cancel_flag);
        uint32_t mip_w = shrunk_w;
        uint32_t mip_h = shrunk_h;
        std::vector<uint8_t> raw_encoded;

        if (fmt_out == TexFormat::Etc2Rgba8) {
            auto padded = PadRgbaForBlockCompression(frame_rgba.data(), shrunk_w, shrunk_h);
            mip_w = static_cast<uint32_t>(padded.width);
            mip_h = static_cast<uint32_t>(padded.height);
            raw_encoded = EncodeEtc2Rgba8(padded.ptr, padded.width, padded.height, opts.inner_threads, opts.cancel_flag);
        } else if (fmt_out == TexFormat::Etc2Rgb8) {
            auto padded = PadRgbaForBlockCompression(frame_rgba.data(), shrunk_w, shrunk_h);
            mip_w = static_cast<uint32_t>(padded.width);
            mip_h = static_cast<uint32_t>(padded.height);
            raw_encoded = EncodeEtc2Rgb8(padded.ptr, padded.width, padded.height, opts.inner_threads, opts.cancel_flag);
        } else {
            raw_encoded = std::move(frame_rgba);
        }
        std::vector<uint8_t>().swap(frame_rgba);

        // Exact TEXI0001 virtual texture_width / texture_height calculation.
        if (idx == 0 && use_etc2 && !has_sprites) {
            if (actual_shrink > 1) {
                final_tex_w = static_cast<uint32_t>(
                    (static_cast<float>(mip_w) * static_cast<float>(orig_img_w)) / static_cast<float>(shrunk_w));
                final_tex_h = static_cast<uint32_t>(
                    (static_cast<float>(mip_h) * static_cast<float>(orig_img_h)) / static_cast<float>(shrunk_h));
            } else {
                final_tex_w = mip_w;
                final_tex_h = mip_h;
            }
        }

        uint32_t uncomp_len = static_cast<uint32_t>(raw_encoded.size());
        auto [payload, is_lz4] = CompressTexMipIfBeneficial(raw_encoded, label);
        std::vector<uint8_t>().swap(raw_encoded);
        any_lz4 = any_lz4 || is_lz4;

        TexMip out_mip;
        out_mip.width = mip_w;
        out_mip.height = mip_h;
        out_mip.is_lz4 = is_lz4;
        out_mip.uncompressed_size = uncomp_len;
        out_mip.data = std::move(payload);

        TexImage out_img;
        out_img.mips.push_back(std::move(out_mip));
        new_images.push_back(std::move(out_img));
    };

    for (size_t img_idx = 0; img_idx < tex.images.size(); ++img_idx) {
        CheckCancelled(opts.cancel_flag);
        const auto& mips = tex.images[img_idx].mips;
        if (mips.empty()) {
            throw ConvertError(MIRAGE_MOBILE_UNSUPPORTED_TEXTURE_LAYOUT, label);
        }
        const TexMip& base_mip = mips[0];

        // Determine logical active dimensions before shrink.
        uint32_t base_active_w = (img_idx == 0 && orig_img_w > 0) ? orig_img_w : base_mip.width;
        uint32_t base_active_h = (img_idx == 0 && orig_img_h > 0) ? orig_img_h : base_mip.height;
        uint32_t frame_shrink = (shrink_req > 1 && base_active_w > 128 && base_active_h > 128)
            ? shrink_req : 1U;
        uint32_t target_w = std::max<uint32_t>(base_active_w / frame_shrink, 1U);
        uint32_t target_h = std::max<uint32_t>(base_active_h / frame_shrink, 1U);

        // Reuse the smallest existing mip that still supplies all requested pixels (never upscale).
        size_t chosen_mip_idx = 0;
        uint32_t chosen_crop_w = base_active_w;
        uint32_t chosen_crop_h = base_active_h;
        if (frame_shrink > 1 && mips.size() > 1 && !opts.pixel_art) {
            for (size_t mi = 1; mi < mips.size(); ++mi) {
                const TexMip& cand = mips[mi];
                uint32_t cw = static_cast<uint32_t>(std::max<uint64_t>(
                    (static_cast<uint64_t>(base_active_w) * cand.width) / std::max<uint32_t>(base_mip.width, 1U),
                    1ULL));
                uint32_t ch = static_cast<uint32_t>(std::max<uint64_t>(
                    (static_cast<uint64_t>(base_active_h) * cand.height) / std::max<uint32_t>(base_mip.height, 1U),
                    1ULL));
                if (cw >= target_w && ch >= target_h &&
                    cw <= chosen_crop_w && ch <= chosen_crop_h &&
                    cw <= cand.width && ch <= cand.height) {
                    chosen_mip_idx = mi;
                    chosen_crop_w = cw;
                    chosen_crop_h = ch;
                }
            }
        }

        bool is_embedded_video = false;
        DecodedTexImage dec = DecodeSingleMipRgba(tex, mips[chosen_mip_idx], label, &is_embedded_video);
        if (is_embedded_video) {
            return skip("video_texture");
        }

        uint32_t w = dec.width;
        uint32_t h = dec.height;
        std::vector<uint8_t> rgba = std::move(dec.rgba);

        if (img_idx == 0 && chosen_mip_idx == 0) {
            if (orig_img_w == 0 || orig_img_h == 0) {
                orig_img_w = w;
                orig_img_h = h;
                base_active_w = w;
                base_active_h = h;
                frame_shrink = (shrink_req > 1 && w > 128 && h > 128) ? shrink_req : 1U;
                target_w = std::max<uint32_t>(w / frame_shrink, 1U);
                target_h = std::max<uint32_t>(h / frame_shrink, 1U);
                chosen_crop_w = w;
                chosen_crop_h = h;
            }
            if (final_tex_w == 0 || final_tex_h == 0 || tex.free_image_format != -1) {
                final_tex_w = orig_img_w;
                final_tex_h = orig_img_h;
            }
        }

        // Crop padded mip to active image area.
        uint32_t crop_w = std::min(chosen_crop_w, w);
        uint32_t crop_h = std::min(chosen_crop_h, h);
        if (crop_w > 0 && crop_h > 0 && (w > crop_w || h > crop_h)) {
            rgba = CropRgbaTopLeft(rgba.data(), w, crop_w, crop_h);
            w = crop_w;
            h = crop_h;
            if (img_idx == 0) {
                final_tex_w = (chosen_mip_idx == 0) ? crop_w : orig_img_w;
                final_tex_h = (chosen_mip_idx == 0) ? crop_h : orig_img_h;
            }
        }
        // Release raw source mip payload immediately to keep peak working-set RSS minimal.
        std::vector<TexMip>().swap(tex.images[img_idx].mips);

        // Recompute exact target dimensions if embedded image dimensions differed from mip header.
        if (chosen_mip_idx == 0 && (w != base_active_w || h != base_active_h)) {
            if (img_idx == 0 && (orig_img_w > w || orig_img_h > h)) {
                orig_img_w = w;
                orig_img_h = h;
                final_tex_w = w;
                final_tex_h = h;
            }
            frame_shrink = (shrink_req > 1 && w > 128 && h > 128) ? shrink_req : 1U;
            target_w = std::max<uint32_t>(w / frame_shrink, 1U);
            target_h = std::max<uint32_t>(h / frame_shrink, 1U);
        }

        if (w != target_w || h != target_h) {
            rgba = ResizeRgba(rgba.data(), w, h, target_w, target_h, opts.pixel_art);
            w = target_w;
            h = target_h;
        }

        if (img_idx == 0) {
            actual_shrink = frame_shrink;
        }
        frame_shrinks.push_back(frame_shrink);

        if (two_pass_opacity) {
            if (all_opaque) {
                for (size_t i = 3; i < rgba.size(); i += 4) {
                    if (rgba[i] != 255) {
                        all_opaque = false;
                        break;
                    }
                }
            }
            prepared.push_back(PreparedFrame{w, h, std::move(rgba)});
        } else {
            encode_and_store_frame(img_idx, w, h, rgba, target_fmt);
        }
    }

    if (two_pass_opacity) {
        target_fmt = all_opaque ? TexFormat::Etc2Rgb8 : TexFormat::Etc2Rgba8;
        for (size_t idx = 0; idx < prepared.size(); ++idx) {
            encode_and_store_frame(idx, prepared[idx].w, prepared[idx].h, prepared[idx].rgba, target_fmt);
        }
        std::vector<PreparedFrame>().swap(prepared);
    }

    tex.header.texv_version = 5;
    tex.header.texi_version = 1;
    tex.header.format_raw = static_cast<uint32_t>(target_fmt);
    tex.header.texture_width = final_tex_w;
    tex.header.texture_height = final_tex_h;
    tex.header.image_width = orig_img_w;
    tex.header.image_height = orig_img_h;
    tex.texb_version = 4;
    tex.free_image_format = -1;
    tex.images = std::move(new_images);

    // Scale sprite sheet per-frame coordinates when shrunk; gif_width/gif_height stay unchanged.
    if (tex.sprites.has_value()) {
        auto& sp = *tex.sprites;
        sp.version = 3;
        for (auto& fr : sp.frames) {
            uint32_t fs = (fr.image_id < frame_shrinks.size())
                ? frame_shrinks[fr.image_id]
                : actual_shrink;
            if (fs > 1) {
                float inv = 1.0f / static_cast<float>(fs);
                fr.x *= inv;
                fr.y *= inv;
                fr.width *= inv;
                fr.width_y *= inv;
                fr.height_x *= inv;
                fr.height *= inv;
            }
        }
    }

    TexTranscodeResult result;
    result.transcoded = true;
    result.from_format = orig_fmt_raw;
    result.to_format = static_cast<uint32_t>(target_fmt);
    result.actual_shrink = actual_shrink;
    result.lz4_compressed = any_lz4;
    result.bytes = tex.ToBytes(label);
    return result;
}

} // namespace mirage::mobile

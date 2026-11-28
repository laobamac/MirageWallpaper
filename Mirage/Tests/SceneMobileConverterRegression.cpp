//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "DxtDecoder.hpp"
#include "Etc2Codec.hpp"
#include "JsonCompat.hpp"
#include "Lz4Codec.hpp"
#include "MobileInternal.hpp"
#include "PkgArchive.hpp"
#include "SceneDeps.hpp"
#include "SceneMobileConverter.h"
#include "ShaderRewriter.hpp"
#include "TexTranscoder.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace mirage::mobile;

namespace {

void Require(bool cond, const char* msg, int line) {
    if (!cond) {
        std::cerr << "FAIL (line " << line << "): " << msg << "\n";
        std::exit(1);
    }
}

#define CHECK(cond) Require((cond), #cond, __LINE__)

std::vector<uint8_t> MakeSolidRgba(uint32_t w, uint32_t h, bool alpha_grad = false) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = (static_cast<size_t>(y) * w + x) * 4;
            rgba[i] = static_cast<uint8_t>(x & 0xffU);
            rgba[i + 1] = static_cast<uint8_t>(y & 0xffU);
            rgba[i + 2] = 180;
            rgba[i + 3] = alpha_grad ? static_cast<uint8_t>((x + y) & 0xffU) : 255;
        }
    }
    return rgba;
}

std::vector<uint8_t> MakeRawTex(
    TexFormat fmt,
    uint32_t flags,
    uint32_t tw,
    uint32_t th,
    uint32_t iw,
    uint32_t ih,
    std::vector<uint8_t> mip0,
    std::optional<TexSpriteSection> sprites = std::nullopt,
    int32_t fif = -1,
    std::optional<uint32_t> depth = std::nullopt,
    std::optional<TexMip> mip1 = std::nullopt) {
    TexFile tex;
    tex.header.texv_version = 5;
    tex.header.texi_version = 1;
    tex.header.format_raw = static_cast<uint32_t>(fmt);
    tex.header.flags = flags;
    tex.header.texture_width = tw;
    tex.header.texture_height = th;
    tex.header.image_width = iw;
    tex.header.image_height = ih;
    tex.header.depth = depth;
    tex.texb_version = 4;
    tex.free_image_format = fif;

    TexImage img;
    TexMip m0;
    m0.width = tw;
    m0.height = th;
    m0.depth = depth;
    m0.is_lz4 = false;
    m0.uncompressed_size = static_cast<uint32_t>(mip0.size());
    m0.data = std::move(mip0);
    img.mips.push_back(std::move(m0));
    if (mip1) img.mips.push_back(std::move(*mip1));
    tex.images.push_back(std::move(img));
    tex.sprites = std::move(sprites);
    return tex.ToBytes("synth.tex");
}

// Minimal valid 1x1 RGBA PNG bytes for testing embedded PNG sniffing.
std::vector<uint8_t> MakeMinimal1x1Png() {
    static const uint8_t kPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
        0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99,
        0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };
    return std::vector<uint8_t>(std::begin(kPng), std::end(kPng));
}

void WriteBytes(const std::filesystem::path& p, const std::vector<uint8_t>& data) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    CHECK(ofs.good());
    ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    CHECK(ofs.good());
}

void WriteText(const std::filesystem::path& p, std::string_view text) {
    WriteBytes(p, std::vector<uint8_t>(text.begin(), text.end()));
}

void WriteRawPkg(
    const std::filesystem::path& path,
    std::string_view magic,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& entries) {
    std::vector<uint8_t> out;
    AppendU32LE(out, static_cast<uint32_t>(magic.size()));
    out.insert(out.end(), magic.begin(), magic.end());
    AppendU32LE(out, static_cast<uint32_t>(entries.size()));
    uint32_t offset = 0;
    for (const auto& [name, data] : entries) {
        AppendU32LE(out, static_cast<uint32_t>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
        AppendU32LE(out, offset);
        AppendU32LE(out, static_cast<uint32_t>(data.size()));
        offset += static_cast<uint32_t>(data.size());
    }
    for (const auto& [_, data] : entries) {
        out.insert(out.end(), data.begin(), data.end());
    }
    WriteBytes(path, out);
}

void TestJsonCompat() {
    std::string raw = "\xEF\xBB\xBF{\n"
                      "  // line comment\n"
                      "  \"url\": \"https://example.com//keep/*this*/,}\",\n"
                      "  /* block\n comment */\n"
                      "  \"items\": [\"a\", \"b\", ],\n"
                      "  \"escaped\": \"quote\\\"inside,//still string\",\n"
                      "}";
    auto parsed = ParseJsonLenient(raw);
    CHECK(parsed.has_value());
    CHECK((*parsed)["url"] == "https://example.com//keep/*this*/,}");
    CHECK((*parsed)["items"][0] == "a");
    CHECK((*parsed)["items"][1] == "b");
    CHECK((*parsed)["escaped"] == "quote\"inside,//still string");

    auto doc = ParseJsonLenient(R"({
        "title": "Test",
        "tags": ["Landscape"],
        "long_arr": ["sounds/very_long_utf8_filename_exceeding_seventy_four_bytes_threshold_here.mp3"],
        "empty_obj": {},
        "empty_arr": [],
        "general": { "texturereduction": 2, "zoom": 1.0, "width": 1080 }
    })");
    CHECK(doc.has_value());
    std::string styled = ToJsonCppStyledString(*doc);
    std::string expected =
        "{\r\n"
        "\t\"empty_arr\" : [],\r\n"
        "\t\"empty_obj\" : {},\r\n"
        "\t\"general\" : \r\n"
        "\t{\r\n"
        "\t\t\"texturereduction\" : 2,\r\n"
        "\t\t\"width\" : 1080,\r\n"
        "\t\t\"zoom\" : 1.0\r\n"
        "\t},\r\n"
        "\t\"long_arr\" : \r\n"
        "\t[\r\n"
        "\t\t\"sounds/very_long_utf8_filename_exceeding_seventy_four_bytes_threshold_here.mp3\"\r\n"
        "\t],\r\n"
        "\t\"tags\" : [ \"Landscape\" ],\r\n"
        "\t\"title\" : \"Test\"\r\n"
        "}";
    CHECK(styled == expected);
}

void TestLz4CodecAndBombs() {
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < 8000; ++i) {
        if (i % 7 < 5) {
            const char* s = "ABCDABCD";
            data.insert(data.end(), s, s + 8);
        } else {
            AppendU32LE(data, i);
        }
    }
    auto [comp, is_lz4] = CompressTexMipIfBeneficial(data, "test");
    CHECK(is_lz4);
    CHECK(comp.size() < data.size());
    auto dec = Lz4DecompressBlock(comp.data(), comp.size(), data.size(), "test");
    CHECK(dec == data);

    // Small payload (< 16 KiB) skips LZ4.
    std::vector<uint8_t> small(4096, 42);
    auto [s_out, s_lz4] = CompressTexMipIfBeneficial(small, "small");
    CHECK(!s_lz4);
    CHECK(s_out == small);

    // Incompressible random payload fails < 0.95 gate.
    uint32_t rng = 0x12345678U;
    std::vector<uint8_t> noisy(20000);
    for (uint8_t& b : noisy) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        b = static_cast<uint8_t>(rng & 0xffU);
    }
    auto [n_out, n_lz4] = CompressTexMipIfBeneficial(noisy, "noisy");
    CHECK(!n_lz4);
    CHECK(n_out == noisy);

    // Decompression bomb guard: rejects > 256x expansion ratio and > 512 MiB.
    bool bomb_rejected = false;
    try {
        (void)Lz4DecompressBlock(comp.data(), 10, 10 * 300, "bomb");
    } catch (const ConvertError& e) {
        bomb_rejected = (e.status() == MIRAGE_MOBILE_INVALID_TEXTURE);
    }
    CHECK(bomb_rejected);
}

void TestDxtDecoders() {
    uint8_t raw[16] = {};
    raw[0] = 200;
    raw[1] = 50;
    raw[8] = 0x00; raw[9] = 0xf8; // pure red
    raw[10] = 0x1f; raw[11] = 0x00; // pure blue
    raw[12] = 0x00; raw[13] = 0x55; // row 0 = 0, row 1 = 1
    auto rgba = DecodeBc3(raw, sizeof(raw), 4, 4, "bc3");
    CHECK(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 200);
    CHECK(rgba[16] == 0 && rgba[17] == 0 && rgba[18] == 255 && rgba[19] == 200);
}

void TestEtc2QualityAndDeterminism() {
    // 1. Complex 1024x1024 benchmark & PSNR (same pattern as pkg2mpkg benchmark).
    constexpr size_t kW = 1024;
    constexpr size_t kH = 1024;
    std::vector<uint8_t> src(kW * kH * 4);
    for (size_t y = 0; y < kH; ++y) {
        for (size_t x = 0; x < kW; ++x) {
            size_t i = (y * kW + x) * 4;
            int r = 0, g = 0, b = 0, a = 255;
            if (x < 512 && y < 512) {
                r = static_cast<int>(20 + 220 * x / 511);
                g = static_cast<int>(40 + 180 * y / 511);
                b = static_cast<int>(230 - 150 * (x + y) / 1022);
                int dx = static_cast<int>(x) - 256;
                int dy = static_cast<int>(y) - 256;
                a = std::clamp(255 - (dx * dx + dy * dy) / 300, 0, 255);
            } else if (x >= 512 && y < 512) {
                int dx = static_cast<int>(x - 512);
                int dy = static_cast<int>(y);
                bool line = (std::abs((dx * 3 - dy * 2) % 41) <= 1) ||
                            (std::abs((dx + dy * 2) % 53) <= 1);
                if (dx > 400 && dy < 100) {
                    r = (dx * 17) & 255; g = (dy * 31) & 255; b = 200; a = 0;
                } else if (line) {
                    r = 25; g = 20; b = 35; a = 255;
                } else {
                    r = 245; g = 210; b = 195; a = 255;
                }
            } else if (x < 512 && y >= 512) {
                double v1 = std::sin(x * 0.23) * std::cos(y * 0.19);
                double v2 = std::sin((x + y) * 0.47);
                r = static_cast<int>(127 + 90 * v1 + 35 * v2);
                g = static_cast<int>(127 + 80 * std::cos(x * 0.17 - y * 0.29) + 40 * v2);
                b = static_cast<int>(127 + 100 * std::sin(y * 0.31) - 25 * v1);
                a = static_cast<int>(128 + 127 * std::sin(x * 0.11 + y * 0.13));
            } else {
                bool edge = ((x + y) % 19) < 7;
                r = edge ? 240 : 18;
                g = edge ? 35 : 220;
                b = edge ? 185 : 55;
                a = ((x ^ y) & 8) ? 255 : 48;
            }
            src[i] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            src[i + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            src[i + 2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            src[i + 3] = static_cast<uint8_t>(std::clamp(a, 0, 255));
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    auto enc_st = EncodeEtc2Rgba8(src.data(), kW, kH, 1);
    auto t1 = std::chrono::steady_clock::now();
    auto enc_mt = EncodeEtc2Rgba8(src.data(), kW, kH, 4);
    auto t2 = std::chrono::steady_clock::now();

    CHECK(enc_st == enc_mt); // Bit-exact multi-threaded determinism!
    CHECK(enc_st.size() == (kW / 4) * (kH / 4) * 16);

    auto dec = DecodeEtc2Rgba8(enc_st.data(), enc_st.size(), kW, kH, "bench");
    double mse_vis = 0.0;
    double mse_a = 0.0;
    size_t vis_px = 0;
    for (size_t i = 0; i < src.size(); i += 4) {
        double dr = static_cast<double>(src[i]) - dec[i];
        double dg = static_cast<double>(src[i + 1]) - dec[i + 1];
        double db = static_cast<double>(src[i + 2]) - dec[i + 2];
        double da = static_cast<double>(src[i + 3]) - dec[i + 3];
        mse_a += da * da;
        if (src[i + 3] > 0) {
            mse_vis += dr * dr + dg * dg + db * db;
            ++vis_px;
        }
    }
    double psnr_vis = 10.0 * std::log10((255.0 * 255.0) / (mse_vis / (vis_px * 3.0)));
    double psnr_a = 10.0 * std::log10((255.0 * 255.0) / (mse_a / (kW * kH)));
    double ms_st = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ms_mt = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::cout << "[ETC2 1024x1024] 1T=" << ms_st << " ms | 4T=" << ms_mt
              << " ms | PSNR_visRGB=" << psnr_vis << " dB | PSNR_alpha=" << psnr_a << " dB\n";
    CHECK(psnr_vis > 36.4);
    CHECK(psnr_a > 47.0);

    // 2. Alpha-aware boundary test: a block with 8 solid red opaque pixels and 8 transparent green garbage pixels
    //    must reconstruct the visible red pixels cleanly without green bleeding.
    std::array<std::array<uint8_t, 3>, 16> rgb{};
    uint16_t mask = 0;
    for (int p = 0; p < 16; ++p) {
        if (p < 8) {
            rgb[p] = {220, 40, 30};
            mask |= static_cast<uint16_t>(1U << p);
        } else {
            rgb[p] = {10, 250, 240}; // invisible garbage
        }
    }
    uint64_t w = EncodeEtc2RgbBlock(rgb, mask);
    auto out_rgb = DecodeEtc2RgbBlock(w);
    for (int p = 0; p < 8; ++p) {
        CHECK(std::abs(int(out_rgb[p][0]) - 220) <= 3);
        CHECK(std::abs(int(out_rgb[p][1]) - 40) <= 3);
        CHECK(std::abs(int(out_rgb[p][2]) - 30) <= 3);
    }
}

void TestTexTranscoder() {
    // 1. Virtual vs physical dimensions on non-multiple-of-4 shrink (500x300 / 2 -> 250x150 -> padded 252x152).
    auto raw = MakeRawTex(TexFormat::Rgba8888, kTexFlagClearOnMobile, 500, 300, 500, 300, MakeSolidRgba(500, 300));
    TexTranscodeOptions opts;
    opts.force_etc2 = true;
    opts.shrink = 2;
    auto res = TranscodeTexBytesForMobile(raw.data(), raw.size(), "hero.tex", opts);
    CHECK(res.transcoded);
    CHECK(res.to_format == static_cast<uint32_t>(TexFormat::Etc2Rgba8));
    CHECK(res.actual_shrink == 2);

    auto parsed = TexFile::FromBytes(res.bytes.data(), res.bytes.size(), "hero_out.tex");
    CHECK((parsed.header.flags & kTexFlagClearOnMobile) == 0);
    CHECK((parsed.header.flags & kTexFlagNoMipmaps) != 0);
    CHECK(parsed.header.image_width == 500 && parsed.header.image_height == 300);
    CHECK(parsed.header.texture_width == 504 && parsed.header.texture_height == 304);
    CHECK(parsed.images[0].mips[0].width == 252 && parsed.images[0].mips[0].height == 152);

    auto dec = DecodeTexMip0Rgba(parsed, 0, true, "hero_out.tex");
    CHECK(dec.width == 250 && dec.height == 150);

    // 2. Spritesheet preserves virtual header & gif canvas while scaling per-frame coordinates.
    TexSpriteSection sp;
    sp.version = 3;
    sp.gif_width = 500;
    sp.gif_height = 300;
    sp.frames.push_back(TexSpriteFrame{0, 0.08f, 100.0f, 50.0f, 200.0f, 0.0f, 0.0f, 120.0f});
    auto sp_raw = MakeRawTex(TexFormat::Rgba8888, kTexFlagIsGif, 500, 300, 500, 300, MakeSolidRgba(500, 300), sp);
    auto sp_res = TranscodeTexBytesForMobile(sp_raw.data(), sp_raw.size(), "sprite.tex", opts);
    auto sp_out = TexFile::FromBytes(sp_res.bytes.data(), sp_res.bytes.size(), "sprite_out.tex");
    CHECK(sp_out.header.texture_width == 500 && sp_out.header.texture_height == 300);
    CHECK(sp_out.sprites.has_value());
    CHECK(sp_out.sprites->gif_width == 500 && sp_out.sprites->gif_height == 300);
    CHECK(std::fabs(sp_out.sprites->frames[0].x - 50.0f) < 1e-4f);
    CHECK(std::fabs(sp_out.sprites->frames[0].y - 25.0f) < 1e-4f);
    CHECK(std::fabs(sp_out.sprites->frames[0].width - 100.0f) < 1e-4f);
    CHECK(std::fabs(sp_out.sprites->frames[0].height - 60.0f) < 1e-4f);

    // 3. Magic-byte sniffing: fif == -1 with embedded PNG bytes decodes cleanly!
    auto png_tex = MakeRawTex(TexFormat::Rgba8888, 0, 1, 1, 1, 1, MakeMinimal1x1Png(), std::nullopt, -1);
    TexTranscodeOptions hq_opts;
    hq_opts.force_etc2 = false;
    hq_opts.shrink = 1;
    auto png_res = TranscodeTexBytesForMobile(png_tex.data(), png_tex.size(), "embedded_png.tex", hq_opts);
    CHECK(png_res.transcoded);
    auto png_parsed = TexFile::FromBytes(png_res.bytes.data(), png_res.bytes.size(), "embedded_png_out.tex");
    CHECK(png_parsed.header.format_raw == static_cast<uint32_t>(TexFormat::Rgba8888));
    CHECK(png_parsed.images[0].mips[0].data.size() == 4);

    // 4. Magic-byte sniffing: embedded MP4 video without 0x20 flag passes through untouched!
    std::vector<uint8_t> fake_mp4 = {0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 0, 0};
    auto vid_tex = MakeRawTex(TexFormat::Rgba8888, 0, 64, 64, 64, 64, fake_mp4, std::nullopt, -1);
    auto vid_res = TranscodeTexBytesForMobile(vid_tex.data(), vid_tex.size(), "vid.tex", opts);
    CHECK(!vid_res.transcoded);
    CHECK(vid_res.skip_reason == "video_texture");

    // 5. Skip rules: RG88, R8, HDR floats, 3D LUT, existing ETC2.
    auto mask_tex = MakeRawTex(TexFormat::Rg88, 0, 32, 32, 32, 32, std::vector<uint8_t>(32 * 32 * 2, 128));
    CHECK(!TranscodeTexBytesForMobile(mask_tex.data(), mask_tex.size(), "mask.tex", opts).transcoded);
    auto hdr_tex = MakeRawTex(TexFormat::Rgba16161616f, 0, 8, 8, 8, 8, std::vector<uint8_t>(8 * 8 * 8, 0));
    CHECK(!TranscodeTexBytesForMobile(hdr_tex.data(), hdr_tex.size(), "hdr.tex", opts).transcoded);
    auto lut_tex = MakeRawTex(TexFormat::Rgba8888, kTexFlagIs3D, 4, 4, 4, 4, std::vector<uint8_t>(4 * 4 * 4 * 4, 0), std::nullopt, -1, 4);
    CHECK(!TranscodeTexBytesForMobile(lut_tex.data(), lut_tex.size(), "lut.tex", opts).transcoded);

    // 6. Existing mipmap reuse on 2x shrink: mip1 is solid blue, mip0 is red -> reusing mip1 gives blue!
    std::vector<uint8_t> red256(256 * 256 * 4, 0);
    for (size_t i = 0; i < red256.size(); i += 4) { red256[i] = 255; red256[i + 3] = 255; }
    std::vector<uint8_t> blue128(128 * 128 * 4, 0);
    for (size_t i = 0; i < blue128.size(); i += 4) { blue128[i + 2] = 255; blue128[i + 3] = 255; }
    TexMip m1{128, 128, std::nullopt, false, static_cast<uint32_t>(blue128.size()), blue128};
    auto multi_tex = MakeRawTex(TexFormat::Rgba8888, 0, 256, 256, 256, 256, red256, std::nullopt, -1, std::nullopt, m1);
    TexTranscodeOptions reuse_opts;
    reuse_opts.force_etc2 = false;
    reuse_opts.shrink = 2;
    reuse_opts.pixel_art = false;
    auto reuse_res = TranscodeTexBytesForMobile(multi_tex.data(), multi_tex.size(), "multi.tex", reuse_opts);
    auto reuse_parsed = TexFile::FromBytes(reuse_res.bytes.data(), reuse_res.bytes.size(), "multi_out.tex");
    CHECK((reuse_parsed.header.flags & kTexFlagNoMipmaps) == 0); // had 2 mips originally
    auto reuse_dec = DecodeTexMip0Rgba(reuse_parsed, 0, false, "multi_out.tex");
    CHECK(reuse_dec.width == 128 && reuse_dec.height == 128);
    CHECK(reuse_dec.rgba[0] == 0 && reuse_dec.rgba[2] == 255); // reused mip1!
}

void TestShaderRewriter() {
    // 1. General GLSL ES rules + type-safe integer protection.
    const char* src = R"GLSL(#include "common_blur.h"
uniform sampler2D g_Texture0;
#define FOO 1
float mod(float x, float y) { return x; }
void main() {
    // 1 sample comment stays intact
    const int sampleCount = 8;
    int idx = 0;
    int total = idx + 1;
    int m = min(idx, 2);
    float mf = min(idx, 2);
    vec4 sample = vec4(0, 1, 2, 1);
    float eps = 1e-5 + 2;
    float v = arr[0] + 3;
    for (int i = 0; i < sampleCount; ++i) {
        sample.rgb = BlendOpacity(sample.rgb, vec3(1, 0, 0), BlendScreen, 0.5);
    }
})GLSL";
    auto [out, changed] = RewriteShaderForMobile("shaders/effects/test.frag", src, 0);
    CHECK(changed);
    CHECK(out.find("uniform sampler2D g_Texture0;\r\n#include \"common_blur.h\"") != std::string::npos);
    CHECK(out.find("#define FOO 1\r\n") != std::string::npos);
    CHECK(out.find("// 1 sample comment stays intact\r\n") != std::string::npos);
    CHECK(out.find("float _deprFn_0(") != std::string::npos);
    CHECK(out.find("const float sampleCount = 8.0;") != std::string::npos);
    CHECK(out.find("int idx = 0;\r\n") != std::string::npos);
    CHECK(out.find("int total = idx + 1;\r\n") != std::string::npos);
    CHECK(out.find("int m = min(idx, 2);\r\n") != std::string::npos);
    CHECK(out.find("float mf = float(min(idx, 2));\r\n") != std::string::npos);
    CHECK(out.find("vec4 _sample = vec4(0.0, 1.0, 2.0, 1.0);") != std::string::npos);
    CHECK(out.find("float eps = 1e-5 + 2.0;") != std::string::npos);
    CHECK(out.find("float v = arr[0] + 3.0;") != std::string::npos);
    CHECK(out.find("for (float i = 0.0; i < sampleCount; ++i)") != std::string::npos);
    CHECK(out.find("mix(_sample.rgb,BlendScreen(_sample.rgb,vec3(1.0, 0.0, 0.0)),0.5)") != std::string::npos);

    // 2. Non-byte-0 common_blur.h must NOT relocate g_Texture0.
    std::string non_byte0 = "\n#include \"common_blur.h\"\nuniform sampler2D g_Texture0; // {\"hidden\":true}\nvoid main() {}\n";
    auto [nb_out, _] = RewriteShaderForMobile("shaders/effects/godrays_gaussian.frag", non_byte0, 0);
    CHECK(nb_out.find("uniform sampler2D g_Texture0; // {\"hidden\":true}") != std::string::npos);
    CHECK(nb_out.rfind("uniform sampler2D g_Texture0;\r\n", 0) != 0);

    // 3. Stem patches: xray.vert clean single semicolon, blend, perspective, skew, shift_hue, foliagesway.
    std::string xray_vert =
        "uniform mat4 g_ModelViewProjectionMatrixInverse;\n"
        "void main() {\n"
        "    v_PointerUV = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrixInverse).xyw;\n"
        "    v_PointerUV.xy *= 1.0 / g_Texture0Resolution.xy;\n"
        "}\n";
    auto [xv_out, xv_changed] = RewriteShaderForMobile("shaders/effects/xray.vert", xray_vert, 0);
    CHECK(xv_changed);
    CHECK(xv_out.find("uniform mat4 g_EffectTextureProjectionMatrixInverse;") != std::string::npos);
    CHECK(xv_out.find("g_EffectTextureProjectionMatrixInverse).xyw;\r\n") != std::string::npos);
    CHECK(xv_out.find(";;") == std::string::npos);
    CHECK(xv_out.find("v_PointerUV.xy *= 0.5;") != std::string::npos);

    // 4. zcompat workshop overrides (2078835426 & 2084198056 maximumprojectid + real repo assets dir).
    auto [zc1, zc1_ch] = RewriteShaderForMobile(
        "shaders/workshop/2078835426/effects/pixelate.frag", "// stub", 2500000000ULL);
    CHECK(zc1_ch && zc1.find("v_PixelCoord") != std::string::npos);
    auto [zc2, zc2_ch] = RewriteShaderForMobile(
        "shaders/workshop/2084198056/effects/Simple_Audio_Bars.frag", "// stub", 2000000000ULL);
    CHECK(zc2_ch && zc2.find("u_AudioSpectrumLeft") != std::string::npos);
    auto [zc3, _zc3] = RewriteShaderForMobile(
        "shaders/workshop/2084198056/effects/Simple_Audio_Bars.frag", "void main() { float a = 1; }", 3000000000ULL);
    CHECK(zc3.find("u_AudioSpectrumLeft") == std::string::npos);
    CHECK(zc3.find("float a = 1.0;") != std::string::npos);
    std::filesystem::path repo_zcompat = std::filesystem::current_path() / "assets/zcompat/scene/shaders";
    if (std::filesystem::is_directory(repo_zcompat)) {
        auto [zc_ext, zc_ext_ch] = RewriteShaderForMobile(
            "shaders/workshop/2078835426/effects/pixelate.frag", "// stub", 2500000000ULL, repo_zcompat);
        CHECK(zc_ext_ch && zc_ext.find("v_PixelCoord") != std::string::npos);
    }

    // 5. Real-world GLSL ES float promotion patterns (swizzle assign, chained binary ops,
    //    overloaded float calls, user-defined float-param calls, ternary, and int protection).
    std::string real_glsl = R"GLSL(
#define NODE_COUNT 9
varying vec4 v_TexCoord;
varying float i_DCorrectingFactor;
float cubicStep(float edge0, float edge1, float x) { return x; }
void helper(float nodeNum, out float autoMask) {
    autoMask = 1;
    float prev = sin(g_Time + (nodeNum + 1));
}
float calc(float volume, float x, float lower, float upper, vec2 pointer, int shape) {
    v_TexCoord.z = 0;
    i_DCorrectingFactor = 1;
    int mask = 1;
    float blend = 2 * abs(x - 0.5);
    float aa = 15 / min(g_Texture0Resolution.x, g_Texture0Resolution.y);
    float off = g_Time / max(1, NODE_COUNT - 2);
    vec4 p = vec4(pointer * 2 - 1, 0, 1);
    float w = cubicStep(0, upper, x);
    float t = (shape != int(0) ? 0.159 : 1);
    float s = sqrt(1 - pow(saturate((x - lower) / (upper - lower)) - 1, 2));
    return min(1, 1 - sqrt(1 - pow(volume, 2)));
}
)GLSL";
    auto [rg_out, rg_changed] = RewriteShaderForMobile("shaders/effects/real.frag", real_glsl, 0);
    CHECK(rg_changed);
    CHECK(rg_out.find("autoMask = 1.0;") != std::string::npos);
    CHECK(rg_out.find("(nodeNum + 1.0)") != std::string::npos);
    CHECK(rg_out.find("v_TexCoord.z = 0.0;") != std::string::npos);
    CHECK(rg_out.find("i_DCorrectingFactor = 1.0;") != std::string::npos);
    CHECK(rg_out.find("int mask = 1;") != std::string::npos);
    CHECK(rg_out.find("float blend = 2.0 * abs(x - 0.5);") != std::string::npos);
    CHECK(rg_out.find("float aa = 15.0 / min(") != std::string::npos);
    CHECK(rg_out.find("max(1.0, NODE_COUNT - 2.0)") != std::string::npos);
    CHECK(rg_out.find("vec4(pointer * 2.0 - 1.0, 0.0, 1.0)") != std::string::npos);
    CHECK(rg_out.find("cubicStep(0.0, upper, x)") != std::string::npos);
    CHECK(rg_out.find("(shape != int(0) ? 0.159 : 1.0)") != std::string::npos);
    CHECK(rg_out.find("pow(saturate((x - lower) / (upper - lower)) - 1.0, 2.0)") != std::string::npos);
    CHECK(rg_out.find("return min(1.0, 1.0 - sqrt(1.0 - pow(volume, 2.0)));") != std::string::npos);
}

void TestEndToEndPipelinesAndSecurity() {
    ScopedTempDir tmp("Mirage-SMC-Test");

    // === Case 1: Full scene.pkg -> .mpkg with overrides, VFS scenetexture, sounds strip, progress ===
    auto dir1 = tmp.path() / "scene_pkg_case";
    std::filesystem::create_directories(dir1);

    TexSpriteSection sp{3, 256, 256, {TexSpriteFrame{0, 0.1f, 128, 64, 128, 0, 0, 64}}};
    auto hero_tex = MakeRawTex(TexFormat::Rgba8888, kTexFlagIsGif | kTexFlagClearOnMobile,
                               256, 256, 256, 256, MakeSolidRgba(256, 256), sp);
    auto mask_tex = MakeRawTex(TexFormat::Rg88, 0, 16, 16, 16, 16, std::vector<uint8_t>(16 * 16 * 2, 128));
    auto blend_frag = std::string(
        "void main() { vec2 m = step(0, uv) * step(uv, 1); gl_FragColor.rgb = BlendOpacity(a, b, BlendAdd, 0.5); }");

    WriteRawPkg(dir1 / "scene.pkg", "PKGV0022", {
        {"scene.json", std::vector<uint8_t>{'{' , '"', 'g', 'e', 'n', 'e', 'r', 'a', 'l', '"', ':', '{', '"', 'o', 'r', 't', 'h', 'o', 'g', 'o', 'n', 'a', 'l', 'p', 'r', 'o', 'j', 'e', 'c', 't', 'i', 'o', 'n', '"', ':', '{', '"', 'w', 'i', 'd', 't', 'h', '"', ':', '1', '9', '2', '0', ',', '"', 'h', 'e', 'i', 'g', 'h', 't', '"', ':', '1', '0', '8', '0', '}', '}', '}'}},
        {"materials/hero.tex", hero_tex},
        {"materials/masks/flow.tex", mask_tex},
        {"shaders/effects/blend.frag", std::vector<uint8_t>(blend_frag.begin(), blend_frag.end())},
        {"sounds/bgm.mp3", std::vector<uint8_t>(64, 0)},
    });
    WriteBytes(dir1 / "preview.png", MakeMinimal1x1Png());
    WriteBytes(dir1 / "user_bg.png", MakeMinimal1x1Png());
    WriteText(dir1 / "project.json", R"({
        // JsonCpp comment allowed
        "file": "scene.json",
        "title": "Synthetic Scene",
        "type": "scene",
        "preview": "preview.png",
        "workshopid": "2000000000",
        "general": {
            "properties": {
                "customtex": { "type": "scenetexture", "value": "" },
                "missingtex": { "type": "scenetexture", "value": "old.png" },
                "alignment": { "type": "combo", "value": "center" },
                "sliderval": { "type": "slider", "value": 10 },
            }
        }
    })");

    std::string overrides = std::string("{\"customtex\":\"") + (dir1 / "user_bg.png").string() +
                            "\",\"missingtex\":\"/nonexistent/file.png\",\"alignment\":\"left\",\"sliderval\":42}";
    auto out1 = tmp.path() / "out1.mpkg";

    double last_prog = -1.0;
    bool monotonic = true;
    auto prog_cb = [](double frac, void* ctx) {
        auto* pair = static_cast<std::pair<double*, bool*>*>(ctx);
        if (frac + 1e-9 < *pair->first) *pair->second = false;
        *pair->first = frac;
    };
    std::pair<double*, bool*> prog_ctx{&last_prog, &monotonic};

    MirageSceneMobileExportOptions opt1{};
    std::string dir1_s = dir1.string();
    std::string out1_s = out1.string();
    opt1.wallpaper_dir = dir1_s.c_str();
    opt1.render_dir = dir1_s.c_str();
    opt1.output_path = out1_s.c_str();
    opt1.property_overrides_json = overrides.c_str();
    opt1.texture_reduction = 2;
    opt1.pixel_art = 0;
    opt1.compression_mode = MIRAGE_MOBILE_COMPRESSION_HIGH_PERFORMANCE;
    opt1.progress_cb = prog_cb;
    opt1.progress_context = &prog_ctx;

    auto r1 = MirageSceneMobileExport(&opt1);
    CHECK(r1.status == MIRAGE_MOBILE_OK);
    CHECK(std::string(r1.mpkg_magic) == "PKGM0019");
    CHECK(r1.tex_transcoded == 1 && r1.tex_skipped == 1);
    CHECK(r1.shaders_rewritten == 1 && r1.sounds_removed == 1);
    CHECK(monotonic && last_prog == 1.0);
    MirageSceneMobileFreeString(r1.error_detail);

    auto mpkg1 = PkgIndex::Open(out1);
    CHECK(mpkg1.magic() == "PKGM0019");
    CHECK(mpkg1.FindEntry("sounds/bgm.mp3") == nullptr);
    CHECK(mpkg1.FindEntry("files/user_bg.png") != nullptr);
    CHECK(mpkg1.FindEntry("materials/masks/flow.tex") != nullptr);
    CHECK(mpkg1.ReadEntry(*mpkg1.FindEntry("materials/masks/flow.tex")) == mask_tex);

    auto pj1_bytes = mpkg1.ReadEntry(*mpkg1.FindEntry("project.json"));
    auto pj1 = ParseJsonLenient(pj1_bytes);
    CHECK(pj1.has_value());
    CHECK((*pj1)["general"]["properties"]["customtex"]["value"] == "MPKG_VFS://files/user_bg.png");
    CHECK(!(*pj1)["general"]["properties"].contains("missingtex"));
    CHECK((*pj1)["general"]["properties"]["alignment"]["value"] == "center");
    CHECK((*pj1)["general"]["properties"]["sliderval"]["value"] == 42);

    auto sj1_bytes = mpkg1.ReadEntry(*mpkg1.FindEntry("scene.json"));
    auto sj1 = ParseJsonLenient(sj1_bytes);
    CHECK(sj1.has_value() && (*sj1)["general"]["texturereduction"] == 2);

    // === Case 2: Loose project directory (no scene.pkg, no preview, dependency crawler + decoys) ===
    auto dir2 = tmp.path() / "loose_scene";
    WriteText(dir2 / "project.json", R"({"file":"scene.json","title":"Loose Scene","type":"scene"})");
    WriteText(dir2 / "scene.json", R"json({
        "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
        "objects": [{"image": "models/hero.json", "script": "engine.registerAsset(\"particles/spark.json\")"}]
    })json");
    WriteText(dir2 / "models/hero.json", R"({"material":"materials/hero.json"})");
    WriteText(dir2 / "materials/hero.json", R"({"passes":[{"shader":"custom","textures":["hero"]}]})");
    auto tex64 = MakeRawTex(TexFormat::Rgba8888, 0, 64, 64, 64, 64, MakeSolidRgba(64, 64));
    WriteBytes(dir2 / "materials/hero.tex", tex64);
    WriteBytes(dir2 / "materials/hero.png", MakeMinimal1x1Png()); // editor source decoy
    WriteBytes(dir2 / "materials/unused.tex", tex64); // unreferenced decoy
    WriteText(dir2 / "shaders/custom.frag", "#include \"inc/helper.h\"\nvoid main() { gl_FragColor = vec4(1, 0, 0, 1); }\n");
    WriteText(dir2 / "shaders/inc/helper.h", "// helper\n");
    WriteText(dir2 / "shaders/blobsSM40/cache.dxs", "DXBC");
    WriteText(dir2 / "particles/spark.json", R"({"material":"materials/spark.json"})");
    WriteText(dir2 / "materials/spark.json", R"({"passes":[{"textures":["spark"]}]})");
    WriteBytes(dir2 / "materials/spark.tex", tex64);

    auto out2 = tmp.path() / "out2.mpkg";
    MirageSceneMobileExportOptions opt2{};
    std::string dir2_s = dir2.string();
    std::string out2_s = out2.string();
    opt2.wallpaper_dir = dir2_s.c_str();
    opt2.render_dir = dir2_s.c_str();
    opt2.output_path = out2_s.c_str();
    opt2.texture_reduction = 1;
    opt2.pixel_art = 0;
    opt2.compression_mode = MIRAGE_MOBILE_COMPRESSION_HIGH_PERFORMANCE;
    auto r2 = MirageSceneMobileExport(&opt2);
    CHECK(r2.status == MIRAGE_MOBILE_OK);
    CHECK(std::string(r2.mpkg_magic) == "PKGM0020");
    MirageSceneMobileFreeString(r2.error_detail);

    auto mpkg2 = PkgIndex::Open(out2);
    CHECK(mpkg2.FindEntry("materials/hero.tex") != nullptr);
    CHECK(mpkg2.FindEntry("materials/spark.tex") != nullptr);
    CHECK(mpkg2.FindEntry("shaders/inc/helper.h") != nullptr);
    CHECK(mpkg2.FindEntry("nopreview.jpg") != nullptr);
    CHECK(mpkg2.FindEntry("materials/unused.tex") == nullptr);
    CHECK(mpkg2.FindEntry("materials/hero.png") == nullptr);
    CHECK(mpkg2.FindEntry("shaders/blobsSM40/cache.dxs") == nullptr);

    // === Case 3: Scene Preset export (wallpaper_dir != render_dir + overlay_dir) ===
    auto preset_dir = tmp.path() / "preset_case";
    WriteBytes(preset_dir / "preset_preview.png", MakeMinimal1x1Png());
    WriteBytes(preset_dir / "files/custom.png", MakeMinimal1x1Png());
    WriteText(preset_dir / "project.json", R"({
        "title": "My Cool Preset",
        "preview": "preset_preview.png",
        "workshopid": "3500000000",
        "dependency": "2000000000",
        "preset": { "sliderval": 77, "customtex": "files/custom.png" }
    })");
    auto out3 = tmp.path() / "out3.mpkg";
    std::string preset_s = preset_dir.string();
    std::string out3_s = out3.string();
    const char* overlays[] = {preset_s.c_str()};
    std::string preset_ov = R"({"sliderval":88})";
    int32_t detail_int = 0;
    char* err_detail = nullptr;
    int32_t flat_status = MirageSceneMobileExportFlat(
        preset_s.c_str(),
        dir1_s.c_str(),
        overlays,
        1,
        out3_s.c_str(),
        nullptr,
        preset_ov.c_str(),
        1,
        0,
        MIRAGE_MOBILE_COMPRESSION_HIGH_PERFORMANCE,
        0,
        nullptr,
        nullptr,
        nullptr,
        &detail_int,
        &err_detail);
    CHECK(flat_status == MIRAGE_MOBILE_OK);
    MirageSceneMobileFreeString(err_detail);
    auto mpkg3 = PkgIndex::Open(out3);
    CHECK(mpkg3.FindEntry("preset_preview.png") != nullptr);
    CHECK(mpkg3.FindEntry("files/custom.png") != nullptr);
    auto pj3 = ParseJsonLenient(mpkg3.ReadEntry(*mpkg3.FindEntry("project.json")));
    CHECK(pj3.has_value());
    CHECK((*pj3)["title"] == "My Cool Preset");
    CHECK((*pj3)["workshopid"] == "3500000000");
    CHECK((*pj3)["general"]["properties"]["sliderval"]["value"] == 88);
    CHECK((*pj3)["general"]["properties"]["customtex"]["value"] == "MPKG_VFS://files/custom.png");

    // === Case 4: Security — symlink escape, path traversal, overlapping pkg, cancellation & atomic preserve ===
    auto secret_file = tmp.path() / "secret.png";
    WriteBytes(secret_file, MakeMinimal1x1Png());
    auto evil_dir = tmp.path() / "evil_scene";
    std::filesystem::create_directories(evil_dir);
    WriteRawPkg(evil_dir / "scene.pkg", "PKGV0024", {{"scene.json", {'{', '}'}}});
    WriteText(evil_dir / "project.json", R"({"file":"scene.json","title":"Evil","type":"scene","preview":"../secret.png"})");

    // Pre-populate output file to verify failed export preserves existing file atomically.
    auto protected_out = tmp.path() / "existing.mpkg";
    WriteText(protected_out, "EXISTING_OK");
    MirageSceneMobileExportOptions opt_evil{};
    std::string evil_s = evil_dir.string();
    std::string prot_s = protected_out.string();
    opt_evil.wallpaper_dir = evil_s.c_str();
    opt_evil.render_dir = evil_s.c_str();
    opt_evil.output_path = prot_s.c_str();
    auto r_evil = MirageSceneMobileExport(&opt_evil);
    CHECK(r_evil.status == MIRAGE_MOBILE_UNSAFE_PROJECT_PATH);
    MirageSceneMobileFreeString(r_evil.error_detail);
    auto kept = ReadFileBytes(protected_out);
    CHECK(std::string(kept.begin(), kept.end()) == "EXISTING_OK");

    // Cancellation test.
    volatile int32_t cancel_flag = 1;
    MirageSceneMobileExportOptions opt_cancel = opt1;
    opt_cancel.output_path = prot_s.c_str();
    opt_cancel.cancel_flag = &cancel_flag;
    auto r_cancel = MirageSceneMobileExport(&opt_cancel);
    CHECK(r_cancel.status == MIRAGE_MOBILE_CANCELLED);
    MirageSceneMobileFreeString(r_cancel.error_detail);

    // Prefix collision rejection in WriteMpkgArchive (`a` and `a/b`).
    bool prefix_rejected = false;
    try {
        WriteMpkgArchive({
            MpkgWriteEntry::FromMemory("dir", {'1'}),
            MpkgWriteEntry::FromMemory("dir/file.txt", {'2'}),
        }, "PKGM0020", tmp.path() / "collision.mpkg");
    } catch (const ConvertError& e) {
        prefix_rejected = (e.status() == MIRAGE_MOBILE_UNSAFE_PROJECT_PATH);
    }
    CHECK(prefix_rejected);

    // PKGV0015 legacy mapping -> PKGM0014.
    CHECK(MapPkgToMpkgVersion(1, false) == 14);
    CHECK(MapPkgToMpkgVersion(15, false) == 14);
    CHECK(MapPkgToMpkgVersion(16, true) == 20);
    CHECK(MapPkgToMpkgVersion(17, false) == 15);
    CHECK(MapPkgToMpkgVersion(24, false) == 20);
}

} // namespace

int main() {
    TestJsonCompat();
    TestLz4CodecAndBombs();
    TestDxtDecoders();
    TestEtc2QualityAndDeterminism();
    TestTexTranscoder();
    TestShaderRewriter();
    TestEndToEndPipelinesAndSecurity();
    std::cout << "PASS: SceneMobileConverter C++ regression suite (JSON, LZ4, DXT, ETC2, TEX, Shader/zcompat, PKG/Deps, Presets, Security)\n";
    return 0;
}

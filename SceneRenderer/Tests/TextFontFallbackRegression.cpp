//
// Mirage Wallpaper
//
// Copyright © 2026 王孝慈. All rights reserved.
//

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

import sr.text;

namespace
{

using Bytes = std::vector<std::byte>;

bool Check(bool value, const char* message) {
    if (! value) std::cerr << "FAIL: " << message << '\n';
    return value;
}

std::shared_ptr<Bytes> Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (! input || input.tellg() <= 0) return {};
    auto bytes = std::make_shared<Bytes>(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    if (! input.read(reinterpret_cast<char*>(bytes->data()),
                     static_cast<std::streamsize>(bytes->size())))
        return {};
    return bytes;
}

std::uint32_t U32(const Bytes& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value = (value << 8) | std::to_integer<std::uint8_t>(bytes.at(offset + i));
    return value;
}

void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        bytes.at(offset + i) = static_cast<std::byte>(value >> (24 - i * 8));
}

Bytes Collection(const Bytes& first, const Bytes& second) {
    Bytes bytes(20);
    Put32(bytes, 0, 0x74746366);
    Put32(bytes, 4, 0x00010000);
    Put32(bytes, 8, 2);
    std::size_t index = 0;
    for (const auto* font : { &first, &second }) {
        bytes.resize((bytes.size() + 3) & ~std::size_t(3));
        const auto base = bytes.size();
        Put32(bytes, 12 + index++ * 4, static_cast<std::uint32_t>(base));
        bytes.insert(bytes.end(), font->begin(), font->end());
        const auto count =
            (std::to_integer<unsigned>(font->at(4)) << 8) | std::to_integer<unsigned>(font->at(5));
        for (unsigned i = 0; i < count; ++i) {
            const auto offset = base + 12 + i * 16 + 8;
            Put32(bytes, offset, static_cast<std::uint32_t>(base) + U32(bytes, offset));
        }
    }
    return bytes;
}

std::vector<std::uint8_t> Ink(const sr::text::FontFace& face, std::uint32_t codepoint) {
    const auto* glyph = face.Lookup(codepoint);
    if (! glyph) return {};
    const auto                pixels = face.AtlasPixels();
    const auto                width  = face.Metrics().atlas_w;
    std::vector<std::uint8_t> ink;
    for (std::uint32_t y = 0; y < glyph->pixel_h; ++y) {
        const auto start = (glyph->atlas_y + y) * width + glyph->atlas_x;
        ink.insert(ink.end(), pixels.begin() + start, pixels.begin() + start + glyph->pixel_w);
    }
    return ink;
}

bool SameGlyph(sr::text::FontFace* actual, sr::text::FontFace* expected, std::uint32_t codepoint) {
    if (! actual || ! expected) return false;
    const std::array codepoints { codepoint };
    actual->Populate(codepoints);
    expected->Populate(codepoints);
    const auto* a = actual->Lookup(codepoint);
    const auto* b = expected->Lookup(codepoint);
    return a && b && a->pixel_w > 0 && a->pixel_h > 0 && a->pixel_w == b->pixel_w &&
           a->pixel_h == b->pixel_h && std::abs(a->advance_x - b->advance_x) < 0.001f &&
           Ink(*actual, codepoint) == Ink(*expected, codepoint);
}

struct FontConfig {
    FcConfig* saved { FcConfigReference(FcConfigGetCurrent()) };
    FcConfig* isolated { FcConfigCreate() };

    ~FontConfig() {
        FcConfigSetCurrent(saved);
        FcConfigDestroy(isolated);
        FcConfigDestroy(saved);
    }
};

struct TemporaryDirectory {
    std::filesystem::path path;
    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

bool AddFont(FcConfig* config, const std::filesystem::path& path) {
    return FcConfigAppFontAddFile(config, reinterpret_cast<const FcChar8*>(path.c_str())) == FcTrue;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 77;
    const auto primary     = Read(std::filesystem::path(argv[1]) / "Monofur-PK7og.ttf");
    const auto symbols     = Read("/System/Library/Fonts/Apple Symbols.ttf");
    const auto last_resort = Read("/System/Library/Fonts/LastResort.otf");
    const auto emoji       = Read("/System/Library/Fonts/Apple Color Emoji.ttc");
    const std::filesystem::path cjk_path = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf";
    const auto                  cjk      = Read(cjk_path);
    if (! primary || ! symbols || ! last_resort || ! emoji || ! cjk) {
        std::cerr << "font fixtures unavailable\n";
        return 77;
    }

    const auto         nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryDirectory temporary { std::filesystem::temp_directory_path() /
                                   ("mirage-font-fallback-" + std::to_string(nonce)) };
    std::filesystem::create_directories(temporary.path);
    const auto collection_path = temporary.path / "collection.ttc";
    const auto collection      = std::make_shared<Bytes>(Collection(*primary, *symbols));
    {
        std::ofstream output(collection_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(collection->data()),
                     static_cast<std::streamsize>(collection->size()));
        if (! output) return 1;
    }

    if (! FcInit()) return 1;
    FontConfig            config;
    constexpr const char* rules =
        R"(<fontconfig><match target="pattern"><edit name="family" mode="prepend" binding="strong"><string>Mirage Broken</string><string>.LastResort</string></edit></match></fontconfig>)";
    if (! config.isolated ||
        ! FcConfigParseAndLoadFromMemory(
            config.isolated, reinterpret_cast<const FcChar8*>(rules), FcTrue) ||
        ! AddFont(config.isolated, "/System/Library/Fonts/LastResort.otf") ||
        ! AddFont(config.isolated, collection_path) || ! FcConfigSetCurrent(config.isolated))
        return 1;

    bool       ok = true;
    FcPattern* pattern =
        FcNameParse(reinterpret_cast<const FcChar8*>(":charset=1d11e:scalable=true"));
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult   result;
    FcPattern* match        = FcFontMatch(nullptr, pattern, &result);
    FcChar8*   matched_file = nullptr;
    ok &= Check(
        match && FcPatternGetString(match, FC_FILE, 0, &matched_file) == FcResultMatch &&
            std::string(reinterpret_cast<const char*>(matched_file)).ends_with("LastResort.otf"),
        "fixture reproduces the placeholder being the first font match");
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pattern);

    sr::text::FontCache cache;
    auto*               face   = cache.GetFace(primary, 20);
    auto*               first  = cache.GetFace(collection, 20, 0);
    auto*               second = cache.GetFace(collection, 20, 1);
    ok &= Check(first && second && first != second && first->AtlasUrl() != second->AtlasUrl(),
                "collection faces have distinct cache entries and atlases");
    ok &= Check(cache.GetFace(collection, 20, 1) == second, "collection face is reused");
    ok &= Check(SameGlyph(face, second, 0x1D11E),
                "missing music symbol uses the real glyph in collection face one");
    ok &= Check(SameGlyph(face, first, 'A'), "primary Latin glyph remains unchanged");
    ok &= Check(cache.GetFace(last_resort, 20) == nullptr,
                "placeholder is rejected as a primary font");

    const auto resolved = sr::text::FontCache::ResolveSystemFont("systemfont_apple symbols", false);
    ok &= Check(resolved.bytes && resolved.source == collection_path.string() &&
                    resolved.face_index == 1,
                "system font resolution retains the collection face index");
    ok &= Check(SameGlyph(cache.GetFace(resolved.bytes, 20, resolved.face_index), second, 0x1D11E),
                "resolved system font renders the selected face");
    ok &= Check(SameGlyph(cache.GetFace(resolved.bytes, 40, resolved.face_index),
                          cache.GetFace(symbols, 40),
                          0x1D11E),
                "resizing a selected collection face preserves its glyph");

    if (face) {
        const std::array<std::uint32_t, 1> missing { 0x10FFFF };
        face->Populate(missing);
        const auto* glyph = face->Lookup(missing[0]);
        ok &= Check(glyph && glyph->pixel_w == 0 && glyph->pixel_h == 0,
                    "unsupported character never uses a LastResort glyph");
    }

    FcPattern* broken  = FcPatternCreate();
    FcCharSet* charset = FcCharSetCreate();
    FcCharSetAddChar(charset, 0x1D11E);
    FcPatternAddString(broken, FC_FAMILY, reinterpret_cast<const FcChar8*>("Mirage Broken"));
    FcPatternAddString(broken,
                       FC_FILE,
                       reinterpret_cast<const FcChar8*>((temporary.path / "missing.ttf").c_str()));
    FcPatternAddInteger(broken, FC_INDEX, 0);
    FcPatternAddBool(broken, FC_SCALABLE, FcTrue);
    FcPatternAddCharSet(broken, FC_CHARSET, charset);
    FcCharSetDestroy(charset);
    FcFontSetAdd(FcConfigGetFonts(config.isolated, FcSetApplication), broken);
    ok &= Check(SameGlyph(cache.GetFace(primary, 21), cache.GetFace(symbols, 21), 0x1D11E),
                "unreadable candidate does not prevent later glyph fallback");

    ok &= Check(AddFont(config.isolated, cjk_path), "register CJK font fixture");
    for (const auto codepoint : { 0x4E2Du, 0x97F3u, 0x4E50u }) {
        ok &= Check(SameGlyph(cache.GetFace(primary, 19), cache.GetFace(cjk, 19), codepoint),
                    "Chinese text uses real CJK glyphs");
    }

    ok &= Check(AddFont(config.isolated, "/System/Library/Fonts/Apple Color Emoji.ttc"),
                "register bitmap font fixture");
    auto* emoji_face = cache.GetFace(primary, 10);
    if (emoji_face) {
        const std::array<std::uint32_t, 1> notes { 0x1F3B5 };
        emoji_face->Populate(notes);
        FT_Library library   = nullptr;
        FT_Face    reference = nullptr;
        if (FT_Init_FreeType(&library) ||
            FT_New_Memory_Face(library,
                               reinterpret_cast<const FT_Byte*>(emoji->data()),
                               static_cast<FT_Long>(emoji->size()),
                               0,
                               &reference) ||
            FT_Set_Pixel_Sizes(reference, 0, 20) ||
            FT_Load_Char(reference, notes[0], FT_LOAD_RENDER | FT_LOAD_COLOR)) {
            ok &= Check(false, "load reference bitmap glyph");
        } else {
            const auto&               bitmap = reference->glyph->bitmap;
            std::vector<std::uint8_t> alpha;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                for (unsigned y = 0; y < bitmap.rows; ++y)
                    for (unsigned x = 0; x < bitmap.width; ++x)
                        alpha.push_back(
                            bitmap
                                .buffer[static_cast<std::ptrdiff_t>(y) * bitmap.pitch + x * 4 + 3]);
            }
            ok &= Check(! alpha.empty() && Ink(*emoji_face, notes[0]) == alpha,
                        "BGRA glyph contributes its alpha channel to the text atlas");
        }
        if (reference) FT_Done_Face(reference);
        if (library) FT_Done_FreeType(library);
        auto* small = cache.GetFace(primary, 8);
        if (small) small->Populate(notes);
        const auto* small_glyph = small ? small->Lookup(notes[0]) : nullptr;
        ok &= Check(small_glyph && small_glyph->pixel_w > 0 && small_glyph->layout_w > 0 &&
                        small_glyph->layout_w <= 16,
                    "bitmap fallback supports a size absent from its fixed strikes");
    } else {
        ok &= Check(false, "load primary font for bitmap fallback");
    }

    if (ok) std::cout << "Text font fallback regression passed\n";
    return ok ? 0 : 1;
}

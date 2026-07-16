module;

#include <rstd/macro.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <fontconfig/fontconfig.h>
module sr.text;
import sr.spec_texs;
import sr.core;
import sr.types;
import rstd.log;
import rstd.cppstd;
import sr.scene;
import sr.shader_compile;

namespace sr::text
{

namespace
{

constexpr std::uint32_t kMinAtlasDim { 1024 };

// 4×4 white cell at (0,0) so a single-channel atlas can also serve solid-fill
// quads (e.g. opaquebackground rectangle) and as a tofu fallback when the
// atlas overflows.
constexpr std::uint32_t kWhiteCellSize { 4 };

// True for Unicode whitespace/format codepoints that carry layout advance but
// no visible ink (regular space, NBSP, the fixed-width spaces, narrow/zero-
// width no-break spaces, etc.). These must NOT be routed through the
// fontconfig glyph fallback: a fallback face resolves e.g. U+202F to its own
// narrow-space advance, collapsing the spacing the primary font (and the
// scene's clock/date scripts) were laid out against. Keeping them on the
// primary face preserves that font's advance for the codepoint.
bool IsLayoutWhitespace(std::uint32_t cp) {
    switch (cp) {
    case 0x0020: // space
    case 0x00A0: // no-break space
    case 0x2000: // en quad
    case 0x2001: // em quad
    case 0x2002: // en space
    case 0x2003: // em space
    case 0x2004: // three-per-em space
    case 0x2005: // four-per-em space
    case 0x2006: // six-per-em space
    case 0x2007: // figure space
    case 0x2008: // punctuation space
    case 0x2009: // thin space
    case 0x200A: // hair space
    case 0x202F: // narrow no-break space
    case 0x205F: // medium mathematical space
    case 0x3000: // ideographic space
        return true;
    default:
        return false;
    }
}

std::uint32_t AtlasDimForPixelSize(std::uint32_t pixel_size) {
    if (pixel_size > 512) return 4096;
    if (pixel_size > 256) return 2048;
    return kMinAtlasDim;
}

class FtLibrary {
public:
    static FtLibrary& Get() {
        static FtLibrary inst;
        return inst;
    }
    FT_Library handle() const noexcept { return m_lib; }

private:
    FtLibrary() {
        if (FT_Init_FreeType(&m_lib) != 0) {
            rstd_error("FT_Init_FreeType failed");
            m_lib = nullptr;
        }
    }
    ~FtLibrary() {
        if (m_lib != nullptr) FT_Done_FreeType(m_lib);
    }
    FtLibrary(const FtLibrary&)            = delete;
    FtLibrary& operator=(const FtLibrary&) = delete;

    FT_Library m_lib { nullptr };
};

std::uint64_t HashBlob(std::span<const std::byte> blob) {
    // FNV-1a 64. Good enough for keying — collisions only matter if the
    // caller hands us two genuinely different fonts whose hashes collide,
    // which is fine since the underlying FT_Face open is the source of truth
    // (we cache per-pixel-size below the blob).
    std::uint64_t h = 1469598103934665603ull;
    for (std::byte b : blob) {
        h ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
        h *= 1099511628211ull;
    }
    return h;
}

bool IsFontExt(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext.compare(".ttf") == 0 || ext.compare(".otf") == 0 || ext.compare(".ttc") == 0;
}

std::shared_ptr<std::vector<std::byte>> ReadAll(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (! f) return nullptr;
    auto size = f.tellg();
    if (size <= 0) return nullptr;
    f.seekg(0, std::ios::beg);
    auto buf = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
    if (! f.read(reinterpret_cast<char*>(buf->data()), size)) return nullptr;
    return buf;
}

std::filesystem::path ResolveFontconfigCodepoint(std::uint32_t codepoint) {
    if (! FcInit()) return {};

    FcPattern* pat = FcPatternCreate();
    if (pat == nullptr) return {};
    FcCharSet* charset = FcCharSetCreate();
    if (charset == nullptr) {
        FcPatternDestroy(pat);
        return {};
    }
    FcCharSetAddChar(charset, static_cast<FcChar32>(codepoint));
    FcPatternAddCharSet(pat, FC_CHARSET, charset);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult   res   = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pat, &res);
    FcCharSetDestroy(charset);
    FcPatternDestroy(pat);

    if (match == nullptr || res != FcResultMatch) {
        if (match != nullptr) FcPatternDestroy(match);
        return {};
    }

    FcChar8*              file = nullptr;
    std::filesystem::path out;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
        out = std::filesystem::path(reinterpret_cast<const char*>(file));
    }
    FcPatternDestroy(match);
    return out;
}

} // namespace

// -- FontFace -------------------------------------------------------------

struct FontFace::Impl {
    struct FallbackFace {
        std::shared_ptr<std::vector<std::byte>> blob;
        FT_Face                                 face { nullptr };

        ~FallbackFace() {
            if (face != nullptr) FT_Done_Face(face);
        }
    };

    std::shared_ptr<std::vector<std::byte>> blob;
    FT_Face                                 face { nullptr };
    std::uint32_t                           pixel_size { 0 };

    std::uint32_t             atlas_w { kMinAtlasDim };
    std::uint32_t             atlas_h { kMinAtlasDim };
    std::vector<std::uint8_t> atlas;

    // Shelf packer state: pen advances along the current shelf, falls to a
    // new shelf when the next glyph won't fit horizontally.
    std::uint32_t pen_x { 0 };
    std::uint32_t pen_y { 0 };
    std::uint32_t shelf_h { 0 };

    std::unordered_map<std::uint32_t, GlyphInfo>                   glyphs;
    std::unordered_map<std::string, std::unique_ptr<FallbackFace>> fallback_faces;
    std::unordered_map<std::uint32_t, FT_Face>                     fallback_by_codepoint;
    std::unordered_set<std::uint32_t>                              fallback_misses;

    // Pixel-coord rects pushed by Populate() — drained once per frame by
    // the renderer to vkCmdCopyBufferToImage just the changed regions.
    std::vector<AtlasDirtyRect> dirty_rects;

    // Set by FontCache::GetFace; consumed by the renderer's per-frame
    // atlas-commit hook to look the face's VkImage up by URL.
    std::string atlas_url;

    ~Impl() {
        if (face != nullptr) FT_Done_Face(face);
    }

    Impl() {
        atlas.assign(static_cast<std::size_t>(atlas_w) * atlas_h, 0);
        SeedWhiteCell();
    }

    void ResetAtlas(std::uint32_t dim) {
        atlas_w = dim;
        atlas_h = dim;
        atlas.assign(static_cast<std::size_t>(atlas_w) * atlas_h, 0);
        dirty_rects.clear();
        glyphs.clear();
        pen_x   = 0;
        pen_y   = 0;
        shelf_h = 0;
        SeedWhiteCell();
    }

    void SeedWhiteCell() {
        for (std::uint32_t y = 0; y < kWhiteCellSize; ++y) {
            for (std::uint32_t x = 0; x < kWhiteCellSize; ++x) {
                atlas[y * atlas_w + x] = 0xFF;
            }
        }
        pen_x   = kWhiteCellSize + 1;
        pen_y   = 0;
        shelf_h = kWhiteCellSize;
        dirty_rects.push_back({ 0, 0, kWhiteCellSize, kWhiteCellSize });
    }

    bool ReserveSlot(std::uint32_t w, std::uint32_t h, std::uint32_t& out_x, std::uint32_t& out_y) {
        if (pen_x + w > atlas_w) {
            pen_y += shelf_h + 1;
            pen_x   = 0;
            shelf_h = 0;
        }
        if (pen_y + h > atlas_h) return false;
        out_x = pen_x;
        out_y = pen_y;
        pen_x += w + 1;
        if (h > shelf_h) shelf_h = h;
        return true;
    }

    FT_Face ResolveFallbackFace(std::uint32_t codepoint) {
        if (auto it = fallback_by_codepoint.find(codepoint); it != fallback_by_codepoint.end()) {
            return it->second;
        }
        if (fallback_misses.count(codepoint) != 0) return nullptr;

        auto path = ResolveFontconfigCodepoint(codepoint);
        if (path.empty()) {
            fallback_misses.insert(codepoint);
            return nullptr;
        }

        std::string key = path.string();
        auto        it  = fallback_faces.find(key);
        if (it == fallback_faces.end()) {
            auto bytes = ReadAll(path);
            if (! bytes) {
                fallback_misses.insert(codepoint);
                return nullptr;
            }

            auto       fallback = std::make_unique<FallbackFace>();
            FT_Library lib      = FtLibrary::Get().handle();
            if (lib == nullptr ||
                FT_New_Memory_Face(lib,
                                   reinterpret_cast<const FT_Byte*>(bytes->data()),
                                   static_cast<FT_Long>(bytes->size()),
                                   0,
                                   &fallback->face) != 0 ||
                FT_Set_Pixel_Sizes(fallback->face, 0, pixel_size) != 0) {
                fallback_misses.insert(codepoint);
                return nullptr;
            }
            fallback->blob = std::move(bytes);
            it             = fallback_faces.emplace(std::move(key), std::move(fallback)).first;
        }

        FT_Face fallback_face = it->second->face;
        if (fallback_face == nullptr || FT_Get_Char_Index(fallback_face, codepoint) == 0) {
            fallback_misses.insert(codepoint);
            return nullptr;
        }
        fallback_by_codepoint.emplace(codepoint, fallback_face);
        return fallback_face;
    }

    void Blit(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
              const std::uint8_t* src, std::uint32_t pitch) {
        for (std::uint32_t row = 0; row < h; ++row) {
            std::memcpy(&atlas[(y + row) * atlas_w + x], src + row * pitch, w);
        }
    }
};

FontFace::FontFace(): m_impl(std::make_unique<Impl>()) {}
FontFace::~FontFace()                              = default;
FontFace::FontFace(FontFace&&) noexcept            = default;
FontFace& FontFace::operator=(FontFace&&) noexcept = default;

FontMetrics FontFace::Metrics() const {
    FontMetrics m {};
    if (m_impl->face != nullptr && m_impl->face->size != nullptr) {
        const auto& sm = m_impl->face->size->metrics;
        m.ascender     = static_cast<float>(sm.ascender) / 64.0f;
        m.descender    = static_cast<float>(sm.descender) / 64.0f;
        m.line_height  = static_cast<float>(sm.height) / 64.0f;
        m.pixel_size   = m_impl->pixel_size;
    }
    m.atlas_w = m_impl->atlas_w;
    m.atlas_h = m_impl->atlas_h;
    return m;
}

std::span<const std::uint8_t> FontFace::AtlasPixels() const {
    return std::span<const std::uint8_t>(m_impl->atlas);
}

std::span<const AtlasDirtyRect> FontFace::DirtyRects() const noexcept {
    return m_impl->dirty_rects;
}
void               FontFace::ClearDirtyRects() noexcept { m_impl->dirty_rects.clear(); }
const std::string& FontFace::AtlasUrl() const noexcept { return m_impl->atlas_url; }

const GlyphInfo* FontFace::Lookup(std::uint32_t codepoint) const noexcept {
    auto& impl = *m_impl;
    if (auto it = impl.glyphs.find(codepoint); it != impl.glyphs.end()) {
        return &it->second;
    }
    return nullptr;
}

void FontFace::Populate(std::span<const std::uint32_t> codepoints) {
    auto& impl = *m_impl;
    if (impl.face == nullptr) return;
    for (std::uint32_t codepoint : codepoints) {
        if (impl.glyphs.find(codepoint) != impl.glyphs.end()) continue;

        FT_Face render_face = impl.face;
        FT_UInt glyph_index = FT_Get_Char_Index(render_face, codepoint);
        if (glyph_index == 0 && ! IsLayoutWhitespace(codepoint)) {
            render_face = impl.ResolveFallbackFace(codepoint);
            glyph_index = render_face != nullptr ? FT_Get_Char_Index(render_face, codepoint) : 0;
        }
        if (glyph_index == 0) {
            // No renderable glyph (whitespace the primary face lacks, or a
            // codepoint no fallback face covers). Reserve the primary face's
            // .notdef (glyph 0) advance so the codepoint still occupies layout
            // width — matching the pre-fallback port behavior — but never
            // rasterize a tofu box for it.
            GlyphInfo gi {};
            if (FT_Load_Glyph(impl.face, 0, FT_LOAD_DEFAULT) == 0) {
                gi.advance_x = static_cast<float>(impl.face->glyph->advance.x) / 64.0f;
            }
            impl.glyphs.emplace(codepoint, gi);
            continue;
        }

        if (FT_Load_Glyph(render_face, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
            continue;
        }
        FT_GlyphSlot g = render_face->glyph;
        GlyphInfo    gi {};
        gi.pixel_w   = g->bitmap.width;
        gi.pixel_h   = g->bitmap.rows;
        gi.bearing_x = static_cast<float>(g->bitmap_left);
        gi.bearing_y = static_cast<float>(g->bitmap_top);
        gi.advance_x = static_cast<float>(g->advance.x) / 64.0f;

        if (gi.pixel_w == 0 || gi.pixel_h == 0) {
            gi.atlas_x = 0;
            gi.atlas_y = 0;
            impl.glyphs.emplace(codepoint, gi);
            continue;
        }

        if (! impl.ReserveSlot(gi.pixel_w, gi.pixel_h, gi.atlas_x, gi.atlas_y)) {
            // Atlas full → tofu mapped to the white cell. Cache the fallback so
            // we don't FT_Load the same codepoint every frame.
            gi.atlas_x = 0;
            gi.atlas_y = 0;
            gi.pixel_w = kWhiteCellSize;
            gi.pixel_h = kWhiteCellSize;
            impl.glyphs.emplace(codepoint, gi);
            continue;
        }

        impl.Blit(gi.atlas_x,
                  gi.atlas_y,
                  gi.pixel_w,
                  gi.pixel_h,
                  g->bitmap.buffer,
                  static_cast<std::uint32_t>(g->bitmap.pitch));
        impl.dirty_rects.push_back({ gi.atlas_x, gi.atlas_y, gi.pixel_w, gi.pixel_h });
        impl.glyphs.emplace(codepoint, gi);
    }
}

// -- FontCache ------------------------------------------------------------

struct FontCache::Impl {
    struct Key {
        std::uint64_t blob_hash;
        std::uint32_t pixel_size;
        bool          operator==(const Key& o) const noexcept {
            return blob_hash == o.blob_hash && pixel_size == o.pixel_size;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return std::hash<std::uint64_t> {}(k.blob_hash) ^
                   (std::hash<std::uint32_t> {}(k.pixel_size) << 1);
        }
    };
    std::unordered_map<Key, std::unique_ptr<FontFace>, KeyHash> faces;
};

FontCache::FontCache(): m_impl(std::make_unique<Impl>()) {}
FontCache::~FontCache() = default;

FontFace* FontCache::GetFace(std::shared_ptr<std::vector<std::byte>> blob,
                             std::uint32_t                           pixel_size) {
    if (! blob || blob->empty() || pixel_size == 0) return nullptr;

    auto      blob_span = std::span<const std::byte>(blob->data(), blob->size());
    auto      blob_hash = HashBlob(blob_span);
    Impl::Key key { blob_hash, pixel_size };
    if (auto it = m_impl->faces.find(key); it != m_impl->faces.end()) {
        return it->second.get();
    }

    FT_Library lib = FtLibrary::Get().handle();
    if (lib == nullptr) return nullptr;

    auto face = std::make_unique<FontFace>();
    if (FT_New_Memory_Face(lib,
                           reinterpret_cast<const FT_Byte*>(blob_span.data()),
                           static_cast<FT_Long>(blob_span.size()),
                           0,
                           &face->m_impl->face) != 0) {
        rstd_error("FT_New_Memory_Face failed");
        return nullptr;
    }
    if (FT_Set_Pixel_Sizes(face->m_impl->face, 0, pixel_size) != 0) {
        rstd_error("FT_Set_Pixel_Sizes failed (px={})", pixel_size);
        return nullptr;
    }
    // Keep the bytes alive for the face's lifetime: FT_Face holds raw
    // pointers into this buffer and dereferences them on every glyph load.
    face->m_impl->blob       = std::move(blob);
    face->m_impl->pixel_size = pixel_size;
    face->m_impl->ResetAtlas(AtlasDimForPixelSize(pixel_size));
    face->m_impl->atlas_url =
        "_text_atlas_" + std::to_string(blob_hash) + "_" + std::to_string(pixel_size);

    auto* raw          = face.get();
    m_impl->faces[key] = std::move(face);
    return raw;
}

std::vector<FontFace*> FontCache::Faces() const {
    std::vector<FontFace*> out;
    out.reserve(m_impl->faces.size());
    for (auto& [_k, f] : m_impl->faces) out.push_back(f.get());
    return out;
}

FontCache& EnsureSceneFontCache(sr::Scene& scene) {
    if (! scene.font_cache) {
        scene.font_cache = { new FontCache(), [](void* p) noexcept {
                                delete static_cast<FontCache*>(p);
                            } };
    }
    return *static_cast<FontCache*>(scene.font_cache.get());
}
FontCache* SceneFontCache(sr::Scene& scene) noexcept {
    return static_cast<FontCache*>(scene.font_cache.get());
}

// WE references system fonts as `systemfont_<lowercased-windows-name>`,
// e.g. `systemfont_arial`. Fontconfig has an alias table that maps Windows
// family names (Arial, Courier New, ...) to whatever the user actually has
// installed. Strip the prefix and ask fc.
static std::filesystem::path ResolveViaFontconfig(std::string_view name) {
    constexpr std::string_view kPrefix = "systemfont_";
    // Match on the basename — scenes occasionally prefix a dir.
    std::string base = std::filesystem::path(name).filename().native();
    if (base.size() <= kPrefix.size() ||
        std::string_view(base).substr(0, kPrefix.size()).compare(kPrefix) != 0) {
        return {};
    }
    std::string family = base.substr(kPrefix.size());
    if (family.empty()) return {};
    family[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(family[0])));

    if (! FcInit()) return {};
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(family.c_str()));
    if (pat == nullptr) return {};
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult   res   = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pat, &res);
    FcPatternDestroy(pat);
    if (match == nullptr || res != FcResultMatch) {
        if (match != nullptr) FcPatternDestroy(match);
        return {};
    }
    FcChar8*              file = nullptr;
    std::filesystem::path out;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
        out = std::filesystem::path(reinterpret_cast<const char*>(file));
    }
    FcPatternDestroy(match);
    return out;
}

FontCache::ResolvedBlob FontCache::ResolveSystemFont(std::string_view name, bool fallback_to_any) {
    namespace fs = std::filesystem;

    auto try_load = [](const fs::path& p) -> ResolvedBlob {
        if (! fs::exists(p) || ! fs::is_regular_file(p)) return { nullptr, {} };
        auto bytes = ReadAll(p);
        if (! bytes) return { nullptr, {} };
        return { std::move(bytes), p.string() };
    };

    if (! name.empty()) {
        // Direct path?
        if (auto rb = try_load(fs::path(name)); rb.bytes) return rb;
        // WE's systemfont_<family> alias → fontconfig.
        if (auto p = ResolveViaFontconfig(name); ! p.empty()) {
            if (auto rb = try_load(p); rb.bytes) return rb;
        }
        // Bare filename: search common roots.
        // macOS system font directories. /Library/Fonts is user-installed,
        // /System/Library/Fonts is the OS font set.
        std::vector<fs::path> roots {
            "/System/Library/Fonts",
            "/System/Library/Fonts/Supplemental",
            "/Library/Fonts",
        };
        if (auto* home = std::getenv("HOME"); home != nullptr) {
            roots.emplace_back(fs::path(home) / "Library/Fonts");
        }
        std::string           base    = fs::path(name).filename().string();
        std::size_t           scanned = 0;
        constexpr std::size_t kCap    = 8192;
        for (const auto& root : roots) {
            if (! fs::exists(root)) continue;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(
                     root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (++scanned > kCap) break;
                if (! it->is_regular_file(ec)) continue;
                if (it->path().filename() == base) {
                    if (auto rb = try_load(it->path()); rb.bytes) return rb;
                }
            }
            if (scanned > kCap) break;
        }
    }

    if (! fallback_to_any) return { nullptr, {} };

    // Last-resort fallback: any .ttf/.otf in the system roots.
    std::vector<fs::path> roots {
        "/System/Library/Fonts",
        "/System/Library/Fonts/Supplemental",
        "/Library/Fonts",
    };
    if (auto* home = std::getenv("HOME"); home != nullptr) {
        roots.emplace_back(fs::path(home) / "Library/Fonts");
    }
    std::size_t           scanned = 0;
    constexpr std::size_t kCap    = 4096;
    for (const auto& root : roots) {
        if (! fs::exists(root)) continue;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (++scanned > kCap) break;
            if (! it->is_regular_file(ec)) continue;
            if (IsFontExt(it->path())) {
                if (auto rb = try_load(it->path()); rb.bytes) return rb;
            }
        }
        if (scanned > kCap) break;
    }
    return { nullptr, {} };
}

// -- Atlas snapshot -------------------------------------------------------

std::shared_ptr<sr::Image> BuildAtlasImage(const FontFace& face, const std::string& key) {
    auto fm  = face.Metrics();
    auto pix = face.AtlasPixels();
    if (fm.atlas_w == 0 || fm.atlas_h == 0 || pix.empty()) return nullptr;

    auto img = std::make_shared<sr::Image>();
    img->key = key;

    img->header.width         = static_cast<sr::i32>(fm.atlas_w);
    img->header.height        = static_cast<sr::i32>(fm.atlas_h);
    img->header.mapWidth      = img->header.width;
    img->header.mapHeight     = img->header.height;
    img->header.mipmap_larger = false;
    img->header.mipmap_pow2   = false;
    img->header.type          = sr::ImageType::UNKNOWN;
    img->header.format        = sr::TextureFormat::R8;
    img->header.count         = 1;
    img->header.isSprite      = false;
    img->header.sample        = { sr::TextureWrap::CLAMP_TO_EDGE,
                                  sr::TextureWrap::CLAMP_TO_EDGE,
                                  sr::TextureFilter::LINEAR,
                                  sr::TextureFilter::LINEAR };

    img->slots.resize(1);
    auto& slot  = img->slots[0];
    slot.width  = img->header.width;
    slot.height = img->header.height;
    slot.mipmaps.resize(1);
    auto& mip  = slot.mipmaps[0];
    mip.width  = img->header.width;
    mip.height = img->header.height;
    mip.size   = static_cast<sr::isize>(pix.size());

    // Alias the face's live CPU atlas (no memcpy). The renderer's first
    // CreateTex call samples whatever pixels are present at that moment, so
    // glyphs the actuator Populated between parse-time and the first draw
    // are picked up. The face is scene-owned and outlives the Image.
    mip.data = sr::ImageDataPtr(const_cast<std::uint8_t*>(pix.data()), [](std::uint8_t*) noexcept {
    });

    return img;
}

// -- Text shader ----------------------------------------------------------

namespace
{

constexpr const char* kTextShaderHlsl = R"hlsl(
[[vk::binding(0, 0)]] cbuffer ww_Uniforms {
    column_major float4x4 g_ModelViewProjectionMatrix;
};

struct VSInput {
    float3 a_Position : a_Position;
    float2 a_TexCoord : a_TexCoord;
    float4 a_Color    : a_Color;
};
struct PSInput {
    float4 sv_pos : SV_Position;
    float2 v_uv   : TEXCOORD0;
    float4 v_col  : COLOR0;
};

PSInput main_vs(VSInput i) {
    PSInput o;
    o.sv_pos = mul(g_ModelViewProjectionMatrix, float4(i.a_Position, 1.0));
    o.v_uv   = i.a_TexCoord;
    o.v_col  = i.a_Color;
    return o;
}

[[vk::combinedImageSampler]][[vk::binding(1, 0)]]
Texture2D<float4> g_Texture0;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]]
SamplerState g_Texture0_sampler;

float4 main_ps(PSInput i) : SV_Target {
    float a = g_Texture0.Sample(g_Texture0_sampler, i.v_uv).r;
    return float4(i.v_col.rgb, i.v_col.a * a);
}
)hlsl";

constexpr const char* kTextCopyBackgroundShaderHlsl = R"hlsl(
[[vk::binding(0, 0)]] cbuffer ww_Uniforms {
    column_major float4x4 g_ModelViewProjectionMatrix;
    column_major float4x4 g_EffectModelViewProjectionMatrix;
};

struct VSInput {
    float3 a_Position : a_Position;
    float2 a_TexCoord : a_TexCoord;
};
struct PSInput {
    float4 sv_pos : SV_Position;
    float4 v_proj : TEXCOORD0;
};

PSInput main_vs(VSInput i) {
    float4 pos = float4(i.a_Position, 1.0);
    PSInput o;
    o.sv_pos = mul(g_ModelViewProjectionMatrix, pos);
    o.v_proj = mul(g_EffectModelViewProjectionMatrix, pos);
    return o;
}

[[vk::combinedImageSampler]][[vk::binding(1, 0)]]
Texture2D<float4> g_Texture0;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]]
SamplerState g_Texture0_sampler;

float4 main_ps(PSInput i) : SV_Target {
    float2 uv = (i.v_proj.xy / i.v_proj.w) * 0.5 + 0.5;
    return float4(g_Texture0.Sample(g_Texture0_sampler, uv).rgb, 0.0);
}
)hlsl";

std::shared_ptr<sr::SceneShader> CompileInlineShader(std::string_view name,
                                                      std::string_view source) {
    using namespace sr::vulkan;
    std::string src(source);

    std::array<ShaderCompUnit, 2> units {
        ShaderCompUnit { sr::ShaderType::VERTEX, src, "main_vs", SourceLang::Hlsl },
        ShaderCompUnit { sr::ShaderType::FRAGMENT, src, "main_ps", SourceLang::Hlsl },
    };
    ShaderCompOpt opt {};
    opt.target   = VulkanTarget::Vulkan_1_1;
    opt.optimize = false;

    std::vector<Uni_ShaderSpv> spvs;
    if (! CompileAndLinkShaderUnits(units, opt, spvs)) {
        rstd_error("{} shader compile failed", name);
        return nullptr;
    }

    auto shader  = std::make_shared<sr::SceneShader>();
    shader->id   = 0;
    shader->name = std::string(name);
    shader->codes.reserve(spvs.size());
    for (auto& spv : spvs) {
        shader->codes.emplace_back(std::move(spv->spirv));
    }
    return shader;
}

} // namespace

std::shared_ptr<sr::SceneShader> GetTextSceneShader() {
    static std::once_flag                    once;
    static std::shared_ptr<sr::SceneShader> shader;
    std::call_once(once, [] {
        shader = CompileInlineShader("text", kTextShaderHlsl);
    });
    return shader;
}

std::shared_ptr<sr::SceneShader> GetTextCopyBackgroundSceneShader() {
    static std::once_flag                    once;
    static std::shared_ptr<sr::SceneShader> shader;
    std::call_once(once, [] {
        shader = CompileInlineShader("text_copybackground", kTextCopyBackgroundShaderHlsl);
    });
    return shader;
}

// -- TextLayouter ---------------------------------------------------------

namespace
{

struct TextLineRunGI {
    std::vector<const GlyphInfo*> glyphs;
    float                         width { 0.0f };
};

bool ContainsSubstring(std::string_view s, std::string_view what) noexcept {
    return s.find(what) != std::string::npos;
}

} // namespace

struct TextLayouter::Impl {
    FontFace*                       face { nullptr };
    std::shared_ptr<sr::SceneMesh> mesh;
    TextLayoutStyle                 style;
    std::size_t                     peak_quads { 0 };
    FontMetrics                     metrics;

    float       last_text_w { 0.0f };
    float       last_text_h { 0.0f };
    float       last_source_w { 0.0f };
    float       last_source_h { 0.0f };
    float       last_source_center_x { 0.0f };
    float       last_source_center_y { 0.0f };
    std::string current_text;
    bool        missing_glyph_logged { false };
    bool        truncate_logged { false };

    // Scratch buffers reused across SetText calls — avoids reallocs for
    // every script tick. Sized at construction to peak capacity.
    std::vector<float>         positions;
    std::vector<float>         texcoords;
    std::vector<float>         colors;
    std::vector<std::uint32_t> indices;

    Impl(FontFace* f, std::shared_ptr<sr::SceneMesh> m, TextLayoutStyle s, std::size_t pq)
        : face(f),
          mesh(std::move(m)),
          style(std::move(s)),
          peak_quads(pq),
          metrics(face->Metrics()) {
        positions.assign(pq * 4 * 3, 0.0f);
        texcoords.assign(pq * 4 * 2, 0.0f);
        colors.assign(pq * 4 * 4, 0.0f);
        indices.assign(pq * 6, 0u);
    }
};

TextLayouter::TextLayouter(FontFace* face, std::shared_ptr<sr::SceneMesh> mesh,
                           TextLayoutStyle style, std::size_t peak_quads)
    : m_impl(std::make_unique<Impl>(face, std::move(mesh), std::move(style), peak_quads)) {}

TextLayouter::~TextLayouter() = default;

float             TextLayouter::TextWidth() const noexcept { return m_impl->last_text_w; }
float             TextLayouter::TextHeight() const noexcept { return m_impl->last_text_h; }
float             TextLayouter::SourceWidth() const noexcept { return m_impl->last_source_w; }
float             TextLayouter::SourceHeight() const noexcept { return m_impl->last_source_h; }
FontFace*         TextLayouter::Face() const noexcept { return m_impl->face; }
TextLayoutMetrics TextLayouter::Metrics() const noexcept {
    return {
        .text_width      = m_impl->last_text_w,
        .text_height     = m_impl->last_text_h,
        .source_width    = m_impl->last_source_w,
        .source_height   = m_impl->last_source_h,
        .source_center_x = m_impl->last_source_center_x,
        .source_center_y = m_impl->last_source_center_y,
        .padding         = m_impl->style.padding,
    };
}

void TextLayouter::SetFace(FontFace* face) {
    auto& im = *m_impl;
    if (face == nullptr || face == im.face) return;
    im.face                 = face;
    im.metrics              = face->Metrics();
    im.missing_glyph_logged = false;
    im.truncate_logged      = false;
    SetText(im.current_text);
}

void TextLayouter::SetColor(float r, float g, float b) {
    auto& im = *m_impl;
    if (im.style.color[0] == r && im.style.color[1] == g && im.style.color[2] == b) return;
    im.style.color = { r, g, b };
    // Re-run the layout so every glyph quad picks up the new vertex color.
    SetText(im.current_text);
}

void TextLayouter::SetAlpha(float alpha) {
    auto& im = *m_impl;
    if (im.style.alpha == alpha) return;
    im.style.alpha = alpha;
    SetText(im.current_text);
}

TextGeometry ResolveTextGeometry(const TextGeometryPolicy& policy,
                                 const TextLayoutMetrics&  metrics) {
    auto positive = [](float value, float fallback) {
        return value > 0.0f ? value : fallback;
    };
    const float frame_w = positive(policy.frame_width, 1.0f);
    const float frame_h = positive(policy.frame_height, 1.0f);
    const float text_w  = positive(metrics.text_width, 1.0f);
    const float text_h  = positive(metrics.text_height, 1.0f);
    const float src_w   = positive(metrics.source_width, text_w);
    const float src_h   = positive(metrics.source_height, text_h);
    const float pad     = std::max(0.0f, metrics.padding);
    const float src_cx  = std::isfinite(metrics.source_center_x) ? metrics.source_center_x : 0.0f;
    const float src_cy  = std::isfinite(metrics.source_center_y) ? metrics.source_center_y : 0.0f;

    const float text_bbox_w = text_w + 2.0f * pad;
    const float text_bbox_h = text_h + 2.0f * pad;
    const float src_bbox_w  = src_w + 2.0f * pad;
    const float src_bbox_h  = src_h + 2.0f * pad;

    const float dynamic_w      = std::max({ src_bbox_w, text_bbox_w, frame_w * 3.0f, 1024.0f });
    const float dynamic_h      = std::max({ src_bbox_h, text_bbox_h, frame_h * 2.0f, 256.0f });
    const bool  dynamic_effect = policy.dynamic && policy.has_effect;

    const float rt_max_w = policy.dynamic ? (! policy.has_effect ? dynamic_w : frame_w)
                                          : (policy.has_effect ? frame_w : src_bbox_w);
    const float rt_max_h = policy.dynamic ? (! policy.has_effect ? dynamic_h : frame_h)
                                          : (policy.has_effect ? frame_h : src_bbox_h);

    TextGeometry out;
    out.rt_width            = std::max(src_bbox_w, rt_max_w);
    out.rt_height           = std::max(src_bbox_h, rt_max_h);
    out.effect_frame_width  = frame_w;
    out.effect_frame_height = frame_h;

    if (! policy.has_effect) {
        const bool tight_bbox = ! policy.preserve_text_bbox;
        out.draw_width        = tight_bbox ? src_w : text_bbox_w;
        out.draw_height       = tight_bbox ? src_h : text_bbox_h;
        out.draw_offset_x     = tight_bbox ? src_cx : 0.0f;
        out.draw_offset_y     = tight_bbox ? src_cy : 0.0f;
        out.uv_source_width   = tight_bbox ? src_w : src_bbox_w;
        out.uv_source_height  = tight_bbox ? src_h : src_bbox_h;
        return out;
    }

    if (dynamic_effect) {
        out.draw_width          = std::max(frame_w, src_bbox_w);
        out.draw_height         = frame_h;
        out.uv_source_width     = out.draw_width;
        out.uv_source_height    = std::max(frame_h, src_bbox_h);
        out.effect_frame_width  = out.uv_source_width;
        out.effect_frame_height = out.uv_source_height;
        return out;
    }

    out.draw_width       = frame_w;
    out.draw_height      = frame_h;
    out.uv_source_width  = frame_w;
    out.uv_source_height = frame_h;
    return out;
}

void TextLayouter::SetText(std::string_view utf8) {
    auto& im = *m_impl;

    std::string next_text(utf8);
    im.current_text = std::move(next_text);
    auto codepoints = DecodeUtf8(im.current_text);

    // Split into lines and look up pre-rasterised glyph metrics.
    std::vector<TextLineRunGI> lines;
    lines.emplace_back();
    std::size_t total_glyph_quads = 0;
    for (std::uint32_t cp : codepoints) {
        if (cp == '\n') {
            lines.emplace_back();
            continue;
        }
        const auto* gi = im.face->Lookup(cp);
        if (gi == nullptr) {
            // Actuator is expected to Populate() before SetText, so this only
            // fires for codepoints that genuinely failed to rasterise (e.g.
            // missing in the font). Log-once keeps log noise bounded.
            if (! im.missing_glyph_logged) {
                rstd_info("text: codepoint U+{:04X} not rasterised, skipping", cp);
                im.missing_glyph_logged = true;
            }
            continue;
        }
        lines.back().glyphs.push_back(gi);
        lines.back().width += gi->advance_x;
        ++total_glyph_quads;
    }

    bool        has_bg      = im.style.opaquebackground;
    std::size_t total_quads = total_glyph_quads + (has_bg ? 1u : 0u);
    if (total_quads > im.peak_quads) {
        // Off-RT overflow only — the layouter emits top-to-bottom, so the
        // dropped tail quads are below the layer RT's visible window. Log-once
        // keeps a runaway terminal/log script from spamming every frame.
        if (! im.truncate_logged) {
            rstd_info("text: {} quads exceed peak capacity {}, truncating tail",
                      total_quads,
                      im.peak_quads);
            im.truncate_logged = true;
        }
        total_quads = im.peak_quads;
        if (has_bg && total_glyph_quads + 1 > im.peak_quads)
            total_glyph_quads = im.peak_quads - 1;
        else if (! has_bg)
            total_glyph_quads = total_quads;
    }

    auto& fm     = im.metrics;
    float text_w = 0.0f;
    for (auto& l : lines)
        if (l.width > text_w) text_w = l.width;
    float text_h =
        fm.ascender - fm.descender + static_cast<float>(lines.size() - 1) * fm.line_height;
    im.last_text_w          = text_w;
    im.last_text_h          = text_h;
    im.last_source_w        = text_w;
    im.last_source_h        = text_h;
    im.last_source_center_x = 0.0f;
    im.last_source_center_y = 0.0f;

    // Zero the unused tail so stale data from the previous (longer) text
    // doesn't show up. Cheaper than tracking exact quad count downstream.
    std::fill(im.positions.begin(), im.positions.end(), 0.0f);
    std::fill(im.texcoords.begin(), im.texcoords.end(), 0.0f);
    std::fill(im.colors.begin(), im.colors.end(), 0.0f);
    std::fill(im.indices.begin(), im.indices.end(), 0u);

    auto write_quad = [&](std::size_t                 q_idx,
                          float                       left,
                          float                       right,
                          float                       bottom,
                          float                       top,
                          float                       u_l,
                          float                       u_r,
                          float                       v_t,
                          float                       v_b,
                          const std::array<float, 4>& rgba) {
        std::size_t v_off     = q_idx * 4;
        const float pos[4][3] = {
            { left, top, 0.0f },
            { right, top, 0.0f },
            { right, bottom, 0.0f },
            { left, bottom, 0.0f },
        };
        const float uv[4][2] = {
            { u_l, v_t },
            { u_r, v_t },
            { u_r, v_b },
            { u_l, v_b },
        };
        for (std::size_t k = 0; k < 4; ++k) {
            std::memcpy(&im.positions[(v_off + k) * 3], pos[k], sizeof(pos[k]));
            std::memcpy(&im.texcoords[(v_off + k) * 2], uv[k], sizeof(uv[k]));
            std::memcpy(&im.colors[(v_off + k) * 4], rgba.data(), sizeof(float) * 4);
        }
        std::size_t         i_off = q_idx * 6;
        const std::uint32_t base  = static_cast<std::uint32_t>(v_off);
        im.indices[i_off + 0]     = base + 0;
        im.indices[i_off + 1]     = base + 1;
        im.indices[i_off + 2]     = base + 2;
        im.indices[i_off + 3]     = base + 0;
        im.indices[i_off + 4]     = base + 2;
        im.indices[i_off + 5]     = base + 3;
    };

    float text_top    = +text_h * 0.5f;
    float text_bottom = -text_h * 0.5f;
    float text_left   = -text_w * 0.5f;
    float text_right  = +text_w * 0.5f;
    (void)text_left;
    (void)text_right;
    (void)text_bottom;
    float pad = im.style.padding;

    std::size_t q = 0;

    if (has_bg) {
        float                u_l = 1.0f / static_cast<float>(fm.atlas_w);
        float                u_r = 3.0f / static_cast<float>(fm.atlas_w);
        float                v_t = 1.0f / static_cast<float>(fm.atlas_h);
        float                v_b = 3.0f / static_cast<float>(fm.atlas_h);
        std::array<float, 4> rgba {
            im.style.background_color[0] * im.style.background_brightness,
            im.style.background_color[1] * im.style.background_brightness,
            im.style.background_color[2] * im.style.background_brightness,
            1.0f,
        };
        write_quad(q++,
                   -text_w * 0.5f - pad,
                   +text_w * 0.5f + pad,
                   -text_h * 0.5f - pad,
                   +text_h * 0.5f + pad,
                   u_l,
                   u_r,
                   v_t,
                   v_b,
                   rgba);
    }

    std::array<float, 4> text_rgba {
        im.style.color[0],
        im.style.color[1],
        im.style.color[2],
        im.style.alpha,
    };

    std::size_t emitted_glyphs       = 0;
    bool        have_glyph_bounds    = false;
    float       glyph_min_x          = 0.0f;
    float       glyph_max_x          = 0.0f;
    float       glyph_min_y          = 0.0f;
    float       glyph_max_y          = 0.0f;
    auto        include_glyph_bounds = [&](float left, float right, float bottom, float top) {
        if (! have_glyph_bounds) {
            glyph_min_x       = left;
            glyph_max_x       = right;
            glyph_min_y       = bottom;
            glyph_max_y       = top;
            have_glyph_bounds = true;
            return;
        }
        glyph_min_x = std::min(glyph_min_x, left);
        glyph_max_x = std::max(glyph_max_x, right);
        glyph_min_y = std::min(glyph_min_y, bottom);
        glyph_max_y = std::max(glyph_max_y, top);
    };
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const auto& line = lines[li];
        float       line_origin_x;
        if (ContainsSubstring(im.style.halign, "left")) {
            line_origin_x = -text_w * 0.5f;
        } else if (ContainsSubstring(im.style.halign, "right")) {
            line_origin_x = +text_w * 0.5f - line.width;
        } else {
            line_origin_x = -line.width * 0.5f;
        }
        float baseline_y = text_top - fm.ascender - static_cast<float>(li) * fm.line_height;

        float pen_x = line_origin_x;
        for (auto* gi : line.glyphs) {
            if (q >= im.peak_quads) break;
            if (gi->pixel_w == 0 || gi->pixel_h == 0) {
                pen_x += gi->advance_x;
                ++emitted_glyphs;
                continue;
            }
            float left   = pen_x + gi->bearing_x;
            float right  = left + static_cast<float>(gi->pixel_w);
            float top    = baseline_y + gi->bearing_y;
            float bottom = top - static_cast<float>(gi->pixel_h);
            float u_l    = static_cast<float>(gi->atlas_x) / static_cast<float>(fm.atlas_w);
            float u_r =
                static_cast<float>(gi->atlas_x + gi->pixel_w) / static_cast<float>(fm.atlas_w);
            float v_t = static_cast<float>(gi->atlas_y) / static_cast<float>(fm.atlas_h);
            float v_b =
                static_cast<float>(gi->atlas_y + gi->pixel_h) / static_cast<float>(fm.atlas_h);
            write_quad(q++, left, right, bottom, top, u_l, u_r, v_t, v_b, text_rgba);
            include_glyph_bounds(left, right, bottom, top);
            pen_x += gi->advance_x;
            ++emitted_glyphs;
            if (emitted_glyphs >= total_glyph_quads) break;
        }
        if (emitted_glyphs >= total_glyph_quads) break;
    }

    if (have_glyph_bounds) {
        im.last_source_w               = std::max(1.0f, glyph_max_x - glyph_min_x);
        im.last_source_h               = std::max(1.0f, glyph_max_y - glyph_min_y);
        im.last_source_center_x        = 0.5f * (glyph_min_x + glyph_max_x);
        im.last_source_center_y        = 0.5f * (glyph_min_y + glyph_max_y);
        const float       shift_x      = -im.last_source_center_x;
        const float       shift_y      = -im.last_source_center_y;
        const std::size_t vertex_count = q * 4;
        for (std::size_t i = 0; i < vertex_count; ++i) {
            im.positions[i * 3 + 0] += shift_x;
            im.positions[i * 3 + 1] += shift_y;
        }
    }

    // Push into the mesh. Vertex array's stride is interleaved with padding
    // already laid out by SceneVertexArray; SetVertex scatters by name.
    auto& v = im.mesh->GetVertexArray(0);
    v.SetVertex(WE_IN_POSITION, im.positions);
    v.SetVertex(WE_IN_TEXCOORD, im.texcoords);
    v.SetVertex(WE_IN_COLOR, im.colors);

    auto& idx = im.mesh->GetIndexArray(0);
    idx.Assign(0, im.indices);
    // Render only the indices we actually populated (rest are zeroed out
    // and reference vertex 0, which is harmless but wastes draw calls).
    idx.SetRenderDataCount(q * 6);

    im.mesh->SetDirty();
}

void TextLayouter::SetHorizontalAlign(std::string_view align) {
    auto& im        = *m_impl;
    im.style.halign = std::string(align);
    SetText(im.current_text);
}

} // namespace sr::text

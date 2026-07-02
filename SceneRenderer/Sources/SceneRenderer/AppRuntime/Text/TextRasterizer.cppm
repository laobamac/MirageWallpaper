module;

export module sr.text;
import sr.types;
import rstd.cppstd;
import sr.scene;

export namespace sr::text
{

inline std::vector<std::uint32_t> DecodeUtf8(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint8_t  b0   = static_cast<std::uint8_t>(s[i]);
        std::uint32_t cp   = 0;
        std::size_t   need = 0;
        if (b0 < 0x80) {
            cp   = b0;
            need = 0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp   = b0 & 0x1Fu;
            need = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp   = b0 & 0x0Fu;
            need = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp   = b0 & 0x07u;
            need = 3;
        } else {
            out.push_back(0xFFFDu);
            ++i;
            continue;
        }
        if (i + need >= s.size()) {
            out.push_back(0xFFFDu);
            break;
        }
        bool ok = true;
        for (std::size_t j = 1; j <= need; ++j) {
            std::uint8_t bj = static_cast<std::uint8_t>(s[i + j]);
            if ((bj & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (bj & 0x3Fu);
        }
        if (! ok) {
            out.push_back(0xFFFDu);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += 1 + need;
    }
    return out;
}

struct GlyphInfo {
    // Position inside the atlas, pixels.
    std::uint32_t atlas_x { 0 };
    std::uint32_t atlas_y { 0 };
    std::uint32_t pixel_w { 0 };
    std::uint32_t pixel_h { 0 };
    // FreeType bearings + advance, fractional pixels.
    float bearing_x { 0.0f };
    float bearing_y { 0.0f };
    float advance_x { 0.0f };
};

struct FontMetrics {
    float         ascender { 0.0f };
    float         descender { 0.0f };
    float         line_height { 0.0f };
    std::uint32_t pixel_size { 0 };
    std::uint32_t atlas_w { 0 };
    std::uint32_t atlas_h { 0 };
};

// Pixel-coord AABB inside the atlas — emitted by Populate() for each glyph
// it rasterised this call. The renderer coalesces these into per-frame
// vkCmdCopyBufferToImage regions.
struct AtlasDirtyRect {
    std::uint32_t x { 0 };
    std::uint32_t y { 0 };
    std::uint32_t w { 0 };
    std::uint32_t h { 0 };
};

class FontFace {
public:
    FontFace();
    ~FontFace();
    FontFace(FontFace&&) noexcept;
    FontFace& operator=(FontFace&&) noexcept;
    FontFace(const FontFace&)            = delete;
    FontFace& operator=(const FontFace&) = delete;

    // Rasterise every codepoint that isn't already in the atlas. Synchronous
    // (FreeType is fast). Each newly-blitted glyph appends an AtlasDirtyRect
    // for the next frame's GPU upload.
    void Populate(std::span<const std::uint32_t> codepoints);

    // Pure read of the cached metrics; nullptr if the codepoint hasn't been
    // Populate()'d yet. No FreeType / atlas mutation.
    const GlyphInfo* Lookup(std::uint32_t codepoint) const noexcept;

    FontMetrics                   Metrics() const;
    std::span<const std::uint8_t> AtlasPixels() const;

    std::span<const AtlasDirtyRect> DirtyRects() const noexcept;
    void                            ClearDirtyRects() noexcept;

    // Stable URL identifying this face's atlas in the renderer's texture
    // cache. Set by FontCache::GetFace at first registration.
    const std::string& AtlasUrl() const noexcept;

private:
    friend class FontCache;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class FontCache {
public:
    FontCache();
    ~FontCache();
    FontCache(const FontCache&)            = delete;
    FontCache& operator=(const FontCache&) = delete;

    // Acquires (or reuses) a face for the given font blob at the given pixel
    // size. The shared_ptr keeps the blob alive for the face's lifetime so
    // FreeType's pointers into it stay valid. Returns nullptr if FreeType
    // cannot open the blob.
    FontFace* GetFace(std::shared_ptr<std::vector<std::byte>> blob, std::uint32_t pixel_size);

    // Iterate every face the cache currently owns (used by the renderer's
    // per-frame atlas-commit hook).
    std::vector<FontFace*> Faces() const;

    struct ResolvedBlob {
        std::shared_ptr<std::vector<std::byte>> bytes;
        std::string                             source; // path or "in-pkg:..."
    };

    // Resolves a font reference. Tries:
    //   1. exact path on the host filesystem
    //   2. recursive search of the macOS system font directories
    //      (/System/Library/Fonts, ~/Library/Fonts, …; capped at ~2k entries)
    //   3. first available .ttf/.otf in those directories as last-resort
    //      fallback (when fallback_to_any == true)
    // Returns {nullptr, ""} if nothing matches.
    static ResolvedBlob ResolveSystemFont(std::string_view name, bool fallback_to_any = true);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Lazy accessor for the scene-owned FontCache. The cache lives behind the
// opaque `Scene::font_cache` pointer so sr.scene doesn't have to depend
// on this module.
FontCache& EnsureSceneFontCache(sr::Scene& scene);
FontCache* SceneFontCache(sr::Scene& scene) noexcept;

// Snapshot the face's atlas pixels into a renderer-consumable Image (R8,
// single slot, single mipmap, LINEAR/CLAMP_TO_EDGE sampler). The returned
// Image owns its pixel buffer; the FontFace can subsequently mutate or be
// destroyed without affecting the snapshot.
std::shared_ptr<sr::Image> BuildAtlasImage(const FontFace& face, const std::string& key);

// Lazily compiles the embedded text HLSL shader (one-time, process-wide
// cached) and returns a ready-to-bind SceneShader. The shader expects:
//   - vertex inputs: a_Position (float3), a_TexCoord (float2),
//                    a_Color (float4)
//   - uniform block ww_Uniforms with member g_ModelViewProjectionMatrix
//   - combined image sampler g_Texture0 (R8 atlas; .r = coverage)
// Returns nullptr if the SPIR-V compile fails.
std::shared_ptr<sr::SceneShader> GetTextSceneShader();

// --- TextLayouter -----------------------------------------------------------
// Lays out a UTF-8 string of glyphs into a SceneMesh's vertex / index arrays.
// SetText() only reads from the face's atlas via Lookup(); the caller is
// expected to have Populated() the codepoints first (the runtime actuator
// does this on every script tick).
//
// Mesh capacity is fixed at construction (`peak_quads`). SetText() that
// would exceed it gets clamped + logged.

struct TextLayoutStyle {
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    float                alpha { 1.0f };
    float                brightness { 1.0f };

    bool                 opaquebackground { false };
    std::array<float, 3> background_color { 0.0f, 0.0f, 0.0f };
    float                background_brightness { 1.0f };

    std::string halign; // "left" / "right" / contains-substring; default = center
    float       padding { 0.0f };
};

class TextLayouter {
public:
    // `face` must outlive the layouter (held non-owning; the scene-owned
    // FontCache keeps it alive). `mesh` must already have its
    // SceneVertexArray/SceneIndexArray sized to peak_quads * 4 vertices and
    // peak_quads * 6 indices.
    TextLayouter(FontFace* face, std::shared_ptr<sr::SceneMesh> mesh, TextLayoutStyle style,
                 std::size_t peak_quads);
    ~TextLayouter();
    TextLayouter(const TextLayouter&)            = delete;
    TextLayouter& operator=(const TextLayouter&) = delete;

    // Rewrites the vertex/index arrays in place, marks the mesh dirty.
    // Safe to call any number of times after construction.
    void SetText(std::string_view utf8);
    void SetHorizontalAlign(std::string_view align);

    // For ParseTextObj's initial-bbox log; reflects the most recent layout.
    float TextWidth() const noexcept;
    float TextHeight() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sr::text

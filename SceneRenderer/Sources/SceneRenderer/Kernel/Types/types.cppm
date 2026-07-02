module;

export module sr.types;
import sr.core;
import rstd.cppstd;

export namespace sr
{

// ---------- enums + TextureSample (was Type.hpp) ---------------------------

enum class ImageType
{
    UNKNOWN = -1,
    BMP     = 0,
    ICO     = 1,
    JPEG    = 2,
    JNG     = 3,
    KOALA   = 4,
    LBM     = 5,
    MNG     = 6,
    PBM     = 7,
    PBMRAW  = 8,
    PCD     = 9,
    PCX     = 10,
    PGM     = 11,
    PGMRAW  = 12,
    PNG     = 13,
    PPM     = 14,
    PPMRAW  = 15,
    RAS     = 16,
    TARGA   = 17,
    TIFF    = 18,
    WBMP    = 19,
    PSD     = 20,
    CUT     = 21,
    XBM     = 22,
    XPM     = 23,
    DDS     = 24,
    GIF     = 25,
    HDR     = 26,
    FAXG3   = 27,
    SGI     = 28,
    EXR     = 29,
    J2K     = 30,
    JP2     = 31,
    PFM     = 32,
    PICT    = 33,
    RAW     = 34,
    // Wallpaper Engine "scene format" wallpapers may inline an MP4/WebM
    // container as a .tex body. We extend ImageType past FreeImage's
    // range (which stops at RAW=34) so the value can flow through the
    // existing ImageHeader::type slot without colliding.
    VIDEO = 100,
};
std::string ToString(const ImageType&);

enum class TextureFormat
{
    BC1,
    BC2,
    BC3,
    RGB8,
    RGBA8,
    RG8,
    R8,
    D32F
};
std::string ToString(const TextureFormat&);

enum class BlendMode
{
    Disable,
    Translucent,
    Additive,
    Normal
};

enum class CullMode
{
    None,
    Front,
    Back
};

enum class ShaderType
{
    VERTEX,
    GEOMETRY,
    FRAGMENT
};

enum class TextureType
{
    IMG_2D,
};

enum class MeshPrimitive
{
    POINT,
    TRIANGLE
};

enum class FillMode
{
    STRETCH,
    ASPECTFIT,
    ASPECTCROP
};

enum class TextureWrap
{
    CLAMP_TO_EDGE,
    REPEAT
};

enum class TextureFilter
{
    LINEAR,
    NEAREST
};

struct TextureSample {
    TextureWrap   wrapS { TextureWrap::REPEAT };
    TextureWrap   wrapT { TextureWrap::REPEAT };
    TextureFilter magFilter { TextureFilter::NEAREST };
    TextureFilter minFilter { TextureFilter::NEAREST };
};

enum class VertexType
{
    FLOAT1,
    FLOAT2,
    FLOAT3,
    FLOAT4,
    UINT1,
    UINT2,
    UINT3,
    UINT4
};

// ---------- BitFlags<EnumT> (was in Utils.cppm) ---------------------------

template<typename EnumT>
class BitFlags {
    static_assert(std::is_enum_v<EnumT>, "Flags can only be specialized for enum types");

    using UnderlyingT = typename std::make_unsigned_t<typename std::underlying_type_t<EnumT>>;

public:
    constexpr BitFlags() noexcept: bits_(0u) {}
    constexpr BitFlags(UnderlyingT val) noexcept: bits_(val) {}

    BitFlags& set(EnumT e, bool value = true) noexcept {
        bits_.set(underlying(e), value);
        return *this;
    }
    BitFlags& reset(EnumT e) noexcept {
        set(e, false);
        return *this;
    }
    BitFlags& reset() noexcept {
        bits_.reset();
        return *this;
    }
    [[nodiscard]] bool                  all() const noexcept { return bits_.all(); }
    [[nodiscard]] bool                  any() const noexcept { return bits_.any(); }
    [[nodiscard]] bool                  none() const noexcept { return bits_.none(); }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return bits_.size(); }
    [[nodiscard]] std::size_t           count() const noexcept { return bits_.count(); }
    constexpr bool                      operator[](EnumT e) const { return bits_[underlying(e)]; }
    constexpr bool                      operator[](UnderlyingT t) const { return bits_[t]; }
    auto                                to_string() const { return bits_.to_string(); }

private:
    static constexpr UnderlyingT         underlying(EnumT e) { return static_cast<UnderlyingT>(e); }
    std::bitset<sizeof(UnderlyingT) * 8> bits_;
};

// ---------- SpriteAnimation (was SpriteAnimation.hpp) ---------------------

struct SpriteFrame {
    i32   imageId { 0 };
    float frametime { 0 };
    float x { 0 };
    float y { 0 };
    float width { 1 };
    float height { 1 };
    float rate { 1 }; // real h / w

    std::array<float, 2> xAxis { 1, 0 };
    std::array<float, 2> yAxis { 0, 1 };
};

class SpriteAnimation {
public:
    const auto& GetAnimateFrame(double newtime) {
        if ((m_remainTime -= newtime) < 0.0f) {
            SwitchToNext();
            const auto& frame = m_frames.at((usize)m_curFrame);
            m_remainTime      = frame.frametime;
        }
        const auto& frame = m_frames.at((usize)m_curFrame);
        return frame;
    }
    const auto& GetCurFrame() const { return m_frames.at((usize)m_curFrame); }
    void        AppendFrame(const SpriteFrame& frame) { m_frames.push_back(frame); }
    // Read a specific frame without advancing the internal cursor. Used by
    // the script-driven setFrame() override path.
    const SpriteFrame& GetFrame(idx i) const { return m_frames.at((usize)i); }

    usize numFrames() const { return m_frames.size(); }

private:
    void SwitchToNext() {
        if (m_curFrame >= std::ssize(m_frames) - 1)
            m_curFrame = 0;
        else
            m_curFrame++;
    }
    idx    m_curFrame { 0 };
    double m_remainTime { 0 };

    std::vector<SpriteFrame> m_frames;
};

// ---------- Image (was Image.hpp) -----------------------------------------

union ImageExtra {
    int32_t val { 0 };
    char    str[125];
};

using ImageDataPtr = std::unique_ptr<uint8_t, std::function<void(uint8_t*)>>;

struct ImageData {
    i32          width { 0 };
    i32          height { 0 };
    isize        size { 0 };
    ImageDataPtr data {};
    /* Video-tex back-channel: when ImageHeader::type == VIDEO, the
     * parser stashes the underlying pkg stream's lifetime here (opaque
     * shared_ptr<void> — concrete type is sr::fs::IBinaryStream).
     * Consumers in the Vulkan layer static_pointer_cast it back. `data`
     * stays empty in that case; the renderer side wraps {stream, offset,
     * size} into an AVIOContext for libavformat. */
    std::shared_ptr<void> videoStream;
    isize                 videoOffset { 0 };
    isize                 videoSize { 0 };
    ImageData() = default;
};

struct ImageHeader {
    i32 width { 0 };
    i32 height { 0 };
    i32 mapWidth { 0 };
    i32 mapHeight { 0 };

    bool mipmap_larger { false };
    bool mipmap_pow2 { false };

    ImageType     type { ImageType::UNKNOWN };
    TextureFormat format { TextureFormat::RGBA8 };
    i32           count { 0 };

    bool          isSprite { false };
    TextureSample sample;

    SpriteAnimation                             spriteAnim;
    std::unordered_map<std::string, ImageExtra> extraHeader;
};

struct Image : NoCopy, NoMove {
    struct Slot {
        i32 width { 0 };
        i32 height { 0 };

        std::vector<ImageData> mipmaps;

        operator bool() { return width * height * std::ssize(mipmaps) > 0; }
    };
    ImageHeader       header;
    std::vector<Slot> slots;
    std::string       key;
};

} // namespace sr

// Small OS utility: dlopen/dlsym wrapper. Lives here so Vulkan support
// can reach it without dragging SceneRendererBase in. hash_combine is co-located
// for the same reason (TextureCache key hashing).
export namespace utils
{

template<typename T>
inline void hash_combine(std::size_t& seed, const T& val) {
    seed ^= std::hash<T>()(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}
template<typename T>
inline void hash_combine_fast(std::size_t& seed, const T& val) {
    seed ^= std::hash<T>()(val) << 1u;
}

class DynamicLibrary : NoCopy {
public:
    DynamicLibrary();
    ~DynamicLibrary();

    DynamicLibrary(const char* filename);

    DynamicLibrary(DynamicLibrary&& o) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& o) noexcept;

    bool IsOpen() const;
    bool Open(const char* filename);
    void Close();

    void* GetSymbolAddr(const char* name) const;

    template<typename T>
    bool GetSymbol(const char* name, T& pfunc) const {
        pfunc = reinterpret_cast<T>(GetSymbolAddr(name));
        return pfunc != nullptr;
    }

private:
    void* handle { nullptr };
};

} // namespace utils

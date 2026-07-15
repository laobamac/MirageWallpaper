module;

#include <rstd/macro.hpp>
#include <lz4.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

module sr.pkg.parse;
import sr.core;
import sr.types;
import rstd.log;
import rstd.cppstd;
import sr.utils;
import sr.scene;
import sr.pkg_asset_version;

using namespace sr;

enum class WPTexFlagEnum : uint32_t
{
    // true for no bilinear
    noInterpolation = 0,
    // true for no repeat
    clampUVs = 1,
    sprite   = 2,

    compo1 = 20,
    compo2 = 21,
    compo3 = 22
};
using WPTexFlags = BitFlags<WPTexFlagEnum>;

namespace
{
char* Lz4Decompress(const char* src, int size, int decompressed_size) {
    char* dst       = new char[(usize)decompressed_size];
    int   load_size = LZ4_decompress_safe(src, dst, size, decompressed_size);
    if (load_size < decompressed_size) {
        rstd_error("lz4 decompress failed");
        delete[] dst;
        return nullptr;
    }
    return dst;
}

// Magic-bytes sniffer used as a fallback when the .tex header's
// `image_type` slot says UNKNOWN but the body is actually an embedded
// image container. Some PKGV0022+ assets ship this way (the texture's
// declared image_type is -1 even though the LZ4-decompressed payload
// is a self-contained PNG/JPEG). Without this fallback the body bytes
// are memcpy'd into a "raw RGBA8" slot, which decodes to garbage and
// the wallpaper renders as a flat clear-color screen.
ImageType DetectEmbeddedImageType(const unsigned char* data, usize size) {
    if (size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return ImageType::PNG;
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return ImageType::JPEG;
    if (size >= 6 && (std::memcmp(data, "GIF87a", 6) == 0 || std::memcmp(data, "GIF89a", 6) == 0))
        return ImageType::GIF;
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return ImageType::BMP;
    if (size >= 4 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2a && data[3] == 0x00) ||
                      (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2a)))
        return ImageType::TIFF;
    // ISO BMFF / MP4 / MOV / 3GP — "....ftyp...." at offset 4. WE's scene
    // wallpapers can inline an H.264/AAC mp4 here; the renderer side
    // hands the bytes to wavsen::video::VideoDecoder.
    if (size >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0) return ImageType::VIDEO;
    // Matroska / WebM EBML header.
    if (size >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3)
        return ImageType::VIDEO;
    return ImageType::UNKNOWN;
}

TextureFormat ToTexFormate(int type) {
    /*
        type
        RGBA8888 = 0,
        DXT5 = 4,
        DXT3 = 6,
        DXT1 = 7,
        RG88 = 8,
        R8 = 9,
    */
    switch (type) {
    case 0: return TextureFormat::RGBA8;
    case 4: return TextureFormat::BC3;
    case 6: return TextureFormat::BC2;
    case 7: return TextureFormat::BC1;
    case 8: return TextureFormat::RG8;
    case 9: return TextureFormat::R8;
    default:
        rstd_error("ERROR::ToTexFormate Unkown image type: {}", type);
        return TextureFormat::RGBA8;
    }
}
// Reads the fixed-layout portion of a .tex header (everything up to and
// including the optional image_type slot). Populates `header.extraHeader`
// with the version stamps + flag bits the renderer consumes downstream,
// and returns the parsed sub-versions so the body / sprite branches can
// dispatch off explicit predicates instead of re-fetching the magic ints.
//
// Version validation is permissive: unsupported (texv,texi,texb) tuples
// log an error but the function still returns a populated struct so the
// caller can decide whether to bail or attempt a best-effort read.
WPTexFormatVersion LoadHeader(fs::IBinaryStream& file, ImageHeader& header) {
    WPTexFormatVersion v;
    v.texv                         = ReadTexVersion(file);
    v.texi                         = ReadTexVersion(file);
    header.extraHeader["texv"].val = v.texv;
    header.extraHeader["texi"].val = v.texi;

    header.format = ToTexFormate(file.ReadInt32());
    WPTexFlags flags(file.ReadUint32());
    {
        header.isSprite     = flags[WPTexFlagEnum::sprite];
        header.sample.wrapS = header.sample.wrapT =
            flags[WPTexFlagEnum::clampUVs] ? TextureWrap::CLAMP_TO_EDGE : TextureWrap::REPEAT;
        header.sample.minFilter = header.sample.magFilter =
            flags[WPTexFlagEnum::noInterpolation] ? TextureFilter::NEAREST : TextureFilter::LINEAR;
        header.extraHeader["compo1"].val = flags[WPTexFlagEnum::compo1];
        header.extraHeader["compo2"].val = flags[WPTexFlagEnum::compo2];
        header.extraHeader["compo3"].val = flags[WPTexFlagEnum::compo3];
    }

    /*
        picture:
        width, height --> pow of 2 (tex size)
        mapw, maph    --> pic size
        mips
        mipw,miph     --> pow of 2

        sprites:
        width, height --> piece of sprite sheet
        mapw, maph    --> same
        1 mip
        mipw,mimp     --> tex size
    */

    header.width  = file.ReadInt32();
    header.height = file.ReadInt32();
    // in sprite this mean one pic
    header.mapWidth  = file.ReadInt32();
    header.mapHeight = file.ReadInt32();

    file.ReadInt32(); // unknown

    v.texb                         = ReadTexVersion(file);
    header.extraHeader["texb"].val = v.texb;

    header.count = file.ReadInt32();

    if (v.body_has_image_type()) header.type = static_cast<ImageType>(file.ReadInt32());
    if (v.body_has_reserved_slot()) file.ReadInt32(); // reserved (always 0 in corpus)

    if (v.texv != 5 || v.texi != 1 || v.texb < 1 || v.texb > 4) {
        rstd_error("TextureAssetDecoder: unsupported version texv={} texi={} texb={}",
                   v.texv,
                   v.texi,
                   v.texb);
    }
    return v;
}

void SetHeaderPow2(ImageHeader& header, i32 mip_0_w, i32 mip_0_h) {
    header.mipmap_pow2   = algorism::IsPowOfTwo((u32)mip_0_w) || algorism::IsPowOfTwo((u32)mip_0_h);
    header.mipmap_larger = mip_0_w * mip_0_h > header.mapWidth * header.mapHeight;
}

// --- external image path resolution ----------------------------------------
// Media-art URLs can arrive as `file://...` (or percent-encoded absolute
// paths) rather than vfs keys. When `Parse`/`ParseHeader` see such a name,
// we bypass the `.tex` container path entirely and load the file straight
// through stb_image, synthesising an ImageHeader so the downstream texture
// cache treats it like any other decoded Image.

std::optional<uint8_t> HexValue(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return std::nullopt;
}

std::optional<std::string> PercentDecode(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (usize i = 0; i < raw.size();) {
        if (raw[i] != '%') {
            out.push_back(raw[i++]);
            continue;
        }
        if (i + 2 >= raw.size()) return std::nullopt;
        auto hi = HexValue(raw[i + 1]);
        auto lo = HexValue(raw[i + 2]);
        if (! hi || ! lo) return std::nullopt;
        out.push_back(static_cast<char>((*hi << 4) | *lo));
        i += 3;
    }
    return out;
}

std::optional<std::string> ResolveExternalImagePath(std::string_view name) {
    std::string path;
    if (name.starts_with("file://localhost/")) {
        path = "/" + std::string(name.substr(std::string_view("file://localhost/").size()));
    } else if (name.starts_with("file:///")) {
        path = std::string(name.substr(std::string_view("file://").size()));
    } else if (! name.empty() && name[0] == '/') {
        path = std::string(name);
    } else {
        return std::nullopt;
    }
    auto            decoded = PercentDecode(path).value_or(path);
    std::error_code ec;
    if (! std::filesystem::is_regular_file(decoded, ec)) return std::nullopt;
    return decoded;
}

ImageHeader MakeExternalImageHeader(int width, int height) {
    ImageHeader header;
    header.width     = width;
    header.height    = height;
    header.mapWidth  = width;
    header.mapHeight = height;
    header.format    = TextureFormat::RGBA8;
    header.type      = ImageType::PNG;
    header.count     = 1;
    header.sample    = TextureSample { TextureWrap::CLAMP_TO_EDGE,
                                       TextureWrap::CLAMP_TO_EDGE,
                                       TextureFilter::LINEAR,
                                       TextureFilter::LINEAR };
    SetHeaderPow2(header, width, height);
    return header;
}

std::shared_ptr<Image> ParseExternalImage(std::string_view key, const std::string& path) {
    int   width = 0, height = 0, channels = 0;
    auto* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (! pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return nullptr;
    }

    auto img_ptr    = std::make_shared<Image>();
    img_ptr->key    = std::string(key);
    img_ptr->header = MakeExternalImageHeader(width, height);
    img_ptr->slots.resize(1);
    auto& slot  = img_ptr->slots[0];
    slot.width  = width;
    slot.height = height;
    slot.mipmaps.resize(1);
    auto& mipmap  = slot.mipmaps[0];
    mipmap.width  = width;
    mipmap.height = height;
    mipmap.size   = width * height * 4;
    mipmap.data   = ImageDataPtr(reinterpret_cast<uint8_t*>(pixels), [](uint8_t* data) {
        stbi_image_free(data);
    });
    return img_ptr;
}

} // namespace

std::shared_ptr<Image> TextureAssetDecoder::Parse(const std::string& name) {
    // Media-art URLs (`file://...` / absolute paths) bypass the .tex
    // container and decode straight from the on-disk image file.
    if (auto path = ResolveExternalImagePath(name)) {
        return ParseExternalImage(name, *path);
    }

    std::string            path    = "/assets/materials/" + name + ".tex";
    std::shared_ptr<Image> img_ptr = std::make_shared<Image>();
    auto&                  img     = *img_ptr;
    img.key                        = name;
    // std::ifstream file = fs::GetFileFstream(vfs, path);
    auto pfile = m_vfs->Open(path);
    if (! pfile) return nullptr;
    auto& file     = *pfile;
    auto  startpos = file.Tell();
    auto  ver      = LoadHeader(file, img.header);

    // image
    i32 _image_count = img.header.count;
    if (_image_count < 0) return nullptr;
    usize image_count = (usize)_image_count;

    img.slots.resize(image_count);
    for (usize i_image = 0; i_image < image_count; i_image++) {
        auto& img_slot = img.slots[i_image];
        auto& mipmaps  = img_slot.mipmaps;

        usize mipmap_count = (usize)std::max<i32>(file.ReadInt32(), 0);
        mipmaps.resize(mipmap_count);
        // load image
        for (usize i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
            auto& mipmap  = mipmaps.at(i_mipmap);
            mipmap.width  = file.ReadInt32();
            mipmap.height = file.ReadInt32();
            if (i_mipmap == 0) {
                img_slot.width  = mipmap.width;
                img_slot.height = mipmap.height;
                SetHeaderPow2(img.header, mipmap.width, mipmap.height);
            }

            bool    LZ4_compressed    = false;
            int32_t decompressed_size = 0;
            // check compress
            if (ver.body_has_lz4_prelude()) {
                LZ4_compressed    = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }

            i32 src_size = file.ReadInt32();
            if (src_size <= 0 || mipmap.width <= 0 || mipmap.height <= 0 || decompressed_size < 0)
                return nullptr;

            // Peek the first 16 bytes of the body so we can route MP4 /
            // WebM containers into the video-tex path without ever
            // pulling the (possibly hundreds of MiB) payload into RAM.
            // The pkg's IBinaryStream is seekable, so we sniff, rewind,
            // and either skip the body (video case) or read it normally.
            if (ver.body_has_image_type() && img.header.type == ImageType::UNKNOWN &&
                ! LZ4_compressed && src_size >= 16) {
                idx           body_off = file.Tell();
                unsigned char sniff[16] {};
                file.Read(sniff, sizeof(sniff));
                ImageType maybe_video = DetectEmbeddedImageType(sniff, sizeof(sniff));
                if (maybe_video == ImageType::VIDEO) {
                    img.header.type   = ImageType::VIDEO;
                    img.header.format = TextureFormat::RGBA8;
                    /* Converting ctor: shared_ptr<IBinaryStream> →
                     * shared_ptr<void>. Keeps the pkg stream alive for
                     * the renderer's lifetime, without dragging
                     * sr.fs into sr.types. */
                    mipmap.videoStream = std::shared_ptr<void>(pfile);
                    mipmap.videoOffset = body_off;
                    mipmap.videoSize   = src_size;
                    mipmap.size        = 0; /* signals "no CPU pixels" to TextureCache */
                    file.SeekSet(body_off + src_size);
                    continue;
                }
                file.SeekSet(body_off);
            }

            char* result;
            result = new char[(usize)src_size];
            file.Read(result, (usize)src_size);

            // is LZ4 compress
            if (LZ4_compressed) {
                char* decompressed_char = Lz4Decompress(result, src_size, decompressed_size);
                src_size                = decompressed_size;
                if (decompressed_char != nullptr) {
                    delete[] result;
                    result = decompressed_char;
                } else {
                    rstd_error("lz4 decompress failed");
                    delete[] result;
                    return nullptr;
                }
            }
            // is image container — declared image_type takes precedence; if
            // it's UNKNOWN, sniff the magic bytes so PKGV0022+ assets that
            // ship containerised PNG/JPEG with image_type=-1 still decode.
            ImageType embedded = img.header.type;
            if (ver.body_has_image_type() && embedded == ImageType::UNKNOWN) {
                embedded = DetectEmbeddedImageType((const unsigned char*)result, (usize)src_size);
            }
            if (ver.body_has_image_type() && embedded != ImageType::UNKNOWN) {
                int32_t w, h, n;
                auto*   data =
                    stbi_load_from_memory((const unsigned char*)result, src_size, &w, &h, &n, 4);
                if (data == nullptr) {
                    rstd_error("stbi failed to decode embedded image (type={})", (int)embedded);
                    delete[] result;
                    return nullptr;
                }
                img.header.type   = embedded;
                img.header.format = TextureFormat::RGBA8;
                mipmap.data       = ImageDataPtr((uint8_t*)data, [](uint8_t* data) {
                    stbi_image_free((unsigned char*)data);
                });
                src_size          = w * h * 4;
            } else {
                mipmap.data = ImageDataPtr(new uint8_t[(usize)src_size], [](uint8_t* data) {
                    delete[] data;
                });
                std::copy(result, result + src_size, mipmap.data.get());
            }
            mipmap.size = src_size * (i32)sizeof(uint8_t);
            delete[] result;
        }
    }
    return img_ptr;
}

ImageHeader TextureAssetDecoder::ParseHeader(const std::string& name) {
    // External media art can change in place, so it deliberately bypasses the
    // cache. Packaged .tex metadata is immutable for the Scene lifetime and is
    // requested repeatedly by scene validation and graph planning.
    if (ResolveExternalImagePath(name)) return ParseHeaderUncached(name);
    {
        std::lock_guard lock(m_header_cache_mutex);
        if (auto it = m_header_cache.find(name); it != m_header_cache.end()) return it->second;
    }
    auto header = ParseHeaderUncached(name);
    {
        std::lock_guard lock(m_header_cache_mutex);
        m_header_cache.insert_or_assign(name, header);
    }
    return header;
}

ImageHeader TextureAssetDecoder::ParseHeaderUncached(const std::string& name) {
    ImageHeader header;
    // External media-art image: probe via stbi_info so the validator
    // sees real dimensions without a full Parse().
    if (auto path = ResolveExternalImagePath(name)) {
        int width = 0, height = 0, channels = 0;
        if (stbi_info(path->c_str(), &width, &height, &channels) && width > 0 && height > 0)
            return MakeExternalImageHeader(width, height);
        return header;
    }
    // WE "_alias_*" textures are runtime aliases the engine resolves
    // internally (light cookies, etc.). We don't model that, so just
    // return an empty header without spamming a vfs miss.
    if (name.find("_alias_") != std::string::npos) return header;
    std::string path  = "/assets/materials/" + name + ".tex";
    auto        pfile = m_vfs->Open(path);
    if (! pfile) return header;
    auto& file = *pfile;

    auto ver = LoadHeader(file, header);
    if (header.count < 0) return header;

    usize image_count = (usize)header.count;

    // load sprite info
    if (header.isSprite) {
        // bypass image data, store width and height
        std::vector<std::vector<float>> imageDatas(image_count);
        for (usize i_image = 0; i_image < image_count; i_image++) {
            int mipmap_count = file.ReadInt32();
            for (int32_t i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
                int32_t width  = file.ReadInt32();
                int32_t height = file.ReadInt32();
                if (i_mipmap == 0) {
                    imageDatas.at(i_image) = { (float)width, (float)height };
                    header.mipmap_pow2     = algorism::IsPowOfTwo((u32)(width * height));
                }
                if (ver.body_has_lz4_prelude()) {
                    int32_t LZ4_compressed    = file.ReadInt32();
                    int32_t decompressed_size = file.ReadInt32();
                    (void)LZ4_compressed;
                    (void)decompressed_size;
                }
                long src_size = file.ReadInt32();
                file.SeekCur(src_size);
            }
        }
        // sprite pos
        ver.texs                       = ReadTexVersion(file);
        header.extraHeader["texs"].val = ver.texs;
        // texs out of [1,3] means the body walk above ended at the wrong
        // offset (corrupt file or a layout drift this parser doesn't
        // know). Reading framecount + frame records past that point gives
        // us garbage, and worse, dereferences imageDatas with whatever
        // the next 4 bytes happen to be — historically that asserted on
        // an empty inner vector and aborted the whole process. Bail out
        // with the structural fields we already populated; sprite-frame
        // info is best-effort anyway in our renderer pipeline.
        if (ver.texs < 1 || ver.texs > 3) {
            rstd_error("TextureAssetDecoder: unsupported texs version {} for {}", ver.texs, name);
            return header;
        }
        int32_t framecount = file.ReadInt32();
        if (ver.sprite_has_atlas_size()) {
            i32 width  = file.ReadInt32();
            i32 height = file.ReadInt32();
            (void)width;
            (void)height;
        }

        for (int32_t i = 0; i < framecount; i++) {
            SpriteFrame sf;
            sf.imageId = file.ReadInt32();
            // Two ways an imageId can be poison: outright negative (old
            // sentinel) or pointing past image_count, or pointing to an
            // image whose mip section was empty. All three previously
            // tripped vector::operator[]'s assertion. Skip the frame's
            // remaining bytes so subsequent frames stay aligned.
            const auto bad_id = sf.imageId < 0 ||
                                static_cast<usize>(sf.imageId) >= imageDatas.size() ||
                                imageDatas[static_cast<usize>(sf.imageId)].size() < 2;
            if (bad_id) {
                rstd_error(
                    "TextureAssetDecoder: invalid sprite frame imageId={} (image_count={}) in {}",
                    sf.imageId,
                    imageDatas.size(),
                    name);
                file.ReadFloat();             // frametime
                for (int j = 0; j < 6; ++j) { // x, y, xAxis[0..1], yAxis[0..1]
                    if (ver.sprite_frame_coords_int())
                        file.ReadInt32();
                    else
                        file.ReadFloat();
                }
                continue;
            }
            float spriteWidth  = imageDatas[static_cast<usize>(sf.imageId)][0];
            float spriteHeight = imageDatas[static_cast<usize>(sf.imageId)][1];

            sf.frametime = file.ReadFloat();
            if (ver.sprite_frame_coords_int()) {
                sf.x        = (float)file.ReadInt32() / spriteWidth;
                sf.y        = (float)file.ReadInt32() / spriteHeight;
                sf.xAxis[0] = (float)file.ReadInt32();
                sf.xAxis[1] = (float)file.ReadInt32();
                sf.yAxis[0] = (float)file.ReadInt32();
                sf.yAxis[1] = (float)file.ReadInt32();
            } else {
                sf.x        = file.ReadFloat() / spriteWidth;
                sf.y        = file.ReadFloat() / spriteHeight;
                sf.xAxis[0] = file.ReadFloat();
                sf.xAxis[1] = file.ReadFloat();
                sf.yAxis[0] = file.ReadFloat();
                sf.yAxis[1] = file.ReadFloat();
            }
            sf.width  = (float)std::sqrt(std::pow(sf.xAxis[0], 2) + std::pow(sf.xAxis[1], 2));
            sf.height = (float)std::sqrt(std::pow(sf.yAxis[0], 2) + std::pow(sf.yAxis[1], 2));
            sf.xAxis[0] /= spriteWidth;
            sf.xAxis[1] /= spriteWidth;
            sf.yAxis[0] /= spriteHeight;
            sf.yAxis[1] /= spriteHeight;
            sf.rate = sf.height / sf.width;
            header.spriteAnim.AppendFrame(sf);
        }
    } else {
        i32 mipmap_count = file.ReadInt32();
        (void)mipmap_count;
        i32 width  = file.ReadInt32();
        i32 height = file.ReadInt32();
        SetHeaderPow2(header, width, height);
        /* Sniff the body for a video container so the validator can
         * report "video tex" without needing a full Parse(). Mirrors
         * the peek in Parse() (line ~210). Cheap: 16 bytes + a
         * SeekSet. */
        if (ver.body_has_image_type() && header.type == ImageType::UNKNOWN) {
            bool    lz4               = false;
            int32_t decompressed_size = 0;
            if (ver.body_has_lz4_prelude()) {
                lz4               = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }
            (void)decompressed_size;
            i32 src_size = file.ReadInt32();
            if (! lz4 && src_size >= 16) {
                idx           body_off = file.Tell();
                unsigned char sniff[16] {};
                file.Read(sniff, sizeof(sniff));
                if (DetectEmbeddedImageType(sniff, sizeof(sniff)) == ImageType::VIDEO) {
                    header.type   = ImageType::VIDEO;
                    header.format = TextureFormat::RGBA8;
                }
                file.SeekSet(body_off);
            }
        }
    }
    return header;
}

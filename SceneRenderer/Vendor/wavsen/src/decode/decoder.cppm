export module wavsen.decode;

import rstd.cppstd;
import rstd;

export namespace wavsen::decode {

enum class ErrorKind : std::int32_t {
    InvalidArgs,
    OpenFailed,
    NoVideoStream,
    DecoderInit,
    SeekFailed,
    DecodeFailed,
    ScaleFailed,
};

struct Error {
    ErrorKind   kind;
    std::string message;
};

struct RgbaImage {
    std::vector<std::uint8_t> data;
    std::uint32_t             width  = 0;
    std::uint32_t             height = 0;
    std::uint32_t             stride = 0;
};

struct ThumbOptions {
    std::uint32_t max_edge        = 512;
    double        seek_seconds    = 1.0;
    double        seek_fraction   = 0.10;
    bool          prefer_keyframe = true;
};

auto extract_thumbnail(std::string_view path, const ThumbOptions& opts)
    -> rstd::Result<RgbaImage, Error>;

} // namespace wavsen::decode

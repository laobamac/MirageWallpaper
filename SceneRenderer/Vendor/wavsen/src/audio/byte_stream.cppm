export module wavsen.audio:byte_stream;

import rstd.cppstd;
import rstd;        // pulls in rstd::io::Result / SeekFrom / error::Error transitively

export namespace wavsen::audio {

// Source for the audio decoder. Mirrors the rstd::io::Read + rstd::io::Seek
// trait shape but is exposed as a virtual base so the FFmpeg AVIO C
// callback (a function pointer) can dispatch back through a single
// `void*` opaque. Concrete impls in this header (PosixFile) and in OWE
// (BStreamAdapter wrapping owe::fs::IBinaryStream).
class IByteStream {
public:
    virtual ~IByteStream() = default;

    // Read up to `len` bytes into `buf`. Returns Ok(n) where n <= len;
    // n == 0 means EOF. Spurious EINTR retried by the implementation.
    virtual auto read(rstd::u8* buf, rstd::usize len)
        -> rstd::io::Result<rstd::usize> = 0;

    // Seek to a position. Returns the resulting absolute byte offset.
    virtual auto seek(rstd::io::SeekFrom pos)
        -> rstd::io::Result<rstd::u64> = 0;
};

// Concrete IByteStream backed by a POSIX file descriptor (open(2) /
// read(2) / lseek(2)). O_RDONLY | O_CLOEXEC. Closes on destruction.
class PosixFile final : public IByteStream {
public:
    static auto open(const std::string& path)
        -> rstd::io::Result<std::unique_ptr<PosixFile>>;

    ~PosixFile() override;
    PosixFile(const PosixFile&)            = delete;
    PosixFile& operator=(const PosixFile&) = delete;

    auto read(rstd::u8* buf, rstd::usize len)
        -> rstd::io::Result<rstd::usize> override;
    auto seek(rstd::io::SeekFrom pos)
        -> rstd::io::Result<rstd::u64>   override;

private:
    explicit PosixFile(int fd) : fd_(fd) {}
    int fd_ { -1 };
};

} // namespace wavsen::audio

module wavsen.audio;

import rstd.cppstd;
import rstd;
import :byte_stream;

namespace wavsen::audio {

using rstd::io::SeekFrom;
using rstd::io::error::Error;
namespace libc = rstd::sys::libc;

auto PosixFile::open(const std::string& path)
    -> rstd::io::Result<std::unique_ptr<PosixFile>>
{
    int fd = libc::open(path.c_str(), libc::O_RDONLY | libc::O_CLOEXEC);
    if (fd < 0) {
        return rstd::Err(Error::from_raw_os_error(libc::errno()));
    }
    return rstd::Ok(std::unique_ptr<PosixFile>(new PosixFile(fd)));
}

PosixFile::~PosixFile() {
    if (fd_ >= 0) libc::close(fd_);
}

auto PosixFile::read(rstd::u8* buf, rstd::usize len)
    -> rstd::io::Result<rstd::usize>
{
    while (true) {
        auto n = libc::read(fd_, buf, len);
        if (n >= 0) return rstd::Ok(static_cast<rstd::usize>(n));
        if (libc::errno() == libc::EINTR) continue;
        return rstd::Err(Error::from_raw_os_error(libc::errno()));
    }
}

auto PosixFile::seek(SeekFrom pos)
    -> rstd::io::Result<rstd::u64>
{
    int whence;
    switch (pos.which) {
    case SeekFrom::Which::Start:   whence = libc::SEEK_SET; break;
    case SeekFrom::Which::Current: whence = libc::SEEK_CUR; break;
    case SeekFrom::Which::End:     whence = libc::SEEK_END; break;
    }
    auto off = libc::lseek(fd_, static_cast<libc::off_t>(pos.offset), whence);
    if (off < 0) {
        return rstd::Err(Error::from_raw_os_error(libc::errno()));
    }
    return rstd::Ok(static_cast<rstd::u64>(off));
}

} // namespace wavsen::audio

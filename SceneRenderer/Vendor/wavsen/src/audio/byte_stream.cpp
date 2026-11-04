module;
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <algorithm>
#include <climits>
#endif
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
#if defined(_WIN32)
    const auto native = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    int fd = ::_wopen(native.c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) return rstd::Err(Error::from_raw_os_error(static_cast<int>(libc::GetLastError())));
#else
    int fd = libc::open(path.c_str(), libc::O_RDONLY | libc::O_CLOEXEC);
    if (fd < 0) {
        return rstd::Err(Error::from_raw_os_error(libc::errno()));
    }
#endif
    return rstd::Ok(std::unique_ptr<PosixFile>(new PosixFile(fd)));
}

PosixFile::~PosixFile() {
#if defined(_WIN32)
    if (fd_ >= 0) ::_close(fd_);
#else
    if (fd_ >= 0) libc::close(fd_);
#endif
}

auto PosixFile::read(rstd::u8* buf, rstd::usize len)
    -> rstd::io::Result<rstd::usize>
{
    while (true) {
#if defined(_WIN32)
        const auto count = static_cast<unsigned int>(std::min<rstd::usize>(len, INT_MAX));
        const auto n = ::_read(fd_, buf, count);
        if (n >= 0) return rstd::Ok(static_cast<rstd::usize>(n));
        if (errno == EINTR) continue;
        return rstd::Err(Error::from_raw_os_error(static_cast<int>(libc::GetLastError())));
#else
        auto n = libc::read(fd_, buf, len);
        if (n >= 0) return rstd::Ok(static_cast<rstd::usize>(n));
        if (libc::errno() == libc::EINTR) continue;
        return rstd::Err(Error::from_raw_os_error(libc::errno()));
#endif
    }
}

auto PosixFile::seek(SeekFrom pos)
    -> rstd::io::Result<rstd::u64>
{
    int whence;
#if defined(_WIN32)
    switch (pos.which) {
    case SeekFrom::Which::Start: whence = SEEK_SET; break;
    case SeekFrom::Which::Current: whence = SEEK_CUR; break;
    case SeekFrom::Which::End: whence = SEEK_END; break;
    }
    const auto off = ::_lseeki64(fd_, static_cast<__int64>(pos.offset), whence);
    if (off < 0) return rstd::Err(Error::from_raw_os_error(static_cast<int>(libc::GetLastError())));
#else
    switch (pos.which) {
    case SeekFrom::Which::Start:   whence = libc::SEEK_SET; break;
    case SeekFrom::Which::Current: whence = libc::SEEK_CUR; break;
    case SeekFrom::Which::End:     whence = libc::SEEK_END; break;
    }
    auto off = libc::lseek(fd_, static_cast<libc::off_t>(pos.offset), whence);
    if (off < 0) {
        return rstd::Err(Error::from_raw_os_error(libc::errno()));
    }
#endif
    return rstd::Ok(static_cast<rstd::u64>(off));
}

} // namespace wavsen::audio

module;
#include <rstd/macro.hpp>
export module rstd:io.cursor;
export import :io.traits;
import rstd.alloc;
import :forward;

using rstd_alloc::vec::Vec;

namespace rstd::io
{

// ── Cursor ────────────────────────────────────────────────────────────────
/// In-memory reader/writer with a position cursor.
/// Supported inner types: `Vec<u8>` (R+W+Seek+BufRead), `slice<u8>` (R+Seek+BufRead).
export template<typename T>
class Cursor {
public:
    T   inner_;
    u64 pos_ = 0;

    USE_TRAIT_WIN(Cursor)

    explicit Cursor(T inner) noexcept: inner_(rstd::move(inner)), pos_(0) {}

    /// Returns a reference to the underlying value in this cursor.
    auto get_ref() const noexcept -> const T& { return inner_; }
    /// Returns a mutable reference to the underlying value in this cursor.
    auto get_mut() noexcept -> T& { return inner_; }
    /// Returns the current position of this cursor.
    auto position() const noexcept -> u64 { return pos_; }
    /// Sets the position of this cursor.
    /// \param p The new byte offset from the start.
    void set_position(u64 p) noexcept { pos_ = p; }
    /// Consumes this cursor, returning the underlying value.
    auto into_inner() -> T { return rstd::move(inner_); }
};

namespace detail
{
inline auto cursor_data(const Vec<u8>& v) noexcept -> const u8* {
    return v.begin();
}
inline auto cursor_len(const Vec<u8>& v) noexcept -> usize {
    return v.len();
}
inline auto cursor_data(slice<u8> s) noexcept -> const u8* {
    return &*s;
}
inline auto cursor_len(slice<u8> s) noexcept -> usize {
    return s.len();
}
} // namespace detail

} // namespace rstd::io

// ── Impl specialisations ──────────────────────────────────────────────────
namespace rstd
{

// Read — Vec<u8> and slice<u8>
template<typename T>
    requires(mtp::same_as<T, Vec<u8>> || mtp::same_as<T, slice<u8>>)
struct Impl<io::Read, io::Cursor<T>> : ImplBase<io::Cursor<T>> {
    auto read(u8* buf, usize len) -> io::Result<usize> {
        auto& self  = this->self();
        usize total = io::detail::cursor_len(self.inner_);
        usize pos   = usize(rstd::min(self.pos_, u64(total)));
        usize n     = rstd::min(len, total - pos);
        rstd::mem::memcpy(buf, io::detail::cursor_data(self.inner_) + pos, n);
        self.pos_ += n;
        return Ok(n);
    }
};

// BufRead — Vec<u8> and slice<u8>
template<typename T>
    requires(mtp::same_as<T, Vec<u8>> || mtp::same_as<T, slice<u8>>)
struct Impl<io::BufRead, io::Cursor<T>> : ImplBase<io::Cursor<T>> {
    auto fill_buf() -> io::Result<slice<u8>> {
        auto& self  = this->self();
        usize total = io::detail::cursor_len(self.inner_);
        usize pos   = usize(rstd::min(self.pos_, u64(total)));
        return Ok(
            slice<u8>::from_raw_parts(io::detail::cursor_data(self.inner_) + pos, total - pos));
    }
    auto consume(usize amt) -> void {
        auto& self  = this->self();
        usize total = io::detail::cursor_len(self.inner_);
        self.pos_   = rstd::min(self.pos_ + u64(amt), u64(total));
    }
};

// Seek — Vec<u8> and slice<u8>
template<typename T>
    requires(mtp::same_as<T, Vec<u8>> || mtp::same_as<T, slice<u8>>)
struct Impl<io::Seek, io::Cursor<T>> : ImplBase<io::Cursor<T>> {
    auto seek(io::SeekFrom sf) -> io::Result<u64> {
        auto& self  = this->self();
        i64   total = i64(io::detail::cursor_len(self.inner_));
        i64   new_pos;
        switch (sf.which) {
        case io::SeekFrom::Which::Start: new_pos = i64(u64(sf.offset)); break;
        case io::SeekFrom::Which::End: new_pos = total + sf.offset; break;
        case io::SeekFrom::Which::Current: new_pos = i64(self.pos_) + sf.offset; break;
        default: new_pos = 0;
        }
        if (new_pos < 0)
            return Err(io::error::Error::from_kind(
                io::error::ErrorKind { io::error::ErrorKind::InvalidInput }));
        self.pos_ = u64(new_pos);
        return Ok(self.pos_);
    }
};

// Write — Vec<u8> only (grows the buffer)
template<>
struct Impl<io::Write, io::Cursor<Vec<u8>>> : ImplBase<io::Cursor<Vec<u8>>> {
    auto write(const u8* buf, usize len) -> io::Result<usize> {
        auto& self  = this->self();
        usize total = self.inner_.len();
        usize pos   = usize(rstd::min(self.pos_, u64(total)));
        usize end   = pos + len;
        for (usize i = total; i < end; ++i) self.inner_.push(u8(0));
        rstd::mem::memcpy(self.inner_.begin() + pos, buf, len);
        self.pos_ += len;
        return Ok(len);
    }
    auto flush() -> io::Result<empty> { return Ok(empty {}); }
};

} // namespace rstd

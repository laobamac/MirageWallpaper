module;
#include <rstd/macro.hpp>
export module rstd:ffi.os_str;
export import :io;
export import rstd.alloc;

using ::alloc::vec::Vec;
using ::alloc::string::String;
using namespace rstd::prelude;

namespace rstd::ffi
{

/// An unsized, platform-native string type.
///
/// On Unix this is an arbitrary byte sequence (often UTF-8).
/// On Windows this would be WTF-8 (not yet implemented).
export struct OsStr {
    ~OsStr() = delete;
};

} // namespace rstd::ffi

namespace rstd
{

template<>
struct Impl<Sized, ffi::OsStr> {
    ~Impl() = delete;
};

template<>
struct Impl<ptr_::Pointee, ffi::OsStr> {
    using Metadata = usize;
};

/// A borrowed reference to a platform-native string.
template<>
struct ref<ffi::OsStr> : ref_base<ref<ffi::OsStr>, u8[], false> {
    USE_TRAIT_WIN(ref)

    using Target = ffi::OsStr;

    u8 const* p { nullptr };
    usize     length { 0 };

    constexpr ref() noexcept = default;
    constexpr ref(u8 const* p, usize len) noexcept: p(p), length(len) {}

    /// Construct from a `ref<str>` (UTF-8 is always valid OS bytes).
    constexpr ref(ref<str> s) noexcept: p(s.data()), length(s.size()) {}

    /// Construct from a null-terminated C string.
    constexpr ref(const char* c_str) noexcept
        : p(rstd::bit_cast<u8 const*>(c_str)), length(rstd::strlen(c_str)) {}

    static constexpr auto from_raw_parts(u8 const* p, usize len) noexcept -> Self {
        return { p, len };
    }

    /// Returns the encoded bytes of this OS string.
    constexpr auto as_encoded_bytes() const noexcept -> slice<u8> {
        return slice<u8>::from_raw_parts(p, length);
    }

    /// Attempts to convert to a UTF-8 string slice.
    ///
    /// Returns `None` if the bytes are not valid UTF-8.
    constexpr auto to_str() const noexcept -> Option<ref<str>> {
        if (char_::is_valid_utf8(p, length)) {
            ref<str> r(p, length);
            return Some(rstd::move(r));
        }
        return None();
    }

    /// Converts to a `String`, replacing invalid UTF-8 with U+FFFD.
    auto to_string_lossy() const -> String {
        auto  buf = String::make();
        usize i   = 0;
        while (i < length) {
            auto [cp, n] = char_::decode_utf8(p + i, length - i);
            if (cp == char_::REPLACEMENT && n == 1 && p[i] > 0x7F) {
                // Invalid byte — emit replacement character
                buf.push(char_::REPLACEMENT);
            } else {
                buf.push(cp);
            }
            i += n;
        }
        return buf;
    }

    constexpr auto len() const noexcept -> usize { return length; }
    constexpr auto is_empty() const noexcept -> bool { return length == 0; }
    constexpr auto data() const noexcept -> u8 const* { return p; }

    constexpr operator bool() const { return length > 0 && p != nullptr; }

    constexpr auto deref() const noexcept -> ref<Target> { return *this; }
};

} // namespace rstd

export namespace rstd::ffi
{

/// An owned, platform-native string.
///
/// On Unix this wraps `Vec<u8>`. Analogous to Rust's `OsString`.
class OsString {
    Vec<u8> inner;

    explicit OsString(Vec<u8>&& v): inner(rstd::move(v)) {}

public:
    OsString()                               = default;
    OsString(OsString&&) noexcept            = default;
    OsString& operator=(OsString&&) noexcept = default;

    /// Creates an empty `OsString`.
    static auto make() -> OsString { return {}; }

    /// Creates an `OsString` from a `String` (zero-cost move on Unix).
    static auto from(String&& s) -> OsString {
        auto bytes = rstd::as<Into<Vec<u8>>>(s).into();
        return OsString { rstd::move(bytes) };
    }

    /// Creates an `OsString` by copying a `ref<str>`.
    static auto from(ref<str> s) -> OsString { return from(String::make(s)); }

    /// Creates an `OsString` by copying a `ref<OsStr>`.
    static auto from(ref<OsStr> s) -> OsString {
        auto v = Vec<u8>::with_capacity(s.len());
        for (usize i = 0; i < s.len(); i++) {
            u8 b = s.data()[i];
            v.push(rstd::move(b));
        }
        return OsString { rstd::move(v) };
    }

    /// Creates an `OsString` from raw bytes without validation.
    static auto from_encoded_bytes_unchecked(Vec<u8>&& bytes) -> OsString {
        return OsString { rstd::move(bytes) };
    }

    /// Returns a borrowed `ref<OsStr>`.
    auto as_os_str() const noexcept -> ref<OsStr> {
        return ref<OsStr>::from_raw_parts(inner.begin(), inner.len());
    }

    /// Attempts to convert to a `String`.
    ///
    /// Returns `Err(self)` if the bytes are not valid UTF-8.
    auto into_string() -> result::Result<String, OsString> {
        if (char_::is_valid_utf8(inner.begin(), inner.len())) {
            return Ok(String::from_utf8_unchecked(rstd::move(inner)));
        }
        return Err(OsString { rstd::move(inner) });
    }

    /// Appends an `OsStr` to this string.
    void push(ref<OsStr> s) {
        for (usize i = 0; i < s.len(); i++) {
            u8 b = s.data()[i];
            inner.push(rstd::move(b));
        }
    }

    auto len() const noexcept -> usize { return inner.len(); }
    auto is_empty() const noexcept -> bool { return inner.len() == 0; }
    auto capacity() const noexcept -> usize { return inner.capacity(); }
    void clear() { inner.clear(); }

    /// Implicit conversion to `ref<OsStr>`.
    operator ref<OsStr>() const noexcept { return as_os_str(); }
};

} // namespace rstd::ffi

// ── Display for ref<OsStr> ───────────────────────────────────────────────
namespace rstd
{

template<>
struct Impl<fmt::Display, ref<ffi::OsStr>> : ImplBase<ref<ffi::OsStr>> {
    auto fmt(fmt::Formatter& f) const -> bool {
        // Print as UTF-8 lossy — valid bytes pass through, invalid → replacement
        auto& s = this->self();
        usize i = 0;
        while (i < s.len()) {
            auto [cp, n] = char_::decode_utf8(s.data() + i, s.len() - i);
            u8   buf[4];
            auto wrote = char_::encode_utf8(cp, buf);
            if (! f.write_raw(buf, wrote)) return false;
            i += n;
        }
        return true;
    }
};

template<>
struct Impl<fmt::Debug, ref<ffi::OsStr>> : ImplBase<ref<ffi::OsStr>> {
    auto fmt(fmt::Formatter& f) const -> bool {
        f.write_raw((const u8*)"\"", 1);
        as<fmt::Display>(this->self()).fmt(f);
        return f.write_raw((const u8*)"\"", 1);
    }
};

} // namespace rstd

module;
#include <rstd/macro.hpp>
export module rstd.alloc:ffi.c_str;
export import :boxed;
export import :vec;

using ::alloc::boxed::Box;
using ::alloc::vec::Vec;

using rstd::alloc::Allocator;
using rstd::ffi::CStr;
namespace fmt = rstd::fmt;
using namespace rstd::prelude;

namespace alloc::ffi
{
export struct NulError {
    usize   size;
    Vec<u8> data;

    NulError(usize s, Vec<u8>&& d) noexcept: size(s), data(rstd::move(d)) {}
    NulError(NulError&&) noexcept;
    NulError& operator=(NulError&&) noexcept;
    ~NulError();
};

/// An owned, C-compatible, nul-terminated string, analogous to Rust's `CString`.
export class CString {
public:
    ::alloc::boxed::Box<u8[]> inner;

    USE_TRAIT_WIN(CString)

    CString(::alloc::boxed::Box<u8[]>&& b) noexcept: inner(rstd::move(b)) {}
    CString(CString&&) noexcept;
    CString& operator=(CString&&) noexcept;
    ~CString();

    /// Creates a `CString` from a byte vector without checking for interior nul bytes.
    /// \param v The byte vector; a nul terminator will be appended.
    /// \return A `CString` wrapping the data.
    static auto from_vec_unchecked(Vec<u8>&& v) -> Self {
        return _from_vec_unchecked(rstd::move(v));
    }
    static auto _from_vec_unchecked(Vec<u8>&& v) -> Self {
        v.push(0);
        auto boxed = v.into_boxed_slice();
        return Self { rstd::move(boxed) };
    }
    /// Creates a new `CString` from a type convertible to `Vec<u8>`, checking for interior nul bytes.
    /// \tparam T A type that implements `Into<Vec<u8>>`.
    /// \param t The input data.
    /// \return `Ok(CString)` on success, or `Err(NulError)` if an interior nul byte is found.
    template<Impled<Into<Vec<u8>>> T>
    static auto make(T t) -> Result<CString, NulError> {
        Vec<u8> vec = rstd::into(rstd::move(t));
        if (auto mem = rstd::memchr::memchr(0, vec.as_slice()); mem.is_some()) {
            return Err(NulError { vec.len(), rstd::move(vec) });
        } else {
            return Ok(CString::_from_vec_unchecked(rstd::move(vec)));
        }
    }

    /// Creates a `CString` by copying from a nul-terminated C string pointer.
    /// \param p A pointer to a nul-terminated C string.
    /// \return A `CString` owning a copy of the data.
    static auto from_raw_parts(char const* p) -> CString {
        auto len    = rstd::strlen(p) + 1;
        auto layout = Layout::array<u8>(len).unwrap();
        auto res    = as<Allocator>(GLOBAL).allocate(layout);
        if (res.is_err()) handle_alloc_error(layout);

        auto* raw = static_cast<u8*>(res.unwrap_unchecked().as_mut_ptr().as_raw_ptr());
        rstd::mem::memcpy(raw, p, len);
        auto boxed = ::alloc::boxed::Box<u8[]>::from_raw(mut_ptr<u8[]>::from_raw_parts(raw, len));
        return CString { rstd::move(boxed) };
    }

    /// Returns a borrowed reference to the inner `CStr`.
    /// \return A `ref<CStr>` view of the C string data.
    auto as_ref() const -> ref<CStr> {
        auto ptr = inner.as_ptr();
        auto p   = reinterpret_cast<CStr const*>(&*ptr);
        return { .p = p, .length = ptr.len() - 1 };
    }

    /// Consumes the `CString` and returns the underlying byte vector without the nul terminator.
    /// \return A `Vec<u8>` containing the string bytes.
    auto into_bytes() -> Vec<u8> {
        Vec<u8> vec = rstd::into(rstd::move(inner));
        (void)vec.pop(); // remove trailing null byte
        return vec;
    }

    /// Returns the contents as a byte slice, not including the nul terminator.
    /// \return A `slice<u8>` of the string bytes.
    auto to_bytes() const -> slice<u8> {
        auto cstr = as_ref();
        return slice<u8>::from_raw_parts(as_cast<const u8*>(cstr.p), cstr.length);
    }

    /// Returns the contents as a byte slice, including the nul terminator.
    /// \return A `slice<u8>` of the string bytes with the trailing nul.
    auto to_bytes_with_nul() const -> slice<u8> {
        auto cstr = as_ref();
        return slice<u8>::from_raw_parts(as_cast<const u8*>(cstr.p), cstr.length + 1);
    }
};

// Out-of-line non-inline defaults so a strong symbol lands in this module's
// object file. Works around gcc 15 modules failing to emit implicit special
// members for module-owned classes.
NulError::NulError(NulError&&) noexcept            = default;
NulError& NulError::operator=(NulError&&) noexcept = default;
NulError::~NulError()                              = default;

CString::CString(CString&&) noexcept            = default;
CString& CString::operator=(CString&&) noexcept = default;
CString::~CString()                             = default;

} // namespace alloc::ffi

using ::alloc::ffi::CString;
using ::alloc::ffi::NulError;

namespace rstd
{

template<>
struct Impl<Clone, CString> : DefaultInImpl<Clone, CString> {
    auto clone() const -> CString { return CString { this->self().inner.clone() }; }
};

template<mtp::same_as<AsRef<ffi::CStr>> T, mtp::same_as<CString> A>
struct Impl<T, A> {};

} // namespace rstd

module;
#include <rstd/macro.hpp>
export module rstd.core:ffi.c_str;
export import :marker;
export import :core;

namespace rstd::ffi
{
// enum FromBytesWithNulError {
//     /// Data provided contains an interior nul byte at byte `position`.
//     InteriorNul {
//         /// The position of the interior nul byte.
//         position: usize,
//     },
//     /// Data provided is not nul terminated.
//     NotNulTerminated,
// }

/// A borrowed reference to a nul-terminated C string.
export class CStr;

} // namespace rstd::ffi

using rstd::ffi::CStr;
namespace rstd
{

template<>
struct Impl<Sized, CStr> {
    ~Impl() = delete;
};

template<>
struct Impl<ptr_::Pointee, CStr> {
    using Metadata = usize;
};

template<>
struct ref<CStr> : ref_base<ref<CStr>, CStr, false> {
    USE_TRAIT_WIN(ref)

    using Target = CStr;

    CStr const* p { nullptr };
    usize       length { 1 };

    auto count_bytes() const noexcept { return length; }
    auto is_empty() const noexcept { return length == 0; }
    auto to_bytes() const noexcept -> slice<u8> {
        return slice<u8>::from_raw_parts(reinterpret_cast<u8 const*>(p), length);
    }

    constexpr auto deref() const noexcept -> ref<Target> { return *this; }
};

template<>
struct mut_ref<CStr> : ref_base<mut_ref<CStr>, CStr, true> {
    USE_TRAIT_WIN(mut_ref)

    using Target = CStr;

    CStr* p { nullptr };
    usize length { 1 };

    auto count_bytes() const noexcept { return length; }
    auto is_empty() const noexcept { return length == 0; }

    constexpr auto deref() const noexcept -> ref<Target> { return this->as_ref(); }
    constexpr auto deref_mut() noexcept -> mut_ref<Target> { return *this; }
};

namespace ffi
{
class CStr {
public:
    CStr()  = delete;
    ~CStr() = delete;

    static auto from_ptr(char const* p) noexcept -> ref<CStr> {
        return ref<CStr>::from_raw_parts(reinterpret_cast<CStr const*>(p), rstd::strlen(p));
    }
};

} // namespace ffi

} // namespace rstd

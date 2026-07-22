module;
#include <compare>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <tuple>
#include <source_location>
export module rstd.basic:prelude;

/// Global placement new operator.
export using ::operator new;
/// Global delete operator.
export using ::operator delete;
namespace std
{
/// Provides the number of elements in a tuple-like type.
export using std::tuple_size;
/// Provides the type of the I-th element in a tuple-like type.
export using std::tuple_element;
/// Extracts the I-th element from a tuple-like type.
export using std::get;
/// A totally ordered comparison result type.
export using std::strong_ordering;
export using std::compare_three_way;
export using std::nullptr_t;
} // namespace std

namespace rstd
{
export {
    /// 8-bit signed integer.
    using i8 = ::int8_t;
    /// 16-bit signed integer.
    using i16 = ::int16_t;
    /// 32-bit signed integer.
    using i32 = ::int32_t;
    /// 64-bit signed integer.
    using i64 = ::int64_t;

    /// 8-bit unsigned integer.
    using u8 = ::uint8_t;
    /// 16-bit unsigned integer.
    using u16 = ::uint16_t;
    /// 32-bit unsigned integer.
    using u32 = ::uint32_t;
    /// 64-bit unsigned integer.
    using u64 = ::uint64_t;

    /// 32-bit IEEE 754 floating-point.
    using f32 = float;
    /// 64-bit IEEE 754 floating-point.
    using f64 = double;
#if defined(__SIZEOF_INT128__) || defined(__int128)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    /// 128-bit unsigned integer.
    using u128 = unsigned __int128;
    /// 128-bit signed integer.
    using i128 = __int128;
#pragma GCC diagnostic pop
#else
    // MSVC does not support __int128. Fall back to a 128-bit struct placeholder.
    // Most rstd code uses u128/i128 only through trait-based generic paths
    // that don't require physical 128-bit registers.
    struct u128 { std::uint64_t lo; std::uint64_t hi; };
    struct i128 { std::uint64_t lo; std::int64_t hi; };
#endif

    /// Signed index type, equivalent to Rust's `isize` used for offsets.
    using idx = ::ptrdiff_t;
    /// Pointer-sized unsigned integer, equivalent to Rust's `usize`.
    using usize = ::size_t;
    /// Pointer-sized signed integer, equivalent to Rust's `isize`.
    using isize = ::intptr_t;
    /// A type representing a raw byte, with no numeric interpretation.
    using std::byte;

    /// Mutable void pointer.
    using voidp = void*;
    /// Const void pointer.
    using const_voidp = void const*;
    /// Integer type guaranteed to hold a pointer value.
    using usizeptr = ::uintptr_t;

    /// A zero-sized type, analogous to Rust's `()` (unit).
    struct empty {};
    /// A zero-sized phantom type parameterized on `T`.
    template<typename>
    struct emptyT {};

    /// The type of `nullptr`.
    using std::nullptr_t;
    /// Provides the minimum and maximum finite values for arithmetic types.
    using std::numeric_limits;
    /// Captures information about the source code location.
    using std::source_location;

    /// Extracts the I-th element from a tuple-like type.
    using std::get;
    /// Provides the type of the I-th element in a tuple-like type.
    using std::tuple_element;
    /// Provides the number of elements in a tuple-like type.
    using std::tuple_size;

    /// Alignment value type for sized deallocation.
    using std::align_val_t;

    /// Function object performing three-way comparison.
    using std::compare_three_way;
    /// A partially ordered comparison result type.
    using std::partial_ordering;
    /// A totally ordered comparison result type.
    using std::strong_ordering;
    /// A weakly ordered comparison result type.
    using std::weak_ordering;

    /// Forwards lvalues as either lvalues or rvalues, preserving value category.
    using std::forward;
    /// Converts a value to an rvalue, enabling move semantics.
    using std::move;
}

// used for using namespace
export namespace prelude
{
using i8  = ::int8_t;
using i16 = ::int16_t;
using i32 = ::int32_t;
using i64 = ::int64_t;

using u8  = ::uint8_t;
using u16 = ::uint16_t;
using u32 = ::uint32_t;
using u64 = ::uint64_t;

using f32 = float;
using f64 = double;

using idx   = ::ptrdiff_t;
using usize = ::size_t;
using isize = ::intptr_t;
using std::byte;
using voidp       = void*;
using const_voidp = void const*;
using usizeptr    = ::uintptr_t;
using rstd::empty;
using rstd::emptyT;

using std::nullptr_t;
using std::numeric_limits;
using std::source_location;

using std::tuple_element;
using std::tuple_size;

using std::align_val_t;

using std::compare_three_way;
using std::partial_ordering;
using std::strong_ordering;
using std::weak_ordering;

} // namespace prelude

} // namespace rstd
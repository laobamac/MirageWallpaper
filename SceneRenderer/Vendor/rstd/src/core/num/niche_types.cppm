module;
#define DEFINE_VALID_RANGE_TYPE(name, T, UT, low, high)                                      \
    namespace rstd::num::niche_types                                                         \
    {                                                                                        \
    export struct name : NonZeroBase<T> {                                                    \
        using Self = name;                                                                   \
        static auto           make(T in) noexcept -> Option<Self>;                           \
        static constexpr auto make_unchecked(T in) noexcept -> Self { return { in }; }       \
        friend constexpr bool operator==(name a, name b) noexcept { return a.val == b.val; } \
        friend constexpr strong_ordering operator<=>(name a, name b) noexcept {              \
            return a.val <=> b.val;                                                          \
        }                                                                                    \
    };                                                                                       \
    auto name::make(T in) noexcept -> Option<Self> {                                         \
        constexpr const UT _low { low };                                                     \
        constexpr const UT _high { high };                                                   \
        const UT           _in { static_cast<UT>(in) };                                      \
        if (_in >= _low && _in <= _high) {                                                   \
            return Some(Self { in });                                                        \
        } else {                                                                             \
            return None();                                                                   \
        }                                                                                    \
    }                                                                                        \
    }

export module rstd.core:num.niche_types;
export import :option;

namespace rstd::num::niche_types
{
template<typename T>
    requires requires { ::new ((void*)0) T(T {}); }
struct Test {
    static constexpr int x = 1;
};
static_assert(Test<i32>::x);

/// Base type for niche-optimized non-zero integer inner representations.
/// \tparam T The underlying integer type.
export template<typename T>
struct NonZeroBase {
    T val;

    auto as_inner() const noexcept -> T { return val; }
};
} // namespace rstd::num::niche_types

DEFINE_VALID_RANGE_TYPE(NonZeroU8Inner, u8, u8, 1, 0xff)
DEFINE_VALID_RANGE_TYPE(NonZeroU16Inner, u16, u16, 1, 0xffff)
DEFINE_VALID_RANGE_TYPE(NonZeroU32Inner, u32, u32, 1, 0xffffffff)
DEFINE_VALID_RANGE_TYPE(NonZeroU64Inner, u64, u64, 1, 0xffffffffffffffff)
DEFINE_VALID_RANGE_TYPE(NonZeroI8Inner, i8, u8, 1, 0xff)
DEFINE_VALID_RANGE_TYPE(NonZeroI16Inner, i16, u16, 1, 0xffff)
DEFINE_VALID_RANGE_TYPE(NonZeroI32Inner, i32, u32, 1, 0xffffffff)
DEFINE_VALID_RANGE_TYPE(NonZeroI64Inner, i64, u64, 1, 0xffffffffffffffff)
DEFINE_VALID_RANGE_TYPE(NonZeroCharInner, char, u32, 1, 0x10ffff)

#if __SIZEOF_POINTER__ == 8 || defined(_M_AMD64) || defined(__x86_64__)
DEFINE_VALID_RANGE_TYPE(UsizeNoHighBit, usize, usize, 0, 0x7fffffffffffffff)
DEFINE_VALID_RANGE_TYPE(NonZeroUsizeInner, usize, usize, 1, 0xffffffffffffffff)
DEFINE_VALID_RANGE_TYPE(NonZeroIsizeInner, isize, usize, 1, 0xffffffffffffffff)
#elif __SIZEOF_POINTER__ == 4 || defined(_M_IX86)
DEFINE_VALID_RANGE_TYPE(UsizeNoHighBit, usize, usize, 0, 0x7fffffff)
DEFINE_VALID_RANGE_TYPE(NonZeroUsizeInner, usize, usize, 1, 0xffffffff)
DEFINE_VALID_RANGE_TYPE(NonZeroIsizeInner, isize, usize, 1, 0xffffffff)
#else
#error "Unsupported pointer width"
#endif

DEFINE_VALID_RANGE_TYPE(U32NotAllOnes, u32, u32, 0, 0xfffffffe)
DEFINE_VALID_RANGE_TYPE(I32NotAllOnes, i32, u32, 0, 0xfffffffe)
DEFINE_VALID_RANGE_TYPE(U64NotAllOnes, u64, u64, 0, 0xfffffffffffffffe)
DEFINE_VALID_RANGE_TYPE(I64NotAllOnes, i64, u64, 0, 0xfffffffffffffffe)

namespace rstd::num::niche_types
{

static_assert(NonZeroU32Inner::make_unchecked(1) == NonZeroU32Inner::make_unchecked(1));
} // namespace rstd::num::niche_types
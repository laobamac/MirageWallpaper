module;
#include <rstd/macro.hpp>
export module rstd.core:str.str;
export import :core;
export import :fmt;
export import :marker;
export import :char_;

namespace rstd::str_
{
/// An unsized UTF-8 string type, analogous to Rust's `str`.
export struct Str {
    ~Str() = delete;
};

/// Concept for types that can be viewed as a string slice (must expose `data()` and `size()`).
template<typename T>
concept ViewableStr = requires(T t) {
    { t.data() } -> mtp::convertible_to<char const*>;
    { t.size() } -> mtp::same_as<usize>;
} || requires(T t) {
    { t.data() } -> mtp::convertible_to<u8 const*>;
    { t.size() } -> mtp::same_as<usize>;
};

} // namespace rstd::str_

namespace rstd
{

template<>
struct Impl<Sized, str_::Str> {
    ~Impl() = delete;
};

template<>
struct Impl<ptr_::Pointee, str_::Str> {
    using Metadata = usize;
};

template<>
struct ref<str_::Str> : ref_base<ref<str_::Str>, u8[], false> {
public:
    USE_TRAIT_WIN(ref)

    using Target = str_::Str;

    u8 const* p { nullptr };
    usize     length { 0 };

    constexpr ref() noexcept = default;

    template<typename T>
        requires str_::ViewableStr<T>
    constexpr ref(const T& t) noexcept(noexcept(rstd::declval<T>().data()))
        : p((u8 const*)t.data()), length(t.size()) {};

    constexpr ref(u8 const* p, usize length) noexcept: p(p), length(length) {}
    constexpr ref(slice<u8> p) noexcept: ref(p.p, p.length) {}

    constexpr ref(char const* c_str) noexcept
        : ref(rstd::bit_cast<u8 const*>(c_str), rstd::strlen(c_str)) {}

    static constexpr auto from_raw_parts(value_type* p, usize length) noexcept -> Self {
        return { p, length };
    }

    constexpr auto size() const { return length; }
    constexpr auto data() const { return p; }

    constexpr auto begin() const { return p; }
    constexpr auto end() const { return p + length; }

    constexpr operator bool() const { return length > 0 && p != nullptr; }

    constexpr auto deref() const noexcept -> ref<Target> { return *this; }
};

/// Type alias for the unsized string type.
export using str = str_::Str;

/// Compares two string slices for equality by value.
export [[nodiscard]]
constexpr bool operator==(ref<str> a, ref<str> b) noexcept {
    return a.size() == b.size() &&
           __builtin_strncmp((char const*)a.data(), (char* const)b.data(), a.size()) == 0;
}

template<>
struct ptr<str_::Str> : ptr_base<ptr<str_::Str>, u8[], false> {
public:
    u8 const* p { nullptr };
    usize     length { 0 };

    using value_type         = u8;
    using Self               = ptr;
    constexpr ptr() noexcept = default;

    template<typename T>
        requires str_::ViewableStr<T>
    constexpr ptr(const T& t) noexcept(noexcept(rstd::declval<T>().data()))
        : p((u8 const*)t.data()), length(t.size()) {};

    constexpr ptr(u8 const* p, usize length) noexcept: p(p), length(length) {}

    static constexpr auto from_raw_parts(value_type* p, usize length) noexcept -> Self {
        return { p, length };
    }

    constexpr auto size() const { return length; }
    constexpr auto data() const { return p; }
    constexpr auto begin() const { return p; }
    constexpr auto end() const { return p + length; }

    constexpr operator bool() const { return length > 0 && p != nullptr; }
};

} // namespace rstd

// ── Chars: UTF-8 code point iterator ─────────────────────────────────────
namespace rstd::str_
{

/// A hand-rolled iterator over Unicode code points in a UTF-8 string slice.
///
/// Supports `next()` for manual iteration and `begin()`/`end()` for range-for.
export struct Chars {
    u8 const* _ptr;
    u8 const* _end;

    /// Returns `true` if there are no remaining code points.
    constexpr auto is_empty() const noexcept -> bool { return _ptr >= _end; }

    /// Decodes and returns the next code point, advancing the position.
    /// Returns `char_::REPLACEMENT` with no advance if already at end.
    /// Use `is_empty()` to check before calling.
    constexpr auto next_unchecked() noexcept -> char32_t {
        auto [cp, n] = char_::decode_utf8(_ptr, static_cast<usize>(_end - _ptr));
        _ptr += n;
        return cp;
    }

    // ── range-for support ────────────────────────────────────────────
    struct Sentinel {};

    struct Iterator {
        Chars*   chars;
        char32_t current { 0 };
        bool     done { false };

        constexpr Iterator(Chars* c): chars(c) { advance(); }

        constexpr void advance() {
            if (chars->_ptr >= chars->_end) {
                done = true;
                return;
            }
            current = chars->next_unchecked();
        }

        constexpr auto operator*() const -> char32_t { return current; }
        constexpr auto operator++() -> Iterator& {
            advance();
            return *this;
        }
        constexpr auto operator!=(Sentinel) const -> bool { return ! done; }
    };

    constexpr auto begin() -> Iterator { return Iterator { this }; }
    constexpr auto end() -> Sentinel { return {}; }
};

} // namespace rstd::str_

// ── ref<str> additional methods (no Option dependency) ───────────────────
namespace rstd
{

/// Creates a string slice from a byte slice without UTF-8 validation.
export constexpr auto from_utf8_unchecked(slice<u8> bytes) noexcept -> ref<str> {
    return ref<str>::from_raw_parts(const_cast<u8*>(&*bytes), bytes.len());
}

} // namespace rstd

namespace rstd::str_
{

/// Returns `true` if the string is empty (zero bytes).
export constexpr auto is_empty(ref<str> s) noexcept -> bool {
    return s.size() == 0;
}

/// Returns `true` if all bytes are ASCII.
export constexpr auto is_ascii(ref<str> s) noexcept -> bool {
    for (usize i = 0; i < s.size(); i++) {
        if (s.data()[i] > 0x7F) return false;
    }
    return true;
}

/// Returns `true` if `pos` is on a UTF-8 character boundary.
export constexpr auto is_char_boundary(ref<str> s, usize pos) noexcept -> bool {
    return char_::is_char_boundary(s.data(), s.size(), pos);
}

/// Returns the byte slice of the string.
export constexpr auto as_bytes(ref<str> s) noexcept -> slice<u8> {
    return slice<u8>::from_raw_parts(s.data(), s.size());
}

/// Returns a `Chars` iterator over the string's Unicode code points.
export constexpr auto chars(ref<str> s) noexcept -> Chars {
    return { s.data(), s.data() + s.size() };
}

/// Returns `true` if `needle` is a substring of `haystack`.
export constexpr auto contains(ref<str> haystack, ref<str> needle) noexcept -> bool {
    if (needle.size() == 0) return true;
    if (needle.size() > haystack.size()) return false;
    for (usize i = 0; i <= haystack.size() - needle.size(); i++) {
        if (__builtin_memcmp(haystack.data() + i, needle.data(), needle.size()) == 0) return true;
    }
    return false;
}

/// Returns `true` if the string starts with `prefix`.
export constexpr auto starts_with(ref<str> s, ref<str> prefix) noexcept -> bool {
    if (prefix.size() > s.size()) return false;
    return __builtin_memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}

/// Returns `true` if the string ends with `suffix`.
export constexpr auto ends_with(ref<str> s, ref<str> suffix) noexcept -> bool {
    if (suffix.size() > s.size()) return false;
    return __builtin_memcmp(s.data() + s.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}

/// Splits the string at the given byte position.
export constexpr auto split_at(ref<str> s, usize mid) noexcept -> rstd::tuple<ref<str>, ref<str>> {
    return { ref<str>::from_raw_parts(const_cast<u8*>(s.data()), mid),
             ref<str>::from_raw_parts(const_cast<u8*>(s.data() + mid), s.size() - mid) };
}

/// Returns the string with leading and trailing ASCII whitespace removed.
export constexpr auto trim(ref<str> s) noexcept -> ref<str> {
    auto* b = s.data();
    auto* e = b + s.size();
    while (b < e && (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r')) ++b;
    while (e > b && (*(e - 1) == ' ' || *(e - 1) == '\t' || *(e - 1) == '\n' || *(e - 1) == '\r'))
        --e;
    return ref<str>::from_raw_parts(const_cast<u8*>(b), static_cast<usize>(e - b));
}

/// Extracts the last `count` path components from a path string.
/// \param path The path string to extract from.
/// \param count The number of trailing path components to extract.
/// \return A string slice containing the last `count` components.
export constexpr auto extract_last(ref<str> path, usize count) -> ref<str> {
    auto pos = path.size();
    while (pos != 0) {
        if (path[pos - 1] == '/' || path[pos - 1] == '\\') {
            --count;
        }
        if (count != 0) {
            --pos;
        } else {
            break;
        }
    }
    auto begin = path.begin() + pos;
    auto size  = path.end() - begin;
    return ref<str>::from_raw_parts(begin, size);
}
} // namespace rstd::str_

namespace rstd
{

template<>
struct Impl<fmt::Display, ref<str>> : ImplBase<ref<str>> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_raw(this->self().data(), this->self().size());
    }
};

template<>
struct Impl<fmt::Debug, ref<str>> : ImplBase<ref<str>> {
    auto fmt(fmt::Formatter& f) const -> bool {
        f.write_raw((const u8*)"\"", 1);
        as<fmt::Display>(this->self()).fmt(f);
        return f.write_raw((const u8*)"\"", 1);
    }
};

} // namespace rstd

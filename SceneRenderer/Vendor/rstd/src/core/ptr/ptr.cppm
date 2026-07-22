module;
#include <rstd/macro.hpp>
export module rstd.core:ptr.ptr;
export import :ptr.metadata;
export import :ops.deref;
export import :cmp;

namespace rstd
{

/// An immutable raw pointer, analogous to Rust's `*const T`.
/// \tparam T The pointee type.
export template<typename T>
struct ptr;

/// A mutable raw pointer, analogous to Rust's `*mut T`.
/// \tparam T The pointee type.
export template<typename T>
struct mut_ptr;

template<typename T, typename U>
auto from_raw_parts(U* self) noexcept -> T {
    if constexpr (requires { T::from_raw_parts(self->as_raw_ptr(), self->metadata()); }) {
        return T::from_raw_parts(self->as_raw_ptr(), self->metadata());
    } else {
        return T::from_raw_parts(self->as_raw_ptr());
    }
}

template<typename T, typename U, typename P>
auto from_raw_parts_override(U* self, P ptr) noexcept -> T {
    if constexpr (requires { self->metadata(); })
        return T::from_raw_parts(ptr, self->metadata());
    else
        return T::from_raw_parts(ptr);
}

/// CRTP base for reference-like pointer types (`ref`, `mut_ref`).
/// \tparam Self The derived type (CRTP).
/// \tparam T The pointee type.
/// \tparam Mutable Whether the reference is mutable.
export template<typename Self, typename T, bool Mutable>
struct ref_base {
    /// we only process T[] for value_type
    using value_type = mtp::cond<Mutable, mtp::rm_ext<T>, mtp::add_const<mtp::rm_ext<T>>>;

    constexpr rstd::strong_ordering operator<=>(const ref_base& o) const noexcept = default;

    constexpr bool operator==(const value_type& other) const noexcept {
        return *(static_cast<Self const*>(this)->p) == other;
    }

    constexpr auto as_ref() const noexcept -> ref<T>
        requires Mutable
    {
        return rstd::from_raw_parts<ref<T>>(static_cast<Self const*>(this));
    }

    constexpr auto as_ptr() const noexcept -> ptr<T> {
        return rstd::from_raw_parts<ptr<T>>(static_cast<Self const*>(this));
    }
    constexpr auto as_mut_ptr() const noexcept -> mut_ptr<T>
        requires Mutable
    {
        return rstd::from_raw_parts<mut_ptr<T>>(static_cast<Self const*>(this));
    }

    constexpr auto as_raw_ptr() const noexcept -> value_type* {
        return static_cast<Self const*>(this)->p;
    }

    template<typename U>
    constexpr auto cast() const noexcept -> mut_ptr<U> {
        return mut_ptr<U>::from_raw_parts(reinterpret_cast<mtp::rm_ext<U>*>(as_raw_ptr()));
    }

    template<typename U>
    constexpr auto cast_array(usize len = 0) const noexcept -> mut_ptr<U[]> {
        return mut_ptr<U[]>::from_raw_parts(reinterpret_cast<U*>(as_raw_ptr()), len);
    }

    /// \name Normal
    /// @{
    static constexpr auto from_raw_parts(value_type* p) noexcept -> Self
        requires(! mtp::DST<T>)
    {
        return { .p = p };
    }
    /// @}

    /// \name Requires: DSTArray
    /// @{
    constexpr decltype(auto) operator[](usize i) const noexcept
        requires mtp::DSTArray<T>
    {
        return *(static_cast<Self const*>(this)->p + i);
    }

    constexpr auto len() const noexcept -> usize
        requires mtp::DSTArray<T>
    {
        return static_cast<Self const*>(this)->length;
    }

    static constexpr auto from_raw_parts(value_type* p, usize length) noexcept -> Self
        requires mtp::DSTArray<T> && mtp::is_aggregate<Self>
    {
        return { .p = p, .length = length };
    }

    constexpr auto metadata() const noexcept
        requires mtp::DSTArray<T>
    {
        return len();
    }
    /// @}
};

/// CRTP base for raw pointer types (`ptr`, `mut_ptr`).
/// \tparam Self The derived type (CRTP).
/// \tparam T The pointee type.
/// \tparam Mutable Whether the pointer is mutable.
export template<typename Self, typename T, bool Mutable>
struct ptr_base {
    /// we only process T[] for value_type
    using value_type = mtp::cond<Mutable, mtp::rm_ext<T>, mtp::add_const<mtp::rm_ext<T>>>;

    constexpr value_type* operator->() const noexcept { return static_cast<Self const*>(this)->p; }
    constexpr value_type& operator*() const noexcept {
        return *(static_cast<Self const*>(this)->p);
    }

    constexpr rstd::strong_ordering operator<=>(const ptr_base& o) const noexcept = default;

    constexpr bool operator==(const value_type& other) const noexcept {
        return *(static_cast<Self const*>(this)->p) == other;
    }
    constexpr bool operator==(std::nullptr_t) const noexcept {
        return static_cast<Self const*>(this)->p == nullptr;
    }

    constexpr auto as_ptr() const noexcept -> ptr<T>
        requires Mutable
    {
        return rstd::from_raw_parts<ptr<T>>(static_cast<Self const*>(this));
    }

    constexpr auto as_ref() const noexcept -> ref<T> {
        return rstd::from_raw_parts<ref<T>>(static_cast<Self const*>(this));
    }

    constexpr auto as_mut_ref() const noexcept -> mut_ref<T>
        requires Mutable
    {
        return rstd::from_raw_parts<mut_ref<T>>(static_cast<Self const*>(this));
    }

    constexpr operator value_type*() const noexcept { return static_cast<Self const*>(this)->p; }

    constexpr void reset() noexcept {
        auto self = static_cast<Self const*>(this);
        self->p   = nullptr;
        if constexpr (mtp::DSTArray<T>) {
            self->length = 0;
        }
    }

    constexpr auto as_raw_ptr() const noexcept -> value_type* {
        return static_cast<Self const*>(this)->p;
    }

    template<typename U>
    constexpr auto cast() const noexcept -> mut_ptr<U> {
        return mut_ptr<U>::from_raw_parts(reinterpret_cast<mtp::rm_ext<U>*>(as_raw_ptr()));
    }

    template<typename U>
    constexpr auto cast_array(usize len = 0) const noexcept -> mut_ptr<U[]> {
        return mut_ptr<U[]>::from_raw_parts(reinterpret_cast<U*>(as_raw_ptr()), len);
    }

    /// \name Normal
    /// @{
    static constexpr auto from_raw_parts(value_type* p) noexcept -> Self
        requires(! mtp::DST<T>) && mtp::is_aggregate<Self>
    {
        return { .p = p };
    }
    /// @}

    /// \name Requires: DSTArray
    /// @{
    constexpr decltype(auto) operator[](usize i) const noexcept
        requires mtp::DSTArray<T>
    {
        return *(static_cast<Self const*>(this)->p + i);
    }

    constexpr auto len() const noexcept
        requires mtp::DSTArray<T>
    {
        return static_cast<Self const*>(this)->length;
    }

    static constexpr auto from_raw_parts(value_type* p, usize length) noexcept -> Self
        requires mtp::DSTArray<T> && mtp::is_aggregate<Self>
    {
        return { .p = p, .length = length };
    }

    constexpr auto metadata() const noexcept
        requires mtp::DSTArray<T>
    {
        return len();
    }
    /// @}
};

template<typename T>
struct ref : ref_base<ref<T>, T, false> {
    static_assert(! mtp::is_const<T>);

    USE_TRAIT_WIN(ref)

    using Target = T;

    T const* p { nullptr };

    constexpr auto deref() const noexcept -> ref<T> { return *this; }
};

template<mtp::DSTArray T>
struct ref<T> : ref_base<ref<T>, T, false> {
    static_assert(! mtp::is_const<T>);

    USE_TRAIT_WIN(ref)

    using Target        = T;
    using value_type    = mtp::rm_ext<T>;
    using metadata_type = usize;

    value_type const* p { nullptr };
    metadata_type     length;

    constexpr auto deref() const noexcept -> ref<T> { return *this; }
};

template<typename T>
struct mut_ref : ref_base<mut_ref<T>, T, true> {
    static_assert(! mtp::is_const<T>);

    USE_TRAIT_WIN(mut_ref)

    using Target = T;

    T* p { nullptr };

    constexpr auto deref() const noexcept -> ref<T> { return this->as_ref(); }
    constexpr auto deref_mut() noexcept -> mut_ref<T> { return *this; }
};

template<mtp::DSTArray T>
struct mut_ref<T> : ref_base<mut_ref<T>, T, true> {
    static_assert(! mtp::is_const<T>);

    USE_TRAIT_WIN(mut_ref)

    using Target     = T;
    using value_type = mtp::rm_ext<T>;

    value_type* p { nullptr };
    usize       length;

    constexpr auto deref() const noexcept -> ref<T> { return this->as_ref(); }
    constexpr auto deref_mut() noexcept -> mut_ref<T> { return *this; }
};

template<typename T>
struct ptr : ptr_base<ptr<T>, T, false> {
    static_assert(! mtp::is_const<T>);
    using Self = ptr;

    T const* p { nullptr };
};

template<mtp::DSTArray T>
struct ptr<T> : ptr_base<ptr<T>, T, false> {
    static_assert(! mtp::is_const<T>);
    using value_type = mtp::rm_ext<T>;
    using Self       = ptr;

    value_type const* p { nullptr };
    usize             length;
};

template<typename T>
struct mut_ptr : ptr_base<mut_ptr<T>, T, true> {
    static_assert(! mtp::is_const<T>);
    using Self = mut_ptr;

    T* p { nullptr };
};

template<typename T>
    requires mtp::DSTArray<T>
struct mut_ptr<T> : ptr_base<mut_ptr<T>, T, true> {
    static_assert(! mtp::is_const<T>);
    using value_type = mtp::rm_ext<T>;
    using Self       = mut_ptr;

    value_type* p { nullptr };
    usize       length;
};

/// A borrowed reference to a contiguous sequence of `T`, analogous to Rust's `&[T]`.
/// \tparam T The element type.
export template<typename T>
using slice = ref<T[]>;

} // namespace rstd

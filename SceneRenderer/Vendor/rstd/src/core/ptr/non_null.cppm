module;
#include <rstd/macro.hpp>
export module rstd.core:ptr.non_null;
export import :marker;
export import :num;
export import :cmp;
export import :option;

namespace rstd::ptr_::non_null
{
/// A non-null pointer type, analogous to Rust's `NonNull<T>`.
/// \tparam T The pointee type.
export template<typename T>
struct NonNull;
} // namespace rstd::ptr_::non_null

namespace rstd::option::detail
{

template<typename T, typename NonePayload, typename SomePayload>
struct option_storage<ptr_::non_null::NonNull<T>, NonePayload, SomePayload>
    : zero_niche_option_storage<NonePayload, SomePayload> {
    using base = zero_niche_option_storage<NonePayload, SomePayload>;
    using base::base;

    static_assert(sizeof(SomePayload) == sizeof(ptr_::non_null::NonNull<T>));
};

} // namespace rstd::option::detail

namespace rstd::ptr_::non_null
{

/// A non-null pointer type, guaranteed to never be null.
/// \tparam T The pointee type.
template<typename T>
struct NonNull {
    using element_type    = T;
    using pointer_t       = mut_ptr<T>;
    using const_pointer_t = ptr<T>;

    // static_assert(mtp::triv_copy<pointer_t>);
    pointer_t pointer;

    USE_TRAIT_WIN(NonNull)

    /// \name T: Sized
    /// @{

    /// Creates a new `NonNull` from a non-zero address without provenance.
    /// \param addr A non-zero address.
    /// \return A `NonNull` pointer with the given address.
    static auto without_provenance(num::nonzero::NonZero<usize> addr) noexcept -> NonNull<T>
        requires Impled<T, Sized>
    {
        T* t = reinterpret_cast<T*>(addr.get());
        return { mut_ptr<T>::from_raw_parts(t) };
    }

    /// Creates a dangling but well-aligned `NonNull` pointer.
    /// \return A `NonNull` pointer aligned to `T` that must not be dereferenced.
    static auto dangling() noexcept -> NonNull<T>
        requires Impled<T, Sized>
    {
        return { mut_ptr<T>::from_raw_parts(reinterpret_cast<T*>(alignof(T))) };
    }
    /// @}

    /// \name T: ?Sized
    /// @{

    /// Creates a `NonNull` if the pointer is non-null, or `None` otherwise.
    /// \param p The pointer to wrap.
    /// \return `Some(NonNull)` if `p` is non-null, `None` otherwise.
    static constexpr auto make(pointer_t p) noexcept -> Option<NonNull> {
        if (p == nullptr) return {};
        return Some(NonNull {
            .pointer = p,
        });
    }

    /// Creates a `NonNull` without checking that the pointer is non-null.
    /// \param p The pointer to wrap; must not be null.
    /// \return A `NonNull` wrapping `p`.
    static constexpr auto make_unchecked(pointer_t p) noexcept -> NonNull {
        static_assert(mtp::triv_copy<NonNull>);
        return { .pointer = p };
    }

    /// Returns the inner pointer as a const pointer.
    constexpr auto as_ptr() const noexcept -> const_pointer_t { return pointer.as_ptr(); }

    /// Returns the inner pointer as a mutable pointer.
    constexpr auto as_mut_ptr() const noexcept -> pointer_t { return pointer; }

    /// Returns `true` if the pointer is non-null.
    constexpr explicit operator bool() const noexcept { return pointer != nullptr; }

    /// Dereferences the pointer, returning an immutable reference.
    constexpr auto as_ref() const noexcept { return pointer.as_ref(); }

    /// Dereferences the pointer, returning a mutable reference.
    constexpr auto as_mut() const noexcept { return pointer.as_mut_ref(); }

    /// Returns the underlying raw pointer.
    constexpr auto as_raw_ptr() const noexcept { return pointer.as_raw_ptr(); }
    /// @}

    /// Casts this `NonNull<T>` to a `NonNull<U>`.
    /// \tparam U The target pointee type.
    template<class U>
    constexpr auto cast() const noexcept {
        return NonNull<U>::make_unchecked(pointer.template cast<U>());
    }

    /// Calculates the pointer offset by `count` elements forward.
    /// \param count Number of elements to advance.
    /// \return The offset `NonNull` pointer.
    constexpr NonNull add(usize count) const noexcept {
        return NonNull::make_unchecked(pointer + count);
    }

    /// Calculates the pointer offset by `count` elements backward.
    /// \param count Number of elements to subtract.
    /// \return The offset `NonNull` pointer.
    constexpr NonNull sub(usize count) const noexcept {
        return NonNull::make_unchecked(pointer - count);
    }

    /// Calculates the pointer offset by a signed `count` of elements.
    /// \param count Signed element offset (positive = forward, negative = backward).
    /// \return The offset `NonNull` pointer.
    constexpr NonNull offset(isize count) const noexcept {
        return NonNull::make_unchecked(pointer + count);
    }

    /// Calculates the pointer offset by `bytes` bytes forward.
    /// \param bytes Number of bytes to advance.
    /// \return The offset `NonNull` pointer.
    constexpr NonNull byte_add(usize bytes) const noexcept {
        return NonNull::make_unchecked(pointer.byte_add(bytes));
    }

    /// Calculates the pointer offset by `bytes` bytes backward.
    /// \param bytes Number of bytes to subtract.
    /// \return The offset `NonNull` pointer.
    constexpr NonNull byte_sub(usize bytes) const noexcept {
        return NonNull::make_unchecked(pointer.byte_sub(bytes));
    }

    friend constexpr bool operator<=>(NonNull a, NonNull b) noexcept {
        return a.pointer <=> b.pointer;
    }
    friend constexpr bool operator==(NonNull a, NonNull b) noexcept {
        return a.pointer == b.pointer;
    }
    constexpr bool operator==(pointer_t p) noexcept { return pointer == p; }
    constexpr bool operator==(nullptr_t in) const noexcept { return pointer == in; }
};

} // namespace rstd::ptr_::non_null

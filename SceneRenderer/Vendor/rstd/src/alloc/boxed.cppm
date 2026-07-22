module;
#include <rstd/macro.hpp>
export module rstd.alloc:boxed;
export import rstd.core;
export import :alloc;

using alloc::handle_alloc_error;

using rstd::alloc::Allocator;
using rstd::alloc::Layout;
using rstd::mem::manually_drop::ManuallyDrop;
using rstd::ptr_::non_null::NonNull;
namespace mtp = rstd::mtp;
using namespace rstd::prelude;

namespace alloc::boxed
{

/// A pointer type that uniquely owns a heap allocation of type `T`.
/// \tparam T The type of the value stored on the heap.
export template<typename T>
class Box {
    NonNull<T> m_ptr;

    constexpr explicit Box(NonNull<T> ptr) noexcept: m_ptr(ptr) {}

public:
    USE_TRAIT_WIN(Box)

    using Target = T;

    ~Box() { reset(); }
    Box(const Box&) noexcept            = delete;
    Box& operator=(const Box&) noexcept = delete;

    /// Creates a new `Box` by cloning the contained value.
    /// \return A new `Box` with a cloned copy of the value.
    auto clone() const -> Self
        requires Impled<T, Clone, Sized>
    {
        return make(as<Clone>(as_ptr()).clone());
    }
    /// Replaces the contents of this `Box` with a clone of the source.
    /// \param source The `Box` to clone from.
    void clone_from(Self& source)
        requires requires(Box b) { b.clone(); }
    {
        *this = source.clone();
    }

    constexpr Box(Box&& o) noexcept: m_ptr(o.m_ptr) { rstd::mem::fill(o.m_ptr, 0); }
    Box& operator=(Box&& o) noexcept {
        if (this != &o) {
            // clean
            reset();
            // assign
            m_ptr = o.m_ptr;
            // move
            rstd::mem::fill(o.m_ptr, 0);
        }
        return *this;
    }

    /// Allocates memory on the heap and constructs `T` in place with the given arguments.
    /// \tparam Args The constructor argument types.
    /// \param args The arguments forwarded to the constructor of `T`.
    /// \return A `Box` owning the newly allocated value.
    template<typename... Args>
    static auto make(Args&&... args) -> Box
        requires Impled<T, Sized>
    {
        auto layout = Layout::make<T>();
        auto res    = as<Allocator>(GLOBAL).allocate(layout);
        if (res.is_err()) handle_alloc_error(layout);

        auto p = res.unwrap_unchecked().as_mut_ptr().template cast<T>();
        new (p.as_raw_ptr()) T(rstd::forward<Args>(args)...);
        return from_raw(p);
    }

    /// Allocates memory on the heap for a dynamically-sized trait object.
    /// \tparam U The concrete type that implements the trait `T`.
    /// \param in The value to box as a trait object.
    /// \return A `Box` owning the trait object.
    template<typename U>
    static auto make(U&& in) -> Box
        requires(! Impled<T, Sized> && mtp::dyn_traits<T>::template Impled<U>)
    {
        auto layout = Layout::make<U>();
        auto res    = as<Allocator>(GLOBAL).allocate(layout);
        if (res.is_err()) handle_alloc_error(layout);

        auto p = res.unwrap_unchecked().as_mut_ptr().template cast<U>();
        new (p.as_raw_ptr()) U(rstd::forward<U>(in));
        return from_raw(T::from_ptr(p.as_raw_ptr()));
    }

    /// Constructs a `Box` from a raw mutable pointer.
    /// \param raw The raw pointer that was previously obtained from `into_raw`.
    /// \return A `Box` that takes ownership of the pointed-to value.
    constexpr static Box from_raw(mut_ptr<T> raw) noexcept {
        return Box { NonNull<T>::make_unchecked(raw) };
    }

    /// Consumes the `Box`, returning the wrapped raw pointer without deallocating.
    /// \return A mutable pointer to the heap-allocated value.
    constexpr auto into_raw() && noexcept -> mut_ptr<T> {
        auto b = ManuallyDrop<>::make(rstd::move(*this));
        return b->m_ptr.as_mut_ptr();
    }

    /// Returns a raw pointer to the contained value.
    /// \return A raw pointer to the heap-allocated value.
    constexpr auto get() noexcept -> mut_ptr<T>::value_type* {
        return m_ptr.as_mut_ptr().as_raw_ptr();
    }

    /// Returns an immutable borrow of the contained value.
    constexpr auto deref() const noexcept -> ref<T> { return as_ref(); }
    /// Returns a mutable borrow of the contained value.
    constexpr auto deref_mut() noexcept -> mut_ref<T> { return m_ptr.as_mut_ptr().as_mut_ref(); }
    /// Returns `true` if this `Box` holds a valid (non-null) pointer.
    explicit constexpr operator bool() const noexcept { return ! rstd::mem::all(m_ptr, 0); }

    /// Destroys the contained value and deallocates the memory.
    void reset() noexcept {
        if (! rstd::mem::all(m_ptr, 0)) {
            auto mptr         = m_ptr.as_mut_ptr();
            auto raw_non_null = NonNull<u8>::make_unchecked(
                mut_ptr<u8>::from_raw_parts(reinterpret_cast<u8*>(mptr.as_raw_ptr())));
            Layout layout {};

            if constexpr (mtp::is_array<T>) {
                using V   = mtp::rm_ext<T>;
                usize len = mptr.len();
                auto* p   = reinterpret_cast<V*>(mptr.as_raw_ptr());
                for (usize i = 0; i < len; ++i) {
                    p[i].~V();
                }
                layout = Layout::array<V>(len).unwrap();
            } else if constexpr (requires { mptr.metadata()->drop; }) {
                auto const* meta = mptr.metadata();
                meta->drop(mptr.as_raw_ptr());
                layout = Layout::from_size_align(meta->size, meta->align).unwrap();
            } else {
                rstd::destroy_at(mptr.as_raw_ptr());
                layout = Layout::make<T>();
            }
            as<Allocator>(GLOBAL).deallocate(raw_non_null, layout);
            rstd::mem::fill(m_ptr, 0);
        }
    }

    /// Returns an immutable reference to the contained value.
    /// \return A `ref<T>` to the boxed value.
    constexpr auto as_ref() const noexcept -> ref<T> { return m_ptr.as_ptr().as_ref(); }

    /// Returns a const pointer to the contained value.
    /// \return A `ptr<T>` to the boxed value.
    constexpr auto as_ptr() const noexcept -> ptr<T> { return m_ptr.as_ptr(); }
    /// Returns a mutable pointer to the contained value.
    /// \return A `mut_ptr<T>` to the boxed value.
    constexpr auto as_mut_ptr() const noexcept -> mut_ptr<T> { return m_ptr.as_mut_ptr(); }

    /// Creates a new `Box` by cloning all elements of the contained array.
    /// \return A new `Box` owning a cloned copy of the array.
    auto clone() const -> Self
        requires mtp::is_array<T>
    {
        using V     = mtp::rm_ext<T>;
        auto old    = as_ptr();
        auto length = old.len();
        auto layout = Layout::array<V>(length).unwrap();

        auto res = as<Allocator>(GLOBAL).allocate(layout);
        if (res.is_err()) handle_alloc_error(layout);

        auto* raw = static_cast<V*>(res.unwrap_unchecked().as_mut_ptr().as_raw_ptr());
        for (usize i = 0; i < length; ++i) {
            new (raw + i) V(old[i]);
        }
        auto p = mut_ptr<T>::from_raw_parts(raw, length);
        return from_raw(p);
    }
};
} // namespace alloc::boxed

using ::alloc::boxed::Box;
namespace rstd
{

} // namespace rstd

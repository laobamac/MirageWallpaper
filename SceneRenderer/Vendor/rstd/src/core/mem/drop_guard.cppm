module;
#include <rstd/macro.hpp>
export module rstd.core:mem.drop_guard;
export import :cmp;
export import :ops.deref;
export import :mem.manually_drop;

using rstd::mem::manually_drop::ManuallyDrop;

namespace rstd::mem::drop_guard
{
/// A scope guard that invokes a callable on the inner value when dropped.
///
/// On destruction, the inner value is moved out and passed to the stored callable `F`.
/// Useful for ensuring cleanup logic runs even when leaving scope via an exception.
/// \tparam T The guarded value type.
/// \tparam F A callable type invoked as `f(T*)` on drop.
export template<typename T, typename F>
struct DropGuard {
    ManuallyDrop<T> inner;
    ManuallyDrop<F> f;

    USE_TRAIT_WIN(DropGuard)

    ~DropGuard() {
        auto inner = rstd::move(this->inner).take();
        auto f     = this->f.take();
        f(rstd::addressof(inner));
    }

    explicit DropGuard(T&& inner, F&& f)
        : inner(ManuallyDrop<T>::make(rstd::move(inner))),
          f(ManuallyDrop<F>::make(rstd::move(f))) {}

    /// Creates a `DropGuard` wrapping the value and drop function.
    /// \param inner The value to guard.
    /// \param f The callable invoked with a pointer to `inner` on destruction.
    /// \return A new `DropGuard`.
    static auto make(T&& inner, F&& f) -> Self {
        return { .inner = ManuallyDrop<T>::make(rstd::move(inner)),
                 .f     = ManuallyDrop<F>::make(rstd::move(f)) };
    }

    constexpr auto       operator->() noexcept { return as_ptr(); }
    constexpr const auto operator->() const noexcept { return as_ptr(); }

    /// Returns a const pointer to the guarded value.
    constexpr auto as_ptr() const noexcept { return inner.as_ptr(); }

    /// Returns a mutable pointer to the guarded value.
    constexpr auto as_mut_ptr() noexcept { return inner.as_mut_ptr(); }
};
} // namespace rstd::mem::drop_guard

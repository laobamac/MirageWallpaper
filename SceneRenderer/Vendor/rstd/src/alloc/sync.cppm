module;
#include <rstd/macro.hpp>
export module rstd.alloc:sync;
export import rstd.core;

using rstd::mem::maybe_uninit::maybe_uninit_traits;
using rstd::mem::maybe_uninit::MaybeUninit;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::fence;
namespace mtp = rstd::mtp;
using namespace rstd::prelude;

namespace alloc::sync
{

constexpr usize MAX_REFCOUNT = rstd::numeric_limits<usize>::max() / 2;

enum class DeleteType
{
    Value = 0,
    Self,
};

using EmbedDeconstructor = void (*)(voidp);

struct ArcImplTrait {
    template<typename Self, typename = void>
    struct Api {
        using Trait = ArcImplTrait;
        auto data() noexcept -> voidp { return rstd::trait_call<0>(this); }
        void do_delete(DeleteType t, EmbedDeconstructor d) { rstd::trait_call<1>(this, t, d); }
    };

    template<typename F>
    using Funcs = TraitFuncs<&F::data, &F::do_delete>;
};

template<class T>
struct ArcInner {
    Atomic<usize> strong { 1 };
    Atomic<usize> weak { 1 };

    mut_ptr<dyn<ArcImplTrait>> impl;

    ArcInner(mut_ptr<dyn<ArcImplTrait>> i): impl(i) {}

    auto data() noexcept -> mut_ptr<T> {
        return mut_ptr<T>::from_raw_parts(rstd::launder(static_cast<T*>(impl->data())));
    }
    void do_delete(DeleteType t, EmbedDeconstructor de) { return impl->do_delete(t, de); }

    static void embed_deconstruct(voidp p) { rstd::destroy_at(static_cast<T*>(p)); }

    void inc_strong() {
        [[maybe_unused]]
        auto old = strong.fetch_add(1, rstd::sync::atomic::Ordering::Relaxed);
        debug_assert(old < MAX_REFCOUNT);
    }

    void inc_weak() {
        [[maybe_unused]]
        auto old = weak.fetch_add(1, rstd::sync::atomic::Ordering::Relaxed);
        debug_assert(old < MAX_REFCOUNT);
    }

    bool try_inc_strong() {
        usize cur = strong.load(rstd::sync::atomic::Ordering::Acquire);
        while (cur != 0) {
            debug_assert(cur < MAX_REFCOUNT);
            if (strong.compare_exchange_weak(cur,
                                             cur + 1,
                                             rstd::sync::atomic::Ordering::AcqRel,
                                             rstd::sync::atomic::Ordering::Acquire)) {
                return true;
            }
        }
        return false;
    }

    void drop_strong() noexcept {
        // If we were the last strong, destroy T and release the implicit weak.
        if (strong.fetch_sub(1, rstd::sync::atomic::Ordering::AcqRel) == 1) {
            // Synchronize with readers of T through acquire on last release.
            rstd::sync::atomic::fence(rstd::sync::atomic::Ordering::Acquire);

            do_delete(DeleteType::Value, &embed_deconstruct);

            // release the implicit weak held by strong pointers
            if (weak.fetch_sub(1, rstd::sync::atomic::Ordering::AcqRel) == 1) {
                rstd::sync::atomic::fence(rstd::sync::atomic::Ordering::Acquire);
                do_delete(DeleteType::Self, &embed_deconstruct);
            }
        }
    }

    void drop_weak() noexcept {
        if (weak.fetch_sub(1, rstd::sync::atomic::Ordering::AcqRel) == 1) {
            rstd::sync::atomic::fence(rstd::sync::atomic::Ordering::Acquire);
            do_delete(DeleteType::Self, &embed_deconstruct);
        }
    }
};

template<typename T>
struct ArcInnerImpl : ArcInner<T> {
    static_assert(Impled<T, Sized>);

    alignas(T) rstd::byte storage[sizeof(T)];

    ArcInnerImpl();

    auto data() noexcept -> voidp { return &storage; }
    void do_delete(DeleteType t, EmbedDeconstructor deconstruct) {
        if (t == DeleteType::Value) {
            deconstruct(&storage);
        } else {
            delete this;
        }
    }
};

/// A thread-safe reference-counting pointer, analogous to Rust's `Arc<T>`.
/// \tparam T The type of the value managed by atomic reference counting.
export template<typename T>
class Arc;
/// A non-owning, weakly-referenced companion to `Arc` that does not prevent deallocation.
/// \tparam T The type of the referenced value.
export template<typename T>
class Weak;

} // namespace alloc::sync

namespace alloc
{
template<typename T>
sync::ArcInnerImpl<T>::ArcInnerImpl(): ArcInner<T>(dyn<ArcImplTrait>::from_ptr(this)) {
}

} // namespace alloc

namespace alloc::sync
{
template<typename T>
struct ArcData {
    ArcInner<T>* inner { nullptr };
};

/// A raw representation of an `Arc` pointer, used for low-level interop.
/// \tparam T The element type.
export template<typename T>
class ArcRaw {
    ArcInner<T>* inner;

    friend class Arc<T>;
    ArcRaw(auto t): inner(t) {}

public:
    /// Returns a mutable pointer to the managed value.
    auto as_ptr() const { return inner->data(); };
    /// Consumes the `ArcRaw`, returning a raw void pointer without releasing ownership.
    /// \return A raw void pointer to the inner allocation.
    auto into_raw() -> voidp { return rstd::exchange(inner, nullptr); }

    /// Reconstructs an `ArcRaw` from a raw void pointer previously obtained via `into_raw`.
    /// \param p The raw void pointer.
    /// \return An `ArcRaw` wrapping the pointer.
    static auto from_raw(voidp p) -> ArcRaw { return { static_cast<ArcInner<T>*>(p) }; }
};

/// A thread-safe reference-counting pointer, analogous to Rust's `Arc<T>`.
/// \tparam T The type of the value managed by atomic reference counting.
export template<typename T>
class Arc : public DefaultInClass<Arc<T>, Clone> {
    ArcData<T> self;

    template<typename>
    friend class Weak;

    template<typename>
    friend class Arc;

    constexpr Arc(ArcData<T> s): self(s) {}

public:
    USE_TRAIT_WIN(Arc)

    using Target       = T;
    using element_type = T;

    // Copy
    constexpr Arc(const Arc&)            = delete;
    constexpr Arc& operator=(const Arc&) = delete;

    // Move
    constexpr Arc(Arc&& other) noexcept: self({ rstd::exchange(other.self, {}) }) {}
    constexpr Arc& operator=(Arc&& other) noexcept {
        if (this != &other) {
            reset();
            self = rstd::exchange(other.self, {});
        }
        return *this;
    }

    ~Arc() { reset(); }

    auto clone() const -> Arc {
        if (self.inner) {
            self.inner->inc_strong();
        }
        return { self };
    }

    /// Drops the current allocation, decrementing the strong count.
    void reset() {
        if (self.inner) {
            self.inner->drop_strong();
            self.inner = nullptr;
        }
    }

    /// Constructs a new `Arc<T>`.
    template<typename... Args>
    static auto make(Args&&... args) -> Arc {
        auto inner = new ArcInnerImpl<T>;
        rstd::construct_at(reinterpret_cast<T*>(&(inner->storage)), rstd::forward<Args>(args)...);
        return { ArcData<T> { .inner = inner } };
    }

    /// Creates a new `Arc` containing an uninitialized value.
    ///
    /// The returned Arc contains a `MaybeUninit<T>` that must be initialized
    /// before calling `assume_init()` to convert to `Arc<T>`.
    ///
    /// # Example
    /// ```cpp
    /// auto arc = Arc<int>::make_uninit();
    /// // Initialize the value
    /// Arc::get_mut(arc).unwrap().write(42);
    /// auto initialized = arc.assume_init();
    /// ```
    static auto make_uninit() -> Arc<MaybeUninit<T>>
        requires Impled<T, Sized>
    {
        return Arc<MaybeUninit<T>>::make(MaybeUninit<T>::uninit());
    }

    /// Reconstructs an `Arc` from an `ArcRaw` previously obtained via `into_raw`.
    /// \param r The raw Arc representation.
    /// \return An `Arc` that takes back ownership.
    static Arc from_raw(ArcRaw<T> r) noexcept { return { ArcData<T> { .inner = r.inner } }; }

    /// Converts an `Arc<MaybeUninit<T>>` into an `Arc<T>` after the value has been initialized.
    /// \return An `Arc<T>` over the now-initialized value.
    auto assume_init()
        requires(! mtp::same_as<typename maybe_uninit_traits<T>::value_type, void>)
    {
        using V    = maybe_uninit_traits<T>::value_type;
        auto inner = rstd::launder(reinterpret_cast<ArcInner<V>*>(self.inner));
        self.inner = nullptr;
        return Arc<V> { { .inner = inner } };
    }

    /// Returns an immutable borrow of the managed value.
    auto deref() const noexcept -> ref<T> { return self.inner->data().as_ref(); }
    /// Returns a mutable borrow of the managed value.
    auto deref_mut() const noexcept -> mut_ref<T> { return self.inner->data().as_mut_ref(); }
    /// Returns `true` if this `Arc` is non-empty.
    explicit operator bool() const noexcept { return self.inner != nullptr; }

    /// Returns the number of strong (`Arc`) pointers to the same allocation.
    /// \return The current strong reference count.
    usize strong_count() const noexcept {
        return self.inner ? self.inner->strong.load(rstd::sync::atomic::Ordering::Acquire) : 0;
    }

    /// Returns the number of `Weak` pointers to the same allocation.
    /// \return The current weak reference count (excluding the implicit weak held by strong pointers).
    usize weak_count() const noexcept {
        // Rust's Arc::weak_count excludes the implicit weak if strong>0.
        if (! self.inner) return 0;
        auto w = self.inner->weak.load(rstd::sync::atomic::Ordering::Acquire);
        auto s = self.inner->strong.load(rstd::sync::atomic::Ordering::Acquire);
        if (s == 0) return w;
        return w > 0 ? (w - 1) : 0;
    }

    /// Creates a `Weak` pointer to the same allocation without incrementing the strong count.
    /// \return A `Weak<T>` pointer.
    Weak<T> downgrade() const noexcept;

    /// Returns a raw pointer to the managed value.
    auto as_ptr() const noexcept {
        if (! self.inner) [[unlikely]] {
            rstd::panic { "Arc::as_ptr called on an empty Arc" };
        }
        return self.inner->data();
    }

    /// Returns true if the two `Arc`s point to the same allocation
    /// (not just values that compare as equal).
    static bool ptr_eq(const Arc& a, const Arc& b) noexcept { return a.self.inner == b.self.inner; }

    /// Returns true if this is the only `Arc` or `Weak` pointer to the allocation.
    static bool is_unique(const Arc& arc) noexcept {
        return arc.self.inner &&
               arc.self.inner->strong.load(rstd::sync::atomic::Ordering::Acquire) == 1 &&
               arc.self.inner->weak.load(rstd::sync::atomic::Ordering::Acquire) == 1;
    }

    /// Returns a mutable reference to the inner value if there are no other
    /// `Arc` or `Weak` pointers to the same allocation.
    auto get_mut() const noexcept -> Option<mut_ref<T>> {
        if (is_unique(*this)) {
            return Some(self.inner->data().as_mut_ref());
        }
        return None();
    }

    /// Attempts to unwrap the `Arc`, returning the inner value if this is the only strong reference.
    /// \return `Ok(T)` if successful, or `Err(Arc)` if other strong references exist.
    auto try_unwrap() -> Result<T, Arc>
        requires Impled<T, Sized>
    {
        do {
            if (self.inner == nullptr) break;
            // Attempt to drop strong to 0 with CAS to avoid races.
            usize expected = 1;
            if (! self.inner->strong.compare_exchange_strong(
                    expected,
                    0,
                    rstd::sync::atomic::Ordering::Relaxed,
                    rstd::sync::atomic::Ordering::Relaxed)) {
                break;
            }
            rstd::sync::atomic::fence(rstd::sync::atomic::Ordering::Acquire);
            auto w = Weak<T> { self };
            self   = {};
            return Ok(rstd::move(*(w.self.inner->data())));
        } while (0);
        return Err(rstd::move(*this));
    }

    /// Consumes the `Arc` without decrementing the reference count, returning an `ArcRaw`.
    /// \return An `ArcRaw<T>` for low-level interop; must be reconverted with `from_raw`.
    auto into_raw() noexcept {
        auto inner = self.inner;
        self       = {}; // leak ownership to raw
        return ArcRaw<T> { inner };
    }
};

/// A thread-safe weak reference to an `Arc`-managed allocation.
/// \tparam T The type of the referenced value.
export template<class T>
class Weak : public DefaultInClass<Weak<T>, Clone> {
    ArcData<T> self;

#if !defined(RSTD_OS_WINDOWS)
    template<typename, typename>
    friend struct rstd::Impl;
#endif
    friend class Arc<T>;

    constexpr Weak(ArcData<T> s) noexcept: self(s) {}

public:
    using element_type = T;

    // Copy
    constexpr Weak(const Weak& other)            = delete;
    constexpr Weak& operator=(const Weak& other) = delete;

    // Move
    constexpr Weak(Weak&& other) noexcept: self(rstd::exchange(other.self, {})) {}
    constexpr Weak& operator=(Weak&& other) noexcept {
        if (this != &other) {
            reset();
            self = rstd::exchange(other.self, {});
        }
        return *this;
    }

    ~Weak() { reset(); }

    auto clone() const -> Weak {
        if (self.inner) {
            self.inner->inc_weak();
        }
        return { self };
    }

    /// Constructs a new empty `Weak<T>` without allocating any memory.
    /// Calling `upgrade` on the return value always gives an empty `Arc`.
    static constexpr auto make() noexcept -> Weak {
        return Weak { ArcData<T> { .inner = nullptr } };
    }

    /// Releases the weak reference, decrementing the weak count.
    void reset() {
        if (self.inner) {
            self.inner->drop_weak();
            self.inner = nullptr;
        }
    }

    /// Attempts to upgrade the `Weak` pointer to an `Arc`, succeeding only if strong references remain.
    /// \return An `Arc<T>` if the value is still alive, or an empty `Arc` otherwise.
    auto upgrade() const noexcept -> Arc<T> {
        if (self.inner && self.inner->try_inc_strong()) {
            return { self };
        }
        return { {} };
    }

    /// Returns the number of strong (`Arc`) pointers to the same allocation.
    /// \return The current strong reference count.
    auto strong_count() const noexcept -> usize {
        return self.inner ? self.inner->strong.load(rstd::sync::atomic::Ordering::Acquire) : 0;
    }

    /// Returns the number of weak pointers to the same allocation (including this one).
    /// \return The current weak reference count.
    auto weak_count() const noexcept -> usize {
        return self.inner ? self.inner->weak.load(rstd::sync::atomic::Ordering::Acquire) : 0;
    }

    /// Returns `true` if the value has been dropped (strong count is zero).
    bool expired() const noexcept { return strong_count() == 0; }

    /// Returns a raw pointer to the managed value.
    auto as_ptr() const noexcept { return self.inner->data(); }
};

template<class T>
auto Arc<T>::downgrade() const noexcept -> Weak<T> {
    Weak<T> out { self };
    if (self.inner) {
        self.inner->inc_weak();
    }
    return out;
}

} // namespace alloc::sync

module;
#include <rstd/macro.hpp>
export module rstd.alloc:rc;
export import rstd.core;

using namespace rstd::prelude;
namespace mtp = rstd::mtp;

namespace alloc::rc
{

template<typename Allocator, typename T>
using rebind_alloc = typename mtp::allocator_traits<Allocator>::template rebind_alloc<T>;

constexpr bool noexp { false };

void increase_count(usize& count) {
    if (count == rstd::numeric_limits<usize>::max()) {
        rstd::panic("reference count overflow");
    }
    ++count;
}

template<typename T>
constexpr auto compute_alignment() -> usize {
    return rstd::cmp::max(alignof(mtp::rm_ext<T>), alignof(usize));
}

template<typename T>
[[nodiscard]]
constexpr auto layout_for_value(usize align) -> usize {
    const auto size = sizeof(T);
    return (size + align - 1) & ~(align - 1);
}

template<typename T>
[[nodiscard]]
constexpr auto layout_for_value() -> usize {
    return layout_for_value<T>(alignof(T));
}

enum class DeleteType
{
    Value = 0,
    Self,
};

enum class StoragePolicy : u32
{
    Embed = 0,
    Separate,
    SeparateWithDeleter
};

template<typename T>
struct alignas(usize) RcInner {
    using value_t = mtp::rm_ext<T>;

    usize    strong { 1 };
    usize    weak { 1 };
    value_t* value { nullptr };

    RcInner() noexcept {}
    virtual ~RcInner() = default;

    virtual void do_delete(DeleteType t) {
        auto self = this;
        if (t == DeleteType::Value) {
            delete self->value;
        } else {
            delete self;
        }
    }

    void inc_strong() { increase_count(strong); }
    void dec_strong() { --strong; }
    void inc_weak() { increase_count(weak); }
    void dec_weak() { --weak; }
};

template<typename T>
using RcInnerConst = RcInner<mtp::add_const<T>>;

template<typename T, StoragePolicy P, typename ValueDeleter = void>
struct RcInnerImpl {
    static_assert(false);
};

template<typename T>
struct RcInnerImpl<T, StoragePolicy::Embed> : RcInnerConst<T> {
    static_assert(! mtp::is_array<T>);

    alignas(T) rstd::byte storage[sizeof(T)];

    RcInnerImpl() noexcept: RcInnerConst<T>() {}

    void do_delete(DeleteType t) override {
        auto self = this;
        if (t == DeleteType::Value) {
            self->value->~T();
            self->value = nullptr;
        } else {
            delete self;
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto ptr    = new (storage) T(rstd::forward<Args>(args)...);
        this->value = ptr;
    }
};

template<typename T>
struct RcInnerImpl<T, StoragePolicy::Separate> : RcInnerConst<T> {
    RcInnerImpl() noexcept: RcInnerConst<T>() {}

    void do_delete(DeleteType t) override {
        auto self = this;
        if (t == DeleteType::Value) {
            delete self->value;
            self->value = nullptr;
        } else {
            delete self;
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto ptr    = new T(rstd::forward<Args>(args)...);
        this->value = ptr;
    }
};

template<typename T>
struct RcInnerArrayImpl : RcInnerConst<T> {
    const usize size;

    RcInnerArrayImpl(usize n) noexcept: RcInnerConst<T>(), size(n) {}
};

template<typename T>
struct RcInnerImpl<T[], StoragePolicy::Separate> : RcInnerArrayImpl<T[]> {
    using value_t = mtp::rm_ext<T>;

    RcInnerImpl(usize n) noexcept: RcInnerArrayImpl<T[]>(n) {}

    void do_delete(DeleteType t) override {
        auto self = this;
        if (t == DeleteType::Value) {
            auto ptr = const_cast<mtp::rm_const<value_t>*>(self->value);
            for (usize i = 0; i < this->size; i++) {
                (ptr + i)->~value_t();
            }
            ::operator delete[](
                (void*)ptr, sizeof(value_t) * this->size, rstd::align_val_t { alignof(value_t) });
            self->value = nullptr;
        } else {
            delete self;
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto* ptr = static_cast<value_t*>(
            ::operator new[](sizeof(value_t) * this->size, rstd::align_val_t { alignof(value_t) }));
        this->value = ptr;
        for (usize i = 0; i < this->size; i++) {
            new (ptr + i) value_t(rstd::forward<Args>(args)...);
        }
    }
};

template<typename T, typename ValueDeleter>
struct RcInnerImpl<T, StoragePolicy::SeparateWithDeleter, ValueDeleter> : RcInnerConst<T> {
    ValueDeleter value_deletor;

    RcInnerImpl(T* p, ValueDeleter d): RcInnerConst<T>(), value_deletor(rstd::move(d)) {
        this->value = p;
    }

    void do_delete(DeleteType t) override {
        auto self = this;

        if (t == DeleteType::Value) {
            self->value_deletor(const_cast<mtp::rm_cv<T>*>(self->value));
            self->value = nullptr;
        } else {
            delete self;
        }
    }
};

template<typename T, typename Allocator, StoragePolicy P, typename ValueDeleter = void>
struct RcInnerAllocImpl {
    static_assert(false);
};

template<typename T, typename Allocator>
struct RcInnerAllocImpl<T, Allocator, StoragePolicy::Embed> : RcInnerImpl<T, StoragePolicy::Embed> {
    using base_t = RcInnerImpl<T, StoragePolicy::Embed>;
    Allocator allocator;

    RcInnerAllocImpl(Allocator a): base_t(), allocator(a) {}
    void do_delete(DeleteType t) override {
        auto self = this;
        if (t == DeleteType::Value) {
            self->value->~T();
            self->value     = nullptr;
            self->has_value = false;
        } else {
            auto self_allocator = rebind_alloc<Allocator, RcInnerAllocImpl>(self->allocator);
            self_allocator.deallocate(self, 1);
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        base_t::template allocate_value<Args...>(rstd::forward<Args>(args)...);
    }
};

template<typename T, typename Allocator>
struct RcInnerAllocImpl<T, Allocator, StoragePolicy::Separate>
    : RcInnerImpl<T, StoragePolicy::Separate> {
    static_assert(! mtp::is_array<T>);
    using base_t = RcInnerImpl<T, StoragePolicy::Separate>;
    Allocator allocator;

    RcInnerAllocImpl(Allocator a): base_t(), allocator(a) {}
    void do_delete(DeleteType t) override {
        auto self = this;
        if (t == DeleteType::Value) {
            self->value->~T();
            self->allocator.deallocate(const_cast<mtp::rm_cv<T>*>(self->value), 1);
            self->value = nullptr;
        } else {
            auto self_allocator = rebind_alloc<Allocator, RcInnerAllocImpl>(self->allocator);
            self_allocator.deallocate(self, 1);
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto* ptr = allocator.allocate(1);
        new (ptr) T(rstd::forward<Args>(args)...);
        this->value = ptr;
    }
};

template<typename T, typename Allocator>
struct RcInnerAllocImpl<T[], Allocator, StoragePolicy::Separate>
    : RcInnerImpl<T[], StoragePolicy::Separate> {
    using base_t  = RcInnerImpl<T[], StoragePolicy::Separate>;
    using value_t = mtp::rm_ext<T>;
    Allocator allocator;

    RcInnerAllocImpl(Allocator a, usize n): base_t(n), allocator(a) {}
    void do_delete(DeleteType t) override {
        auto self = this;
        if (t == DeleteType::Value) {
            auto n = base_t::size;
            for (usize i = 0; i < n; i++) {
                (self->value + i)->~value_t();
            }
            auto p = const_cast<mtp::rm_const<value_t>*>(self->value);
            self->allocator.deallocate(p, n);
            self->value = nullptr;
        } else {
            auto self_allocator = rebind_alloc<Allocator, RcInnerAllocImpl>(self->allocator);
            self_allocator.deallocate(self, 1);
        }
    }

    template<typename... Args>
    void allocate_value(Args&&... args) {
        auto  n     = this->size;
        auto* ptr   = allocator.allocate(n);
        this->value = ptr;
        for (usize i = 0; i < n; i++) {
            new (ptr + i) value_t(rstd::forward<Args>(args)...);
        }
    }
};

template<typename T, typename Allocator, typename ValueDeleter>
struct RcInnerAllocImpl<T, Allocator, StoragePolicy::SeparateWithDeleter, ValueDeleter>
    : RcInnerImpl<T, StoragePolicy::SeparateWithDeleter, ValueDeleter> {
    using base_t = RcInnerImpl<T, StoragePolicy::Separate>;
    Allocator allocator;

    RcInnerAllocImpl(Allocator a, T* p, ValueDeleter d): base_t(p, rstd::move(d)), allocator(a) {}

    void do_delete(DeleteType t) override {
        auto self = this;

        if (t == DeleteType::Value) {
            base_t::value_deletor(self->value);
            self->value = nullptr;
        } else {
            auto self_allocator = rebind_alloc<Allocator, RcInnerAllocImpl>(self->allocator);
            self_allocator.deallocate(self, 1);
        }
    }
};

/// A single-threaded reference-counting pointer, analogous to Rust's `Rc<T>`.
/// \tparam T The type of the value managed by reference counting.
export template<typename T>
class Rc;

struct RcMakeHelper {
    template<typename T>
    static auto make_rc(Rc<T>::inner_t* inner) noexcept {
        return Rc<T>(inner);
    }
};

/// A non-owning reference to an `Rc`-managed allocation that does not prevent deallocation.
/// \tparam T The type of the referenced value.
export template<typename T>
class Weak final {
    friend class Rc<T>;
    using inner_t = RcInner<mtp::add_const<T>>;
    inner_t* m_ptr;

    explicit Weak(inner_t* p) noexcept: m_ptr(p) {}

public:
    /// Creates an empty `Weak` pointer.
    Weak() noexcept: m_ptr(nullptr) {}

    /// Copy constructs a `Weak` pointer, incrementing the weak count.
    Weak(const Weak& other) noexcept: Weak(other.clone()) {}

    /// Move constructs a `Weak` pointer, taking ownership from `other`.
    Weak(Weak&& other) noexcept: m_ptr(other.m_ptr) { other.m_ptr = nullptr; }

    ~Weak() {
        if (m_ptr) {
            m_ptr->dec_weak();
            if (m_ptr->weak == 0) {
                m_ptr->do_delete(DeleteType::Self);
            }
        }
    }

    /// Creates a new `Weak` pointer to the same allocation.
    /// \return A cloned `Weak` pointer.
    auto clone() const noexcept -> Weak {
        if (m_ptr) m_ptr->inc_weak();
        return Weak(m_ptr);
    }

    /// Attempts to upgrade the `Weak` pointer to an `Rc`, returning `None` if the value has been dropped.
    /// \return An `Option<Rc<T>>` containing the upgraded pointer, or `None`.
    auto upgrade() const -> Option<Rc<T>> {
        if (! m_ptr || m_ptr->strong == 0) return {};
        m_ptr->inc_strong();
        return Some(RcMakeHelper::make_rc<T>(m_ptr));
    }

    /// Returns the number of strong (`Rc`) pointers to the same allocation.
    /// \return The strong reference count.
    auto strong_count() const -> usize { return m_ptr ? m_ptr->strong : 0; }

    /// Returns the number of `Weak` pointers to the same allocation.
    /// \return The weak reference count (excluding the implicit weak held by strong pointers).
    auto weak_count() const -> usize { return m_ptr ? m_ptr->weak - 1 : 0; }
};

template<typename T>
class RcBase {
protected:
    using inner_t = RcInner<mtp::add_const<T>>;
    inner_t* m_ptr;

    explicit RcBase(inner_t* p) noexcept: m_ptr(p) {}

    auto inner() const { return m_ptr; }
    auto inner() { return m_ptr; }

    auto drop() {
        if (m_ptr) {
            m_ptr->dec_strong();
            if (m_ptr->strong == 0) {
                m_ptr->do_delete(DeleteType::Value);
                m_ptr->dec_weak();
                if (m_ptr->weak == 0) {
                    m_ptr->do_delete(DeleteType::Self);
                }
            }
        }
    }

    template<typename Deleter>
    static auto allocate_inner(T* p, Deleter&& d) -> inner_t* {
        return new RcInnerImpl<T, StoragePolicy::SeparateWithDeleter, Deleter>(p, rstd::move(d));
    }

    template<typename Deleter, typename Allocator>
    static auto allocate_inner(T* p, Allocator alloc, Deleter&& d) -> inner_t* {
        using inner_t = RcInnerAllocImpl<T, Allocator, StoragePolicy::SeparateWithDeleter, Deleter>;
        auto self_allocator = rebind_alloc<Allocator, inner_t>(alloc);
        auto mem            = (rstd::byte*)self_allocator.allocate(1);
        return new (mem) inner_t(alloc, p, rstd::move(d));
    }

public:
    using value_t       = mtp::rm_ext<T>;
    using const_value_t = mtp::add_const<mtp::rm_ext<T>>;

    auto strong_count() const -> usize { return m_ptr ? m_ptr->strong : 0; }

    auto weak_count() const -> usize { return m_ptr ? m_ptr->weak - 1 : 0; }

    auto is_unique() const -> bool { return strong_count() == 1 && weak_count() == 0; }

    auto size() const -> usize {
        if (m_ptr) {
            if constexpr (mtp::is_array<T>) {
                auto p = static_cast<const RcInnerArrayImpl<T>*>(m_ptr);
                return p->size;
            } else {
                return 1;
            }
        } else {
            return 0;
        }
    }

    explicit operator bool() const noexcept { return m_ptr != nullptr; }

    void reset() {
        drop();
        m_ptr = nullptr;
    }
    auto get() const noexcept -> const_value_t* {
        auto p = this->inner();
        return p ? p->value : nullptr;
    }
};

template<typename T>
class RcAdaptor : public RcBase<T> {
protected:
    using inner_t = RcBase<T>::inner_t;
    explicit RcAdaptor(inner_t* p) noexcept: RcBase<T>(p) {}

public:
    using value_t       = RcBase<T>::value_t;
    using const_value_t = RcBase<T>::const_value_t;
    auto to_const() const -> Rc<const T> {
        this->m_ptr->inc_strong();
        return RcMakeHelper::make_rc<const T>(this->m_ptr);
    }
    auto get() noexcept -> value_t* {
        auto p = this->inner();
        return p ? const_cast<value_t*>(p->value) : nullptr;
    }
    auto get() const noexcept -> const_value_t* {
        auto p = this->inner();
        return p ? p->value : nullptr;
    }
};
template<typename F>
class RcAdaptor<const F> : public RcBase<const F> {
    using T = const F;

protected:
    using inner_t = RcBase<T>::inner_t;
    explicit RcAdaptor(inner_t* p) noexcept: RcBase<T>(p) {}

public:
    using const_value_t = RcBase<T>::const_value_t;
    auto get() const noexcept -> const_value_t* {
        auto p = this->inner();
        return p ? p->value : nullptr;
    }
};

/// A single-threaded reference-counting pointer, analogous to Rust's `Rc<T>`.
/// \tparam T The type of the value managed by reference counting.
export template<typename T>
class Rc final : public RcAdaptor<T> {
    friend struct RcMakeHelper;
    using inner_t = RcBase<T>::inner_t;
    explicit Rc(inner_t* p) noexcept: RcAdaptor<T>(p) {}

public:
    USE_TRAIT_WIN(Rc)

    using Target = mtp::rm_const<T>;

    /// Creates an empty `Rc` that does not point to any allocation.
    Rc(): Rc((inner_t*)nullptr) {}
    ~Rc() { RcBase<T>::drop(); }

    /// Copy constructs an `Rc`, incrementing the strong reference count.
    Rc(const Rc& other) noexcept(noexp): Rc(other.clone()) {}
    Rc& operator=(const Rc& other) noexcept(noexp) {
        if (this != &other) {
            Rc(other.clone()).swap(*this);
        }
        return *this;
    }

    Rc(Rc&& other) noexcept: Rc(other.m_ptr) { other.m_ptr = nullptr; }
    Rc& operator=(Rc&& other) noexcept {
        Rc(rstd::move(other)).swap(*this);
        return *this;
    }

    template<typename U>
        requires mtp::is_const<T> && mtp::same_as<mtp::rm_cv<T>, U>
    Rc(const Rc<U>& o): Rc(o.to_const()) {}

    /// Constructs an `Rc` from a raw pointer with the default deleter.
    /// \param p The raw pointer to take ownership of.
    explicit Rc(T* p): Rc(p, mtp::default_delete<T>()) {}
    /// Constructs an `Rc` from a raw pointer with a custom deleter.
    /// \tparam Deleter The type of the deleter callable.
    /// \param p The raw pointer to take ownership of.
    /// \param d The deleter to invoke when the value is dropped.
    template<typename Deleter>
    Rc(T* p, Deleter&& d): Rc(RcBase<T>::allocate_inner(p, rstd::move(d))) {}

    /// Constructs an `Rc` from a raw pointer with a custom deleter and allocator.
    /// \tparam Deleter The type of the deleter callable.
    /// \tparam Allocator The type of the allocator.
    /// \param p The raw pointer to take ownership of.
    /// \param d The deleter to invoke when the value is dropped.
    /// \param alloc The allocator used for the control block.
    template<typename Deleter, typename Allocator>
    Rc(T* p, Deleter&& d, Allocator alloc)
        : Rc(RcBase<T>::allocate_inner(p, alloc, rstd::move(d))) {}

    /// Creates a `Weak` pointer to the same allocation.
    /// \return A `Weak<T>` that does not contribute to the strong count.
    auto downgrade() const -> Weak<T> {
        this->m_ptr->inc_weak();
        return Weak<T>(this->m_ptr);
    }
    /// Creates a new `Rc` pointer to the same allocation, incrementing the strong count.
    /// \return A cloned `Rc`.
    auto clone() const noexcept(noexp) -> Rc {
        if (this->m_ptr) this->m_ptr->inc_strong();
        return Rc(this->m_ptr);
    }

    /// Returns an immutable borrow of the managed value.
    auto deref() const noexcept -> ref<Target> {
        if constexpr (mtp::is_array<Target>) {
            return ref<Target>::from_raw_parts(this->get(), this->size());
        } else {
            return ref<Target>::from_raw_parts(this->get());
        }
    }

    /// Swaps the inner pointers of two `Rc`s.
    /// \param other The other `Rc` to swap with.
    void swap(Rc& other) noexcept { rstd::swap(this->m_ptr, other.m_ptr); }
};

/// Constructs an `Rc<T>` by allocating and constructing `T` in place.
/// \tparam T The value type.
/// \tparam Sp The storage policy (default: `Separate`).
/// \tparam Args The constructor argument types.
/// \param args The arguments forwarded to the constructor of `T`.
/// \return An `Rc<T>` owning the newly constructed value.
export template<typename T, StoragePolicy Sp = StoragePolicy::Separate, typename... Args>
    requires(! mtp::is_array<T>)
auto make_rc(Args&&... args) -> Rc<T> {
    auto inner = new RcInnerImpl<T, Sp>();
    inner->allocate_value(rstd::forward<Args>(args)...);
    return RcMakeHelper::make_rc<T>(inner);
}

/// Constructs an `Rc<T[]>` for an array type, initializing `n` elements with `init`.
/// \tparam T The array element type.
/// \tparam Sp The storage policy (default: `Separate`).
/// \param n The number of elements in the array.
/// \param init The value used to initialize each element.
/// \return An `Rc<T[]>` owning the newly constructed array.
export template<typename T, StoragePolicy Sp = StoragePolicy::Separate, typename... Args>
    requires mtp::is_array<T>
auto make_rc(usize n, const typename Rc<T>::const_value_t& init) -> Rc<T> {
    auto inner = new RcInnerImpl<T, Sp>(n);
    inner->allocate_value(init);
    return RcMakeHelper::make_rc<T>(inner);
}

/// Constructs an `Rc<T>` using a custom allocator, constructing `T` in place.
/// \tparam T The value type.
/// \tparam Sp The storage policy (default: `Separate`).
/// \tparam Allocator The allocator type.
/// \tparam Args The constructor argument types.
/// \param alloc The allocator to use.
/// \param args The arguments forwarded to the constructor of `T`.
/// \return An `Rc<T>` owning the newly constructed value.
export template<typename T,
                StoragePolicy Sp = StoragePolicy::Separate,
                typename Allocator,
                typename... Args>
    requires(! mtp::is_array<T>)
auto allocate_make_rc(const Allocator& alloc, Args&&... args) -> Rc<T> {
    using inner_t       = RcInnerAllocImpl<T, Allocator, Sp>;
    auto self_allocator = rebind_alloc<Allocator, inner_t>(alloc);

    auto mem   = (rstd::byte*)self_allocator.allocate(1);
    auto inner = new (mem) inner_t(alloc);
    inner->allocate_value(rstd::forward<Args>(args)...);
    return RcMakeHelper::make_rc<T>(inner);
}
/// Constructs an `Rc<T[]>` for an array type using a custom allocator.
/// \tparam T The array element type.
/// \tparam Sp The storage policy (default: `Separate`).
/// \tparam Allocator The allocator type.
/// \param alloc The allocator to use.
/// \param n The number of elements in the array.
/// \param t The value used to initialize each element.
/// \return An `Rc<T[]>` owning the newly constructed array.
export template<typename T, StoragePolicy Sp = StoragePolicy::Separate, typename Allocator>
    requires mtp::is_array<T>
auto allocate_make_rc(const Allocator& alloc, usize n, typename Rc<T>::const_value_t& t) -> Rc<T> {
    using inner_t       = RcInnerAllocImpl<T, Allocator, Sp>;
    auto self_allocator = rebind_alloc<Allocator, inner_t>(alloc);

    auto mem   = (rstd::byte*)self_allocator.allocate(1);
    auto inner = new (mem) inner_t(alloc, n);
    inner->allocate_value(t);
    return RcMakeHelper::make_rc<T>(inner);
}

/// Swaps two `Rc` pointers.
/// \param lhs The first `Rc`.
/// \param rhs The second `Rc`.
export template<typename T>
void swap(Rc<T>& lhs, Rc<T>& rhs) noexcept {
    lhs.swap(rhs);
}
} // namespace alloc::rc

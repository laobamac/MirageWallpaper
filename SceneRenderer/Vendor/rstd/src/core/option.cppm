module;
#include <rstd/macro.hpp>
#include <rstd/enum.hpp>
export module rstd.core:option;
import :enum_;
export import :clone;
export import :panicking;
export import :slice;
export import :ops.function;
export import :ops.deref;
export import :forward;
export import :cmp;

namespace rstd::option
{

/// An optional value: either `Some` containing a value, or `None`.
/// \tparam T The type of the contained value.
export template<typename T>
class Option;

/// A sentinel type representing an untyped `None` value, implicitly convertible to any `Option<T>`.
export struct Unknown {
    template<typename T>
    constexpr operator Option<T>() {
        return {};
    }
};

/// Creates an `Option` containing the given value.
/// \tparam U Optional explicit type for the contained value; deduced if omitted.
/// \param val The value to wrap.
/// \return An `Option<T>` in the `Some` state.
export template<typename U = void, typename T>
[[gnu::always_inline]]
inline constexpr auto Some(T&& val) {
    if constexpr (mtp::same_as<U, void>) {
        using val_t = mtp::rm_ref<T>;
        if constexpr (mtp::triv_copy<val_t> && (! mtp::is_ref<val_t>)) {
            return Option<val_t>::make_with(rstd::move(val));
        } else {
            // use T here, as lvalue is Option<T&>
            // only move is Option<T>
            return Option<T>::make_with(rstd::forward<T>(val));
        }
    } else {
        return Option<U>::make_with(rstd::forward<T>(val));
    }
}

/// Creates an empty `Option` representing no value.
/// \tparam U Optional explicit type for the `Option`; deduced or returns `Unknown` if omitted.
/// \return An `Option<T>` in the `None` state, or an `Unknown` sentinel.
export template<typename U = void, typename T = Unknown>
[[gnu::always_inline]]
inline constexpr auto None(T&& = {}) {
    if constexpr (mtp::same_as<U, void>) {
        if constexpr (mtp::same_as<Unknown, T>) {
            return Unknown();
        } else {
            return Option<T>();
        }
    } else {
        return Option<U>();
    }
}

namespace detail
{
template<typename T>
struct option_adapter_l1 {};

template<typename T>
struct option_traits {};

template<typename T>
struct option_traits<Option<T>> {
    using value_type    = T;
    using ret_value_t   = T;
    using union_value_t = mtp::cond<mtp::is_ref<T>, mtp::add_ptr<mtp::rm_ref<T>>, T>;
};

template<typename T>
struct option_traits<Option<T>&&> : option_traits<Option<T>> {
    using ret_value_t = mtp::add_ref_rv<T>;
};

template<typename T>
struct option_traits<Option<T>&> : option_traits<Option<T>> {
    using ret_value_t = mtp::add_ref<T>;
};

template<typename T>
struct option_traits<const Option<T>&> : option_traits<Option<T>> {
    using ret_value_t = mtp::add_ref<mtp::add_const<T>>;
};

template<typename T>
struct option_traits<const Option<T>&&> : option_traits<Option<T>> {
    using ret_value_t = mtp::add_ref_rv<mtp::add_const<T>>;
};

export template<typename T, typename NonePayload, typename SomePayload>
struct option_storage : enum_detail::storage<NonePayload, SomePayload> {
    using base = enum_detail::storage<NonePayload, SomePayload>;

    constexpr option_storage() noexcept = default;

    template<usize I, typename... Args>
    explicit constexpr option_storage(enum_detail::in_place_index_t<I> in_place, Args&&... args)
        : base(in_place, rstd::forward<Args>(args)...) {}

    using base::get;
    using base::index;
    using base::is;
    using base::replace;
    using base::valid;
};

template<typename T, typename NonePayload, typename SomePayload>
struct option_storage<T&, NonePayload, SomePayload> {
    constexpr option_storage() noexcept = default;

    constexpr explicit option_storage(enum_detail::in_place_index_t<0>) noexcept
        : none_(), some_() {}

    template<typename V>
    explicit constexpr option_storage(enum_detail::in_place_index_t<1>, V&& val) noexcept
        : none_(), some_() {
        replace(enum_detail::in_place_index<1>, rstd::forward<V>(val));
    }

    constexpr void replace(enum_detail::in_place_index_t<0>) noexcept { some_.value = nullptr; }

    template<typename V>
    constexpr void replace(enum_detail::in_place_index_t<1>, V&& val) noexcept {
        some_.value = rstd::forward<V>(val);
    }

    [[nodiscard]]
    constexpr bool is(enum_detail::in_place_index_t<0>) const noexcept {
        return some_.value == nullptr;
    }

    [[nodiscard]]
    constexpr bool is(enum_detail::in_place_index_t<1>) const noexcept {
        return some_.value != nullptr;
    }

    [[nodiscard]]
    constexpr usize index() const noexcept {
        return is(enum_detail::in_place_index<1>) ? 1 : 0;
    }

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return true;
    }

    [[nodiscard]]
    constexpr NonePayload& get(enum_detail::in_place_index_t<0>) & noexcept {
        return none_;
    }

    [[nodiscard]]
    constexpr NonePayload const& get(enum_detail::in_place_index_t<0>) const& noexcept {
        return none_;
    }

    [[nodiscard]]
    constexpr NonePayload&& get(enum_detail::in_place_index_t<0>) && noexcept {
        return rstd::move(none_);
    }

    [[nodiscard]]
    constexpr NonePayload const&& get(enum_detail::in_place_index_t<0>) const&& noexcept {
        return rstd::move(none_);
    }

    [[nodiscard]]
    constexpr SomePayload& get(enum_detail::in_place_index_t<1>) & noexcept {
        return some_;
    }

    [[nodiscard]]
    constexpr SomePayload const& get(enum_detail::in_place_index_t<1>) const& noexcept {
        return some_;
    }

    [[nodiscard]]
    constexpr SomePayload&& get(enum_detail::in_place_index_t<1>) && noexcept {
        return rstd::move(some_);
    }

    [[nodiscard]]
    constexpr SomePayload const&& get(enum_detail::in_place_index_t<1>) const&& noexcept {
        return rstd::move(some_);
    }

private:
    [[no_unique_address]]
    NonePayload none_;
    SomePayload some_;
};

export template<typename NonePayload, typename SomePayload>
struct zero_niche_option_storage {
    constexpr zero_niche_option_storage() noexcept = default;

    constexpr explicit zero_niche_option_storage(enum_detail::in_place_index_t<0>) noexcept {}

    template<typename V>
    explicit constexpr zero_niche_option_storage(enum_detail::in_place_index_t<1>, V&& val) {
        replace(enum_detail::in_place_index<1>, rstd::forward<V>(val));
    }

    constexpr void replace(enum_detail::in_place_index_t<0>) noexcept {
        if (is(enum_detail::in_place_index<1>)) {
            rstd::destroy_at(_some_ptr());
        }
        _clear();
    }

    template<typename V>
    constexpr void replace(enum_detail::in_place_index_t<1>, V&& val) {
        if (is(enum_detail::in_place_index<1>)) {
            rstd::destroy_at(_some_ptr());
        }
        rstd::construct_at(_some_ptr(), rstd::forward<V>(val));
    }

    [[nodiscard]]
    constexpr bool is(enum_detail::in_place_index_t<0>) const noexcept {
        return ! is(enum_detail::in_place_index<1>);
    }

    [[nodiscard]]
    constexpr bool is(enum_detail::in_place_index_t<1>) const noexcept {
        for (usize i = 0; i < sizeof(m_storage); i++) {
            if (m_storage[i] != rstd::byte(0)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    constexpr usize index() const noexcept {
        return is(enum_detail::in_place_index<1>) ? 1 : 0;
    }

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return true;
    }

    [[nodiscard]]
    constexpr NonePayload& get(enum_detail::in_place_index_t<0>) & noexcept {
        return none_;
    }

    [[nodiscard]]
    constexpr NonePayload const& get(enum_detail::in_place_index_t<0>) const& noexcept {
        return none_;
    }

    [[nodiscard]]
    constexpr NonePayload&& get(enum_detail::in_place_index_t<0>) && noexcept {
        return rstd::move(none_);
    }

    [[nodiscard]]
    constexpr NonePayload const&& get(enum_detail::in_place_index_t<0>) const&& noexcept {
        return rstd::move(none_);
    }

    [[nodiscard]]
    constexpr SomePayload& get(enum_detail::in_place_index_t<1>) & noexcept {
        return *_some_ptr();
    }

    [[nodiscard]]
    constexpr SomePayload const& get(enum_detail::in_place_index_t<1>) const& noexcept {
        return *_some_ptr();
    }

    [[nodiscard]]
    constexpr SomePayload&& get(enum_detail::in_place_index_t<1>) && noexcept {
        return rstd::move(*_some_ptr());
    }

    [[nodiscard]]
    constexpr SomePayload const&& get(enum_detail::in_place_index_t<1>) const&& noexcept {
        return rstd::move(*_some_ptr());
    }

private:
    constexpr auto _some_ptr() noexcept -> SomePayload* {
        return rstd::launder(reinterpret_cast<SomePayload*>(m_storage));
    }

    constexpr auto _some_ptr() const noexcept -> SomePayload const* {
        return rstd::launder(reinterpret_cast<SomePayload const*>(m_storage));
    }

    constexpr void _clear() noexcept {
        for (usize i = 0; i < sizeof(m_storage); i++) {
            m_storage[i] = rstd::byte(0);
        }
    }

    [[no_unique_address]]
    NonePayload none_;
    alignas(SomePayload) rstd::byte m_storage[sizeof(SomePayload)] {};
};

template<typename T>
struct option_adapter;

} // namespace detail
} // namespace rstd::option

namespace rstd
{
export using option::Option;
export using option::Some;
export using option::None;

} // namespace rstd

namespace rstd::option
{

#define RSTD_OPTION_VARIANTS(V) \
    V(None, ())                 \
    V(Some, (union_value_t value;))

/// An optional value: either `Some(T)` or `None`.
/// \tparam T The type of the contained value.
export template<typename T>
class Option : public detail::option_adapter<T> {
    template<typename>
    friend struct detail::option_adapter;
    template<typename>
    friend struct detail::option_adapter_l1;

    using traits        = detail::option_traits<Option<T>>;
    using union_value_t = typename traits::union_value_t;

    RSTD_ENUM_VARIANT_TYPES(RSTD_OPTION_VARIANTS)

    using rstd_enum_storage_type = detail::option_storage<T, None_payload, Some_payload>;

    RSTD_ENUM_STORAGE(Option)

    [[nodiscard]]
    constexpr auto _ptr() const noexcept -> union_value_t const* {
        return rstd::addressof(rstd_enum_storage_.get(RSTD_ENUM_IN_PLACE(Some)).value);
    }

    [[nodiscard]]
    constexpr auto _ptr() noexcept -> union_value_t* {
        return rstd::addressof(rstd_enum_storage_.get(RSTD_ENUM_IN_PLACE(Some)).value);
    }

    template<typename V>
    constexpr void _assign_val(V&& val) {
        if constexpr (mtp::is_ref<T>) {
            rstd_enum_storage_.replace(RSTD_ENUM_IN_PLACE(Some), rstd::addressof(val));
        } else {
            rstd_enum_storage_.replace(RSTD_ENUM_IN_PLACE(Some), rstd::forward<V>(val));
        }
    }

    template<typename V>
    constexpr void _construct_val(V&& val) {
        _assign_val(rstd::forward<V>(val));
    }

    constexpr void _assign_none() { rstd_enum_storage_.replace(RSTD_ENUM_IN_PLACE(None)); }

    template<typename U>
    [[gnu::always_inline]]
    inline static constexpr auto* _ptr_wrapper(U&& self) noexcept {
        if constexpr (mtp::is_ref<T>) {
            auto* ptr = *(self._ptr());
            if constexpr (mtp::is_const<mtp::rm_ref<U>>) {
                using value_const_t = mtp::add_const<mtp::rm_ref<T>>;
                return static_cast<value_const_t*>(ptr);
            } else {
                return ptr;
            }
        } else {
            return self._ptr();
        }
    }

    template<typename U>
    [[gnu::always_inline]]
    inline static constexpr auto&& _get(U&& self) noexcept {
        auto* ptr = _ptr_wrapper(rstd::forward<U>(self));

        using value_t = mtp::rm_ref<decltype(*ptr)>;
        if constexpr (mtp::is_ref_lv<U>)
            return static_cast<mtp::add_ref<value_t>>(*ptr);
        else
            return static_cast<mtp::add_ref_rv<value_t>>(*ptr);
    }

    template<typename U>
    [[gnu::always_inline]]
    constexpr auto&& _get_move(U&& self) noexcept {
        if constexpr (mtp::is_ref_lv<T>) {
            return _get(self);
        } else {
            return _get(rstd::move(self));
        }
    }

    auto _clone_some() const {
        if constexpr (mtp::is_ref<T>) {
            return Some<T>(*as<clone::Clone>(*this->_ptr()).clone());
        } else {
            return Some(as<clone::Clone>(*this->_ptr()).clone());
        }
    }

public:
    USE_TRAIT_WIN(Option)

    using value_type = T;

    constexpr Option() noexcept: RSTD_ENUM_INIT(None) {}

    constexpr inline Option(const Option&) noexcept
        requires mtp::triv_copy<union_value_t>
    = default;
    constexpr inline Option(Option&&) noexcept
        requires mtp::triv_move<union_value_t>
    = default;

    constexpr Option(Option&& o) noexcept(mtp::noex_move<union_value_t>)
        requires mtp::user_move<union_value_t>
        : RSTD_ENUM_INIT(None) {
        if (o.is_some()) {
            this->_construct_val(_get(rstd::move(o)));
        } else {
            this->_assign_none();
        }
    }

    inline ~Option() = default;

    constexpr inline Option& operator=(const Option&)
        requires mtp::triv_assign_copy<union_value_t>
    = default;
    constexpr inline Option& operator=(Option&&)
        requires mtp::triv_assign_move<union_value_t>
    = default;

    constexpr Option& operator=(Option&& v) noexcept(mtp::noex_move<union_value_t>)
        requires mtp::user_move<union_value_t>
    {
        if (this == rstd::addressof(v)) {
            return *this;
        }
        if (v.is_some()) {
            this->_assign_val(_get(rstd::move(v)));
        } else {
            this->_assign_none();
        }
        return *this;
    }

    /// Returns `true` if the option contains a value.
    [[nodiscard]]
    constexpr auto is_some() const noexcept -> bool {
        return rstd_enum_storage_.is(RSTD_ENUM_IN_PLACE(Some));
    }

    /// Returns `true` if the option is `None`.
    constexpr auto is_none() const noexcept -> bool { return ! this->is_some(); }

    /// Returns `true` if the option contains a value and the predicate returns `true`.
    /// \param f A predicate applied to the contained value.
    /// \return `true` if `Some` and `f` returns `true`, otherwise `false`.
    template<typename F>
    // requires ImpledT<FnOnce<F, bool(T)>>
    auto is_some_and(F&& f) -> bool {
        if (this->is_some()) {
            return rstd::forward<F>(f)(_get_move(*this));
        }
        return false;
    }

    /// Converts from `Option<T>` to `Option<const T&>`.
    /// \return An `Option` containing a const reference to the inner value, or `None`.
    constexpr auto as_ref() const -> Option<mtp::add_ref<mtp::add_const<T>>> {
        if (this->is_some()) {
            return option::Some(_get(*this));
        } else {
            return option::None();
        }
    }

    /// Returns the contained value, consuming the option. Panics with the given message if `None`.
    /// \param msg The panic message to display if the value is `None`.
    /// \return The contained value.
    auto expect(ref<str> msg) -> T {
        if (this->is_some()) {
            return _get_move(*this);
        }
        rstd::panic("{}", msg);
        rstd::unreachable();
    }

    /// Returns the contained value, consuming the option. Panics if the value is `None`.
    /// \return The contained value.
    auto unwrap() -> T {
        if (this->is_some()) {
            return _get_move(*this);
        }
        rstd::panic("called `Option::unwrap()` on a `None` value");
        rstd::unreachable();
    }

    /// Returns the contained value or the provided default.
    /// \param default_value The value to return if `None`.
    /// \return The contained value or `default_value`.
    template<typename U>
    auto unwrap_or(U&& default_value) -> T {
        if (this->is_some()) {
            return _get_move(*this);
        }
        return rstd::forward<U>(default_value);
    }

    /// Returns the contained value or computes it from the provided closure.
    /// \param f A closure that produces a default value.
    /// \return The contained value or the result of `f()`.
    template<typename F>
    // requires ImpledT<FnOnce<F, T()>>
    auto unwrap_or_else(F&& f) -> T {
        if (this->is_some()) {
            return _get_move(*this);
        }
        return rstd::forward<F>(f)();
    }

    /// Returns the contained value without checking. Undefined behavior if `None`.
    /// \return The contained value.
    constexpr auto unwrap_unchecked() -> T {
        if (this->is_some()) {
            return _get_move(*this);
        } else {
            rstd::unreachable();
        }
    }

    /// Maps an `Option<T>` to `Option<U>` by applying a function to the contained value.
    /// \param f A function that transforms the contained value.
    /// \return `Some(f(value))` if `Some`, otherwise `None`.
    template<typename F, typename U = mtp::invoke_result_t<F, T>>
    // requires ImpledT<FnOnce<F, U(T)>>
    constexpr auto map(F&& f) -> Option<U> {
        if (this->is_some()) {
            return option::Some(rstd::forward<F>(f)(_get_move(*this)));
        }
        return option::None<U>();
    }

    /// Returns `None` if the option is `None`, otherwise calls `f` with the contained value and
    /// returns the result.
    /// \param f A function that takes the contained value and returns an `Option`.
    /// \return The result of `f(value)` if `Some`, otherwise `None`.
    template<typename F, typename U = mtp::invoke_result_t<F, T>>
        requires mtp::spec_of<U, Option>
    constexpr auto and_then(F&& f) -> U {
        if (this->is_some()) {
            return rstd::forward<F>(f)(_get_move(*this));
        }
        return option::None();
    }

    /// Takes the value out of the option, leaving `None` in its place.
    /// \return The previously contained `Option`.
    constexpr auto take() -> Option<T> { return rstd::exchange(*this, option::None()); }

    /// Inserts a value into the option, replacing any previous value.
    template<typename U>
        requires mtp::init<T, U>
    constexpr auto insert(U&& value) -> T& {
        this->_assign_val(rstd::forward<U>(value));
        return **this;
    }

    /// Dereferences the contained value. Asserts that the option is `Some`.
    [[nodiscard]]
    constexpr auto operator*() const noexcept -> mtp::add_ref<mtp::add_const<mtp::rm_ref<T>>> {
        rstd_assert(this->is_some());
        return _get(*this);
    }

    /// Dereferences the contained value. Asserts that the option is `Some`.
    [[nodiscard]]
    constexpr auto operator*() noexcept -> mtp::add_ref<mtp::rm_ref<T>> {
        rstd_assert(this->is_some());
        return _get(*this);
    }

    /// Accesses the contained value via pointer. Asserts that the option is `Some`.
    [[nodiscard]]
    constexpr auto* operator->() const noexcept {
        rstd_assert(this->is_some());
        return _ptr_wrapper(*this);
    }

    /// Accesses the contained value via pointer. Asserts that the option is `Some`.
    [[nodiscard]]
    constexpr auto* operator->() noexcept {
        rstd_assert(this->is_some());
        return _ptr_wrapper(*this);
    }

    /// Returns `true` if the option is `Some`.
    [[nodiscard]]
    constexpr explicit operator bool() const noexcept {
        return this->is_some();
    }

    /// Creates a deep copy of this option and its contained value.
    /// \return A new `Option` with a cloned value, or `None`.
    auto clone() const -> Option
        requires Impled<union_value_t, clone::Clone>
    {
        if (this->is_some()) {
            return _clone_some();
        }
        return option::Unknown();
    }

    /// Overwrites this option with a clone of the source.
    /// \param source The option to clone from.
    void clone_from(Option& source)
        requires Impled<union_value_t, clone::Clone>
    {
        if (source.is_some()) {
            if constexpr (mtp::is_ref<T>) {
                this->_assign_val(_get(source));
            } else {
                this->_assign_val(as<clone::Clone>(*source._ptr()).clone());
            }
        } else {
            this->_assign_none();
        }
    }

    template<typename U>
    static constexpr auto make_with(U&& val) noexcept(mtp::noex_init<T, U>) {
        return Option(rstd::forward<U>(val));
    }

private:
    template<typename U>
        requires mtp::init<T, U>
    explicit constexpr Option(U&& val) noexcept(mtp::noex_init<T, U>): RSTD_ENUM_INIT(None) {
        this->_construct_val(rstd::forward<U>(val));
    }

    explicit constexpr Option(mtp::rm_ref<T>* ptr) noexcept
        requires mtp::is_ref<T>
        : RSTD_ENUM_INIT(None) {
        this->_construct_val(*ptr);
    }
};

#undef RSTD_OPTION_VARIANTS

} // namespace rstd::option

namespace rstd::option
{

namespace detail
{

template<typename T>
struct option_adapter : option_adapter_l1<T> {
    /// Transforms the `Option<T>` into a `Result<T, E>`, mapping `Some(v)` to `Ok(v)` and `None` to
    /// `Err(err)`.
    /// \param err The error value to use if the option is `None`.
    /// \return `Ok(value)` if `Some`, otherwise `Err(err)`.
    template<typename E>
    auto ok_or(E err) -> result::Result<T, E> {
        auto& self = static_cast<Option<T>&>(*this);
        if (self.is_some()) {
            return result::Ok(self._get_move(self));
        } else {
            return result::Err(err);
        }
    }

    /// Transforms the `Option<T>` into a `Result<T, E>`, mapping `Some(v)` to `Ok(v)` and `None` to
    /// `Err(err())`.
    /// \param err A closure that produces the error value if the option is `None`.
    /// \return `Ok(value)` if `Some`, otherwise `Err(err())`.
    template<typename F, typename E = mtp::invoke_result_t<F>>
    auto ok_or_else(F&& err) -> result::Result<T, E> {
        auto& self = static_cast<Option<T>&>(*this);
        if (self.is_some()) {
            return result::Ok(self._get_move(self));
        } else {
            return result::Err(rstd::move(err)());
        }
    }
};

template<typename T>
struct option_adapter_l1<Option<T>> {
    /// Converts from `Option<Option<T>>` to `Option<T>`, flattening one level of nesting.
    /// \return The inner `Option<T>` if `Some`, otherwise `None`.
    constexpr auto flatten() -> Option<T> {
        auto&& self = static_cast<Option<Option<T>>&&>(*this);
        if (self.is_some()) {
            return self._get_move(self);
        } else {
            return None();
        }
    }
};

} // namespace detail

} // namespace rstd::option

namespace rstd
{

template<typename U, typename A>
    requires mtp::equalable<U, A>
struct Impl<cmp::PartialEq<option::Option<U>>, Option<A>>
    : DefaultInImpl<cmp::PartialEq<option::Option<U>>, Option<A>> {
    auto eq(const Option<U>& other) const noexcept -> bool {
        auto& self = this->self();
        if (self.is_some())
            return other.is_some() && (*self == *other);
        else
            return ! other.is_some();
    }
};

} // namespace rstd

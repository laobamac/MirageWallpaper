module;
#include <rstd/macro.hpp>
#include <rstd/enum.hpp>
export module rstd.core:result;
import :enum_;
export import :clone;
export import :fmt;
export import :ops.function;
export import :option;

namespace rstd::result
{

struct UnknownOk {
    constexpr bool operator==(const UnknownOk&) const noexcept { return true; }
};
struct UnknownErr {
    constexpr bool operator==(const UnknownErr&) const noexcept { return true; }
};

namespace detail
{

template<typename T>
struct result_traits {};

template<typename T, typename E>
struct result_traits<Result<T, E>> {
    using value_type = T;
    using error_type = E;

    using ret_value_t = T;
    using ret_error_t = E;

    using union_value_t = mtp::cond<mtp::is_ref<T>, mtp::add_ptr<mtp::rm_ref<T>>, mtp::rm_cv<T>>;
    using union_error_t = mtp::cond<mtp::is_ref<E>, mtp::add_ptr<mtp::rm_ref<E>>, E>;
};

template<typename T, typename E>
struct result_traits<Result<T, E>&&> : result_traits<Result<T, E>> {
    using ret_value_t = T&&;
    using ret_error_t = E&&;
};

template<typename T, typename E>
struct result_traits<Result<T, E>&> : result_traits<Result<T, E>> {
    using ret_value_t = T&;
    using ret_error_t = E&;
};

template<typename T, typename E>
struct result_traits<const Result<T, E>&> : result_traits<Result<T, E>> {
    using ret_value_t = mtp::add_const<T>&;
    using ret_error_t = mtp::add_const<E>&;
};

template<typename T, typename E>
struct result_traits<const Result<T, E>&&> : result_traits<Result<T, E>> {
    using ret_value_t = mtp::add_const<T>&&;
    using ret_error_t = mtp::add_const<E>&&;
};

} // namespace detail
} // namespace rstd::result

namespace rstd
{
export using result::Result;
export using result::Ok;
export using result::Err;

} // namespace rstd

namespace rstd::result
{

template<typename E>
auto unwrap_failed(ref<str> msg, E& e) {
    rstd::panic { msg };
}

namespace detail
{

template<typename T, typename E>
struct result_base {
    template<typename, typename>
    friend class result::Result;

protected:
    constexpr auto _cast() -> Result<T, E>& { return static_cast<Result<T, E>&>(*this); }
    constexpr auto _cast() const -> const Result<T, E>& {
        return static_cast<const Result<T, E>&>(*this);
    }

    template<i32 I, typename U>
        requires mtp::same_as<mtp::rm_cvf<U>, Result<T, E>>
    static constexpr decltype(auto) _get(U&& self) {
        using traits = detail::result_traits<decltype(self)>;
        if constexpr (I == 0) {
            if constexpr (mtp::is_ref<T>) {
                return static_cast<traits::ret_value_t>(*(*self._value_ptr()));
            } else {
                return static_cast<traits::ret_value_t>(*self._value_ptr());
            }
        } else {
            if constexpr (mtp::is_ref<E>) {
                return static_cast<traits::ret_error_t>(*(*self._error_ptr()));
            } else {
                return static_cast<traits::ret_error_t>(*self._error_ptr());
            }
        }
    }

    template<i32 I>
    constexpr decltype(auto) _get() & {
        return result_base::template _get<I>(static_cast<Result<T, E>&>(*this));
    }

    template<i32 I>
    constexpr decltype(auto) _get() && {
        return result_base::template _get<I>(static_cast<Result<T, E>&&>(*this));
    }

    template<i32 I>
    constexpr decltype(auto) _get() const& {
        return result_base::template _get<I>(static_cast<const Result<T, E>&>(*this));
    }
    template<i32 I>
    constexpr decltype(auto) _get() const&& {
        return result_base::template _get<I>(static_cast<const Result<T, E>&&>(*this));
    }

    template<i32 I>
    constexpr decltype(auto) _get_move() {
        return result_base::template _get<I>(static_cast<Result<T, E>&&>(*this));
    }

    template<i32 I, typename U>
    constexpr decltype(auto) _self_get(U&& self) const {
        return result_base::template _get<I>(rstd::forward<U>(self));
    }

    template<typename V>
    constexpr void _construct_val(V&& val) {
        _cast()._replace_ok(rstd::forward<V>(val));
    }
    template<typename V>
    constexpr void _construct_err(V&& err) {
        _cast()._replace_err(rstd::forward<V>(err));
    }

    template<typename V>
    constexpr void _assign_val(V&& v) {
        _cast()._replace_ok(rstd::forward<V>(v));
    }

    template<typename V>
    constexpr void _assign_err(V&& v) {
        _cast()._replace_err(rstd::forward<V>(v));
    }

public:
    /// Returns `true` if the result is `Ok`.
    [[gnu::always_inline]]
    inline constexpr auto is_ok() const noexcept -> bool {
        return _cast()._is_ok();
    }
    /// Returns `true` if the result is `Err`.
    [[gnu::always_inline]]
    inline constexpr auto is_err() const noexcept -> bool {
        return ! is_ok();
    }

    /// Returns `true` if the result is `Ok` and the predicate returns `true` for the contained value.
    /// \param f A predicate applied to the `Ok` value.
    /// \return `true` if `Ok` and `f` returns `true`, otherwise `false`.
    template<typename F>
    // requires ImpledT<FnOnce<F, bool(T)>>
    auto is_ok_and(F&& f) -> bool {
        if (is_ok()) {
            return rstd::move(f)(_get_move<0>());
        } else
            return false;
    }

    /// Returns `true` if the result is `Err` and the predicate returns `true` for the error value.
    /// \param f A predicate applied to the `Err` value.
    /// \return `true` if `Err` and `f` returns `true`, otherwise `false`.
    template<typename F>
    // requires ImpledT<FnOnce<F, bool(E)>>
    auto is_err_and(F&& f) -> bool {
        if (is_err()) {
            return rstd::move(f)(_get_move<1>());
        } else
            return false;
    }

    /// Converts from `Result<T, E>` to `Option<T>`, discarding the error if any.
    /// \return `Some(value)` if `Ok`, otherwise `None`.
    auto ok() -> Option<T> {
        if (is_ok()) {
            return _get_move<0>();
        } else
            return rstd::None();
    }

    /// Converts from `Result<T, E>` to `Option<E>`, discarding the success value if any.
    /// \return `Some(error)` if `Err`, otherwise `None`.
    auto err() -> Option<E> {
        if (is_err()) {
            return _get_move<1>();
        } else
            return rstd::None();
    }

    /// Maps a `Result<T, E>` to `Result<U, E>` by applying a function to the `Ok` value.
    /// \param op A function that transforms the `Ok` value.
    /// \return `Ok(op(value))` if `Ok`, otherwise the original `Err`.
    template<typename F, typename U = mtp::invoke_result_t<F, T>>
    // requires ImpledT<FnOnce<F, U(T)>>
    auto map(F&& op) -> Result<U, E> {
        if (is_ok()) {
            return Ok(rstd::move(op)(_get_move<0>()));
        } else {
            return Err(_get<1>());
        }
    }

    /// Returns the provided default if `Err`, or applies a function to the `Ok` value.
    /// \param def The default value to return if `Err`.
    /// \param op A function that transforms the `Ok` value.
    /// \return `op(value)` if `Ok`, otherwise `def`.
    template<typename U, typename F>
    // requires ImpledT<FnOnce<F, U(T)>>
    auto map_or(U&& def, F&& op) -> U {
        if (is_ok()) {
            return rstd::move(op)(_get_move<0>());
        } else {
            return def;
        }
    }

    /// Maps a `Result<T, E>` to `U` by applying `f` to the `Ok` value or `def` to the `Err` value.
    /// \param def A closure applied to the `Err` value.
    /// \param f A closure applied to the `Ok` value.
    /// \return The result of `f` or `def`.
    template<typename U, typename D, typename F>
    // requires ImpledT<FnOnce<D, U(E)>, FnOnce<F, U(T)>>
    auto map_or_else(D&& def, F&& f) -> U {
        if (is_ok()) {
            return rstd::move(f)(_get_move<0>());
        } else {
            return rstd::move(def)(_get_move<1>());
        }
    }
    /// Maps a `Result<T, E>` to `Result<T, F>` by applying a function to the `Err` value.
    /// \param op A function that transforms the `Err` value.
    /// \return The original `Ok` value, or `Err(op(error))`.
    template<typename O, typename F = mtp::invoke_result_t<O, E>>
    // requires ImpledT<FnOnce<O, F(E)>>
    auto map_err(O&& op) -> Result<T, F> {
        if (is_ok()) {
            return Ok(_get_move<0>());
        } else {
            return Err(rstd::move(op)(_get_move<1>()));
        }
    }

    /// Calls the provided closure with a reference to the `Ok` value, then returns the result unchanged.
    /// \param f A closure to call with the `Ok` value.
    /// \return The original `Result`.
    template<typename F>
    // requires ImpledT<FnOnce<F, void(T&)>>
    auto inspect(F&& f) -> Result<T, E> {
        if (is_ok()) {
            rstd::move(f)(_get<0>());
        }
        auto& self = _cast();
        return rstd::move(self);
    }

    /// Calls the provided closure with a reference to the `Err` value, then returns the result unchanged.
    /// \param f A closure to call with the `Err` value.
    /// \return The original `Result`.
    template<typename F>
    // requires ImpledT<FnOnce<F, void(E&)>>
    auto inspect_err(F&& f) -> Result<T, E> {
        if (is_err()) {
            rstd::move(f)(_get<1>());
        }
        auto& self = _cast();
        return rstd::move(self);
    }

    /// Returns the contained `Ok` value. Panics with the given message if `Err`.
    /// \param msg The panic message to display on failure.
    /// \return The contained `Ok` value.
    auto expect(ref<str> msg) -> T {
        if (is_ok()) {
            return _get_move<0>();
        } else {
            unwrap_failed(msg, _get<1>());
            rstd::unreachable();
        }
    }

    /// Returns the contained `Ok` value. Panics if the result is `Err`.
    /// \return The contained `Ok` value.
    auto unwrap() -> T {
        if (is_ok()) {
            return _get_move<0>();
        } else {
            unwrap_failed("called `Result::unwrap()` on an `Err` value", _get<1>());
            rstd::unreachable();
        }
    }

    /// Returns the contained `Ok` value or a default-constructed `T`.
    /// \return The contained value if `Ok`, otherwise `T{}`.
    auto unwrap_or_default() -> T
        requires mtp::init<T>
    {
        if (is_ok()) {
            return _get_move<0>();
        } else {
            return {};
        }
    }

    /// Returns the contained `Err` value. Panics with the given message if `Ok`.
    /// \param msg The panic message to display on failure.
    /// \return The contained `Err` value.
    auto expect_err(ref<str> msg) -> E {
        if (is_ok()) {
            unwrap_failed(msg, _get<0>());
            rstd::unreachable();
        } else {
            return _get_move<1>();
        }
    }

    /// Returns the contained `Err` value. Panics if the result is `Ok`.
    /// \return The contained `Err` value.
    auto unwrap_err() -> E {
        if (is_err()) {
            return _get_move<1>();
        } else {
            unwrap_failed("called `Result::unwrap_err()` on an `Ok` value", _get<1>());
            rstd::unreachable();
        }
    }

    /// Calls `op` with the `Ok` value and returns the result. Returns the `Err` value unchanged if `Err`.
    /// \param op A function that takes the `Ok` value and returns a `Result`.
    /// \return The result of `op(value)` if `Ok`, otherwise the original `Err`.
    template<typename U = void,
             typename F,
             typename U2 = typename mtp::invoke_result_t<F, T>::value_type>
    // requires ImpledT<FnOnce<F, Result<mtp::cond<mtp::is_void<U>, U2, U>, E>(T)>>
    auto and_then(F&& op) -> Result<mtp::cond<mtp::is_void<U>, U2, U>, E> {
        if (is_ok()) {
            return rstd::move(op)(_get_move<0>());
        } else {
            return Err(_get_move<1>());
        }
    }

    /// Returns the `Ok` value if `Ok`, otherwise returns `res`.
    /// \param res The alternative result to return if `Err`.
    /// \return The original `Ok` or the provided alternative.
    template<typename F>
    auto or_(Result<T, F>&& res) -> Result<T, F> {
        if (is_ok()) {
            return Ok(_get_move<0>());
        } else {
            auto& self = _cast();
            return rstd::move(res);
        }
    }

    /// Calls `op` with the `Err` value if `Err`, otherwise returns the `Ok` value.
    /// \param op A function that takes the `Err` value and returns a `Result`.
    /// \return The original `Ok`, or the result of `op(error)`.
    template<typename F, typename O>
    // requires ImpledT<FnOnce<O, Result<T, F>(E)>>
    auto or_else(O&& op) -> Result<T, F> {
        if (is_ok()) {
            return Ok(_get_move<0>());
        } else {
            return rstd::move(op)(_get_move<1>());
        }
    }

    /// Returns the contained `Ok` value or the provided default.
    /// \param def The default value to return if `Err`.
    /// \return The contained value or `def`.
    auto unwrap_or(T&& def) -> T {
        if (is_ok()) {
            return _get_move<0>();
        } else {
            return rstd::move(def);
        }
    }

    /// Returns the contained `Ok` value or computes it from the `Err` value using the provided closure.
    /// \param op A closure that takes the `Err` value and returns a `T`.
    /// \return The contained value or the result of `op(error)`.
    template<typename F>
    // requires ImpledT<FnOnce<F, T(E)>>
    auto unwrap_or_else(F&& op) -> T {
        if (is_ok()) {
            return _get_move<0>();
        } else {
            return rstd::move(op)(_get_move<1>());
        }
    }

    /// Returns the contained `Ok` value without checking. Undefined behavior if `Err`.
    /// \return The contained `Ok` value.
    auto unwrap_unchecked() -> T {
        if (is_ok()) {
            return _get_move<0>();
        } else {
            rstd::unreachable();
        }
    }

    /// Returns the contained `Err` value without checking. Undefined behavior if `Ok`.
    /// \return The contained `Err` value.
    auto unwrap_err_unchecked() -> E {
        if (is_err()) {
            return _get_move<1>();
        } else {
            rstd::unreachable();
        }
    }

    /// Converts from `Result<T, E>` to `Result<const T&, const E&>`.
    /// \return A `Result` containing const references to the inner values.
    constexpr auto as_ref() const
        -> Result<mtp::add_ref<mtp::add_const<T>>, mtp::add_ref<mtp::add_const<E>>> {
        if (is_ok()) {
            return Ok(_get<0>());
        } else {
            return Err(_get<1>());
        }
    }

    /// Accesses the contained `Ok` value via pointer. Asserts that the result is `Ok`.
    [[nodiscard]]
    constexpr const mtp::rm_ref<T>* operator->() const noexcept {
        rstd_assert(is_ok());
        return rstd::addressof(_get<0>());
    }

    /// Accesses the contained `Ok` value via pointer. Asserts that the result is `Ok`.
    [[nodiscard]]
    constexpr mtp::rm_ref<T>* operator->() noexcept {
        rstd_assert(is_ok());
        return rstd::addressof(_get<0>());
    }

    /// Dereferences the contained `Ok` value. Asserts that the result is `Ok`.
    [[nodiscard]]
    constexpr const T& operator*() const& noexcept {
        rstd_assert(is_ok());
        return _get<0>();
    }

    /// Dereferences the contained `Ok` value. Asserts that the result is `Ok`.
    [[nodiscard]]
    constexpr T& operator*() & noexcept {
        rstd_assert(is_ok());
        return _get<0>();
    }

    /// Dereferences the contained `Ok` value. Asserts that the result is `Ok`.
    [[nodiscard]]
    constexpr const T&& operator*() const&& noexcept {
        rstd_assert(is_ok());
        return _get<0>();
    }

    /// Dereferences the contained `Ok` value. Asserts that the result is `Ok`.
    [[nodiscard]]
    constexpr T&& operator*() && noexcept {
        rstd_assert(is_ok());
        return _get<0>();
    }

    /// Returns `true` if the result is `Ok`.
    [[nodiscard]]
    constexpr explicit operator bool() const noexcept {
        return is_ok();
    }
};

template<typename T, typename E>
struct result_impl : result_base<T, E> {};

template<typename T, typename E>
struct result_impl<const T&, E> : result_base<const T&, E> {
    /// Maps a `Result<const T&, E>` to `Result<T, E>` by copying the contained value.
    /// \return A new `Result` with a copied `Ok` value.
    constexpr auto copied() -> Result<T, E> {
        if (this->is_ok()) {
            return Ok(T(this->template _self_get<0>()));
        } else {
            return Err(E(this->template _self_get<1>()));
        }
    }

    /// Maps a `Result<const T&, E>` to `Result<T, E>` by cloning the contained value.
    /// \return A new `Result` with a cloned `Ok` value.
    auto cloned() -> Result<T, E>
        requires Impled<T, clone::Clone>
    {
        return this->map([](auto t) {
            return as<clone::Clone>(t).clone();
        });
    }
};

template<typename T, typename E>
struct result_impl<T&, E> : result_base<T&, E> {
    /// Maps a `Result<T&, E>` to `Result<T, E>` by copying the contained value.
    /// \return A new `Result` with a copied `Ok` value.
    constexpr auto copied() -> Result<T, E> {
        if (this->is_ok()) {
            return Ok(T(this->template _self_get<0>()));
        } else {
            return Err(E(this->template _self_get<1>()));
        }
    }

    /// Maps a `Result<T&, E>` to `Result<T, E>` by cloning the contained value.
    /// \return A new `Result` with a cloned `Ok` value.
    auto cloned() -> Result<T, E>
        requires Impled<T, clone::Clone>
    {
        return this->map([](auto t) {
            return as<clone::Clone>(t).clone();
        });
    }
};

template<typename T, typename E>
struct result_impl<Result<T, E>, E> : result_base<Result<T, E>, E> {
    /// Converts from `Result<Result<T, E>, E>` to `Result<T, E>`, flattening one level of nesting.
    /// \return The inner `Result` if `Ok`, otherwise the outer `Err`.
    constexpr auto flatten() -> Result<T, E> {
        if (this->is_ok()) {
            return this->template _self_get<0>();
        } else {
            return Err(this->template _self_get<1>());
        }
    }
};

struct ok_tag {};
struct err_tag {};

} // namespace detail

#define RSTD_RESULT_VARIANTS(V)   \
    V(Ok, (union_value_t value;)) \
    V(Err, (union_error_t error;))

/// A type that represents either success (`Ok`) or failure (`Err`).
/// \tparam T The type of the success value.
/// \tparam E The type of the error value.
export template<typename T, typename E>
class Result : public detail::result_impl<T, E> {
    template<typename, typename>
    friend struct detail::result_base;
#if !defined(RSTD_OS_WINDOWS)
    template<typename, typename>
    friend struct rstd::Impl;
#endif

    using traits        = detail::result_traits<Result<T, E>>;
    using union_value_t = typename traits::union_value_t;
    using union_error_t = typename traits::union_error_t;

    RSTD_ENUM_VARIANT_TYPES(RSTD_RESULT_VARIANTS)
    RSTD_ENUM_DEFAULT_STORAGE(RSTD_RESULT_VARIANTS)
    RSTD_ENUM_STORAGE(Result)

    [[nodiscard]]
    constexpr auto _value_ptr() noexcept -> union_value_t* {
        return rstd::addressof(rstd_enum_storage_.get(RSTD_ENUM_IN_PLACE(Ok)).value);
    }

    [[nodiscard]]
    constexpr auto _value_ptr() const noexcept -> union_value_t const* {
        return rstd::addressof(rstd_enum_storage_.get(RSTD_ENUM_IN_PLACE(Ok)).value);
    }

    [[nodiscard]]
    constexpr auto _error_ptr() noexcept -> union_error_t* {
        return rstd::addressof(rstd_enum_storage_.get(RSTD_ENUM_IN_PLACE(Err)).error);
    }

    [[nodiscard]]
    constexpr auto _error_ptr() const noexcept -> union_error_t const* {
        return rstd::addressof(rstd_enum_storage_.get(RSTD_ENUM_IN_PLACE(Err)).error);
    }

    template<typename V>
    constexpr void _replace_ok(V&& val) {
        if constexpr (mtp::is_ref<T>) {
            rstd_enum_storage_.replace(RSTD_ENUM_IN_PLACE(Ok), rstd::addressof(val));
        } else {
            rstd_enum_storage_.replace(RSTD_ENUM_IN_PLACE(Ok), rstd::forward<V>(val));
        }
    }

    template<typename V>
    constexpr void _replace_err(V&& err) {
        if constexpr (mtp::is_ref<E>) {
            rstd_enum_storage_.replace(RSTD_ENUM_IN_PLACE(Err), rstd::addressof(err));
        } else {
            rstd_enum_storage_.replace(RSTD_ENUM_IN_PLACE(Err), rstd::forward<V>(err));
        }
    }

    [[nodiscard]]
    constexpr auto _is_ok() const noexcept -> bool {
        return rstd_enum_storage_.is(RSTD_ENUM_IN_PLACE(Ok));
    }

public:
    using value_type = T;
    using error_type = E;

    template<typename U>
    using rebind = Result<U, error_type>;

    [[gnu::always_inline]]
    inline constexpr auto is_ok() const noexcept -> bool {
        return _is_ok();
    }
    [[gnu::always_inline]]
    inline constexpr auto is_err() const noexcept -> bool {
        return ! is_ok();
    }

    constexpr Result() noexcept(mtp::noex_init<T>)
        requires mtp::init<T>
        : RSTD_ENUM_INIT(Ok) {}

    // Ok ctor
    constexpr Result(T&& val, detail::ok_tag) noexcept(mtp::noex_init<T, T>) {
        this->_construct_val(rstd::forward<T>(val));
    }

    // Err ctor
    constexpr Result(E&& err, detail::err_tag) noexcept(mtp::noex_init<E, E>) {
        this->_construct_err(rstd::forward<E>(err));
    }

    // from Ok
    template<typename U>
    constexpr Result(U&& o) noexcept(
        mtp::noex_init<T, typename detail::result_traits<U>::value_type>)
        requires mtp::init<UnknownErr, typename detail::result_traits<U>::error_type> &&
                 mtp::init<T, typename detail::result_traits<U>::value_type>
    {
        this->_construct_val(mtp::rm_cvf<U>::template _get<0>(rstd::forward<U>(o)));
    }

    // from Err
    template<typename U>
    constexpr Result(U&& o) noexcept(
        mtp::noex_init<E, typename detail::result_traits<U>::error_type>)
        requires mtp::init<UnknownOk, typename detail::result_traits<U>::value_type> &&
                 mtp::init<E, typename detail::result_traits<U>::error_type>
    {
        this->_construct_err(mtp::rm_cvf<U>::template _get<1>(rstd::forward<U>(o)));
    }

    constexpr inline Result(const Result&) = default;

    constexpr inline Result(Result&& o) noexcept(mtp::noex_move<union_value_t> &&
                                                 mtp::noex_move<union_error_t>)
        requires mtp::move<union_value_t> && mtp::move<union_error_t>
        : rstd_enum_storage_() {
        if (o.is_ok()) {
            this->_construct_val(Result::template _get<0>(rstd::move(o)));
        } else {
            this->_construct_err(Result::template _get<1>(rstd::move(o)));
        }
    }

    constexpr inline ~Result()
        requires(mtp::triv_drop<union_value_t> && mtp::triv_drop<union_error_t>)
    = default;

    constexpr inline ~Result()
        requires(! (mtp::triv_drop<union_value_t> && mtp::triv_drop<union_error_t>))
    {}

    Result& operator=(const Result&) = delete;

    constexpr inline Result& operator=(Result&& o) noexcept(mtp::noex_move<union_value_t> &&
                                                            mtp::noex_move<union_error_t>)
        requires mtp::triv_assign_move<typename traits::union_value_t> &&
                 mtp::triv_assign_move<typename traits::union_error_t>
    {
        if (this == rstd::addressof(o)) {
            return *this;
        }
        if (o.is_ok())
            this->_assign_val(Result::template _get<0>(rstd::move(o)));
        else
            this->_assign_err(Result::template _get<1>(rstd::move(o)));
        return *this;
    }

    constexpr Result&
    operator=(Result&& o) noexcept(mtp::noex_move<typename traits::union_value_t> &&
                                   mtp::noex_move<typename traits::union_value_t> &&
                                   mtp::noex_assign_move<typename traits::union_error_t> &&
                                   mtp::noex_assign_move<typename traits::union_error_t>)
        requires(! (mtp::triv_assign_move<typename traits::union_value_t> &&
                    mtp::triv_assign_move<typename traits::union_error_t>)) &&
                (mtp::assign_move<typename traits::union_value_t> &&
                 mtp::move<typename traits::union_value_t> &&
                 mtp::assign_move<typename traits::union_error_t> &&
                 mtp::move<typename traits::union_error_t>)
    {
        if (this == rstd::addressof(o)) {
            return *this;
        }
        if (o.is_ok())
            this->_assign_val(Result::template _get<0>(rstd::move(o)));
        else
            this->_assign_err(Result::template _get<1>(rstd::move(o)));
        return *this;
    }

    /// Creates a deep copy of this result and its contained value.
    /// \return A new `Result` with a cloned value or error.
    auto clone() const -> Result
        requires Impled<union_value_t, clone::Clone> && Impled<union_error_t, clone::Clone>
    {
        if (this->is_ok()) {
            if constexpr (mtp::is_ref<T>) {
                return Ok(*as<clone::Clone>(*this->_value_ptr()).clone());
            } else {
                return Ok(as<clone::Clone>(*this->_value_ptr()).clone());
            }
        } else {
            if constexpr (mtp::is_ref<E>) {
                return Err(*as<clone::Clone>(*this->_error_ptr()).clone());
            } else {
                return Err(as<clone::Clone>(*this->_error_ptr()).clone());
            }
        }
    }

    /// Overwrites this result with a clone of the source.
    /// \param source The result to clone from.
    void clone_from(Result& source)
        requires Impled<union_value_t, clone::Clone> && Impled<union_error_t, clone::Clone>
    {
        if (source.is_ok()) {
            if constexpr (mtp::is_ref<T>) {
                this->_assign_val(source.template _get<0>());
            } else {
                this->_assign_val(as<clone::Clone>(*source._value_ptr()).clone());
            }
        } else {
            if constexpr (mtp::is_ref<E>) {
                this->_assign_err(source.template _get<1>());
            } else {
                this->_assign_err(as<clone::Clone>(*source._error_ptr()).clone());
            }
        }
    }

    template<typename U, typename E2>
        requires(! mtp::is_void<U>) && requires(const T& t, const U& u, const E& e, const E2& e2) {
            { t == u } -> mtp::convertible_to<bool>;
            { e == e2 } -> mtp::convertible_to<bool>;
        }
    friend constexpr bool operator==(const Result& x, const Result<U, E2>& y) noexcept(
        noexcept(bool(x.template _get<0>() == y.template _get<0>())) &&
        noexcept(bool(x.template _get<1>() == y.template _get<1>()))) {
        if (x.is_ok())
            return y.is_ok() && bool(x.template _get<0>() == y.template _get<0>());
        else
            return ! y.is_ok() && bool(x.template _get<1>() == y.template _get<1>());
    }
};

#undef RSTD_RESULT_VARIANTS

/// Creates a `Result` in the `Ok` state containing the given value.
/// \tparam T The success value type.
/// \tparam TErr The error type (can be deduced later).
/// \param val The success value.
/// \return A `Result<T, TErr>` in the `Ok` state.
template<typename T, typename TErr>
constexpr auto Ok(T&& val) -> Result<T, TErr> {
    return { rstd::forward<T>(val), detail::ok_tag {} };
}

/// Creates a `Result` in the `Err` state containing the given error.
/// \tparam TErr The error type.
/// \tparam T The success type (can be deduced later).
/// \param val The error value.
/// \return A `Result<T, TErr>` in the `Err` state.
template<typename TErr, typename T>
constexpr auto Err(TErr&& val) -> Result<T, TErr> {
    return { rstd::forward<TErr>(val), detail::err_tag {} };
}
} // namespace rstd::result

namespace rstd::option::detail
{
template<typename T, typename E>
struct option_adapter_l1<result::Result<T, E>> {
    /// Transposes an `Option<Result<T, E>>` into a `Result<Option<T>, E>`.
    /// \return `Ok(Some(v))` if `Some(Ok(v))`, `Err(e)` if `Some(Err(e))`, or `Ok(None)` if `None`.
    constexpr auto transpose() -> result::Result<Option<T>, E> {
        auto&& self = static_cast<Option<result::Result<T, E>>&&>(*this);
        if (self.is_some()) {
            auto&& t = self.unwrap_unchecked();
            if (t.is_ok()) {
                return Ok(Some(rstd::move(t.unwrap_unchecked())));
            } else {
                return Err(rstd::move(t.unwrap_err_unchecked()));
            }
        } else {
            return Ok<Option<T>>(None());
        }
    }
};

} // namespace rstd::option::detail

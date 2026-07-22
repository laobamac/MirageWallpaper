#pragma once

#if defined(_WIN32)
#define RSTD_OS_WINDOWS 1
#endif

#if defined(__linux__)
#define RSTD_OS_LINUX 1
#endif

#if defined(__APPLE__)
#define RSTD_OS_APPLE 1
#endif

#if defined(__unix__) || defined(__APPLE__)
#define RSTD_OS_UNIX 1
#endif

#define RSTD_DETAIL_PARTIAL_EQ_OPERATORS()                                  \
    template<typename _USE_TRAIT_T>                                         \
        requires rstd::Impled<Self, rstd::cmp::PartialEq<_USE_TRAIT_T>>     \
    friend bool operator==(const Self& a, const _USE_TRAIT_T& b) noexcept { \
        return as<rstd::cmp::PartialEq<_USE_TRAIT_T>>(a).eq(b);             \
    }

#define RSTD_DETAIL_DEREF_OPERATORS()                                              \
    template<typename _USE_TRAIT_SELF = Self>                                      \
        requires rstd::Impled<_USE_TRAIT_SELF, rstd::ops::Deref>                   \
    constexpr decltype(auto) operator*() const noexcept {                          \
        return rstd::ops::deref_value(static_cast<const _USE_TRAIT_SELF&>(*this)); \
    }                                                                              \
    template<typename _USE_TRAIT_SELF = Self>                                      \
        requires rstd::Impled<_USE_TRAIT_SELF, rstd::ops::DerefMut>                \
    constexpr decltype(auto) operator*() noexcept {                                \
        return rstd::ops::deref_value(static_cast<_USE_TRAIT_SELF&>(*this));       \
    }                                                                              \
    template<typename _USE_TRAIT_SELF = Self>                                      \
        requires rstd::Impled<_USE_TRAIT_SELF, rstd::ops::Deref>                   \
    constexpr auto operator->() const noexcept {                                   \
        return rstd::ops::deref_arrow(static_cast<const _USE_TRAIT_SELF&>(*this)); \
    }                                                                              \
    template<typename _USE_TRAIT_SELF = Self>                                      \
        requires rstd::Impled<_USE_TRAIT_SELF, rstd::ops::DerefMut>                \
    constexpr auto operator->() noexcept {                                         \
        return rstd::ops::deref_arrow(static_cast<_USE_TRAIT_SELF&>(*this));       \
    }

#define USE_TRAIT(Class)               \
    using Self = Class;                \
    template<typename, typename>       \
    friend struct rstd::Impl;          \
    RSTD_DETAIL_PARTIAL_EQ_OPERATORS() \
    RSTD_DETAIL_DEREF_OPERATORS()

// On Windows with Clang modules, friend declarations of Impl across module
// partitions cause "attached to named module" errors. USE_TRAIT_WIN provides
// the same operators without the problematic friend declaration.
#if defined(RSTD_OS_WINDOWS)
#define USE_TRAIT_WIN(Class)           \
    using Self = Class;                \
    RSTD_DETAIL_PARTIAL_EQ_OPERATORS() \
    RSTD_DETAIL_DEREF_OPERATORS()
#else
#define USE_TRAIT_WIN(Class) USE_TRAIT(Class)
#endif

#define RSTD_STR(a, ...)   #a
#define RSTD_FIRST(a, ...) a
#define RSTD_REST(a, ...)  __VA_ARGS__

#define RSTD_REST_ARGS(a, ...) __VA_OPT__(, ) __VA_ARGS__

#define RSTD_DETAIL_TRY_BODY_1(EXPR, RETURN)                                    \
    __extension__({                                                             \
        auto&& rstd_try_result_ = (EXPR);                                       \
        static_assert(::rstd::try_::TrySource<decltype(rstd_try_result_)>,      \
                      "rstd_try requires rstd::Result or rstd::Option");        \
        if (! ::rstd::try_::is_success(rstd_try_result_)) {                     \
            RETURN ::rstd::try_::take_residual(::rstd::move(rstd_try_result_)); \
        }                                                                       \
        ::rstd::try_::take_output(::rstd::move(rstd_try_result_));              \
    })

#define RSTD_DETAIL_TRY_BODY_2(EXPR, FALLBACK, RETURN)                                          \
    __extension__({                                                                             \
        auto&& rstd_try_result_ = (EXPR);                                                       \
        static_assert(::rstd::try_::TrySource<decltype(rstd_try_result_)>,                      \
                      "rstd_try requires rstd::Result or rstd::Option");                        \
        if (! ::rstd::try_::is_success(rstd_try_result_)) {                                     \
            auto&& _e = ::rstd::try_::take_failure(::rstd::move(rstd_try_result_));             \
            RETURN ::rstd::try_::into_residual(                                                 \
                ::rstd::try_::resolve_fallback((FALLBACK), ::rstd::forward<decltype(_e)>(_e))); \
        }                                                                                       \
        ::rstd::try_::take_output(::rstd::move(rstd_try_result_));                              \
    })

#define RSTD_DETAIL_TRY_1(EXPR) ::rstd::try_::finish(RSTD_DETAIL_TRY_BODY_1(EXPR, return))
#define RSTD_DETAIL_TRY_2(EXPR, FALLBACK) \
    ::rstd::try_::finish(RSTD_DETAIL_TRY_BODY_2(EXPR, FALLBACK, return))
#define RSTD_DETAIL_CO_TRY_1(EXPR) ::rstd::try_::finish(RSTD_DETAIL_TRY_BODY_1(EXPR, co_return))
#define RSTD_DETAIL_CO_TRY_2(EXPR, FALLBACK) \
    ::rstd::try_::finish(RSTD_DETAIL_TRY_BODY_2(EXPR, FALLBACK, co_return))

#define RSTD_DETAIL_SELECT_TRY(_1, _2, NAME, ...) NAME
#define rstd_try(...) \
    RSTD_DETAIL_SELECT_TRY(__VA_ARGS__, RSTD_DETAIL_TRY_2, RSTD_DETAIL_TRY_1)(__VA_ARGS__)
#define rstd_co_try(...) \
    RSTD_DETAIL_SELECT_TRY(__VA_ARGS__, RSTD_DETAIL_CO_TRY_2, RSTD_DETAIL_CO_TRY_1)(__VA_ARGS__)

#ifdef NDEBUG
#define debug_assert(...)    ((void)0)
#define debug_assert_eq(...) ((void)0)
#else
#define debug_assert(EXP, ...) \
    if (! (EXP)) rstd::panic(#EXP RSTD_STR(__VA_ARGS__) RSTD_REST_ARGS(__VA_ARGS__))
#define debug_assert_eq(A, B) debug_assert((A) == (B))
#endif

#define rstd_assert(EXP, ...) \
    if (! (EXP)) rstd::panic(#EXP RSTD_STR(__VA_ARGS__) RSTD_REST_ARGS(__VA_ARGS__))
#define rstd_assert_eq(A, B) rstd_assert((A) == (B))

#define rstd_error(...)                                            \
    do {                                                           \
        if (rstd::log::log_enabled(rstd::log::Level::Error, "")) { \
            rstd::log::error(__VA_ARGS__);                         \
        }                                                          \
    } while (0)

#define rstd_warn(...)                                            \
    do {                                                          \
        if (rstd::log::log_enabled(rstd::log::Level::Warn, "")) { \
            rstd::log::warn(__VA_ARGS__);                         \
        }                                                         \
    } while (0)

#define rstd_info(...)                                            \
    do {                                                          \
        if (rstd::log::log_enabled(rstd::log::Level::Info, "")) { \
            rstd::log::info(__VA_ARGS__);                         \
        }                                                         \
    } while (0)

#ifdef NDEBUG
#define rstd_debug(...) ((void)0)
#else
#define rstd_debug(...)                                            \
    do {                                                           \
        if (rstd::log::log_enabled(rstd::log::Level::Debug, "")) { \
            rstd::log::debug(__VA_ARGS__);                         \
        }                                                          \
    } while (0)
#endif

#define rstd_trace(...)                                            \
    do {                                                           \
        if (rstd::log::log_enabled(rstd::log::Level::Trace, "")) { \
            rstd::log::trace(__VA_ARGS__);                         \
        }                                                          \
    } while (0)

// Target-specific macros
#define rstd_error_t(TARGET, ...)                                      \
    do {                                                               \
        if (rstd::log::log_enabled(rstd::log::Level::Error, TARGET)) { \
            rstd::log::error(TARGET, __VA_ARGS__);                     \
        }                                                              \
    } while (0)

#define rstd_warn_t(TARGET, ...)                                      \
    do {                                                              \
        if (rstd::log::log_enabled(rstd::log::Level::Warn, TARGET)) { \
            rstd::log::warn(TARGET, __VA_ARGS__);                     \
        }                                                             \
    } while (0)

#define rstd_info_t(TARGET, ...)                                      \
    do {                                                              \
        if (rstd::log::log_enabled(rstd::log::Level::Info, TARGET)) { \
            rstd::log::info(TARGET, __VA_ARGS__);                     \
        }                                                             \
    } while (0)

#ifdef NDEBUG
#define rstd_debug_t(...) ((void)0)
#else
#define rstd_debug_t(TARGET, ...)                                      \
    do {                                                               \
        if (rstd::log::log_enabled(rstd::log::Level::Debug, TARGET)) { \
            rstd::log::debug(TARGET, __VA_ARGS__);                     \
        }                                                              \
    } while (0)
#endif

#define rstd_trace_t(TARGET, ...)                                      \
    do {                                                               \
        if (rstd::log::log_enabled(rstd::log::Level::Trace, TARGET)) { \
            rstd::log::trace(TARGET, __VA_ARGS__);                     \
        }                                                              \
    } while (0)

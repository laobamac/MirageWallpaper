export module rstd.core:trait;
export import rstd.basic;

namespace rstd
{

/// A compile-time collection of function pointers representing a trait's API.
/// \tparam Api The member function pointers that form the trait interface.
export template<auto... Api>
struct TraitFuncs {};

/// The trait implementation specialization point.
/// Users specialize this struct to implement trait T for type A.
/// \tparam T The trait type to implement.
/// \tparam A The concrete type implementing the trait.
export template<typename T, typename A>
struct Impl;
namespace mtp
{
template<typename T>
struct TraitApiTraits;
}

namespace mtp
{
/// Checks whether T is a valid trait API type with an associated Trait and type info.
export template<typename T>
concept is_trait_api = requires() {
    typename T::Trait;
    typename mtp::TraitApiTraits<T>::type;
};

/// Checks whether T is a direct trait (implemented directly on the type, not via Impl).
export template<typename T>
concept is_direct_trait = T::direct;

/// Checks whether T has an Api member template defining trait methods.
template<template<typename, typename> typename>
struct trait_api_template_probe {};

export template<typename T>
concept has_trait_api = requires { typename trait_api_template_probe<T::template Api>; };

} // namespace mtp

namespace ptr_
{
/// Type-erased delegate for dynamic dispatch of trait T through a vtable.
/// \tparam T The trait type to dispatch dynamically.
export template<typename T>
struct dyn_delegate;
} // namespace ptr_

/// Tag type used to select a default trait implementation for type T.
/// \tparam T The type for which to provide a default implementation.
export template<typename T>
struct default_tag {};

/// Tag type used to select default trait methods inherited directly by T.
/// \tparam T The type inheriting the default implementation.
export template<typename T>
struct in_class_default_tag {};

export struct in_class_tag {};

export struct dyn_tag {};

namespace mtp
{

export struct ImplHelper;

export struct api_check_tag {};

export template<typename Ret>
[[noreturn]]
auto trait_check_return() -> Ret {
    __builtin_unreachable();
}

export template<typename T>
struct TraitFuncsHelper;

export template<auto... Api>
struct TraitFuncsHelper<TraitFuncs<Api...>> {
    template<template<class...> typename T>
    consteval static auto make() {
        return T { Api... };
    }

    template<usize I>
    consteval static auto get() {
        return mtp::get_auto<I, Api...>();
    }

    consteval static auto size() { return sizeof...(Api); }

    template<usize I>
    using type_at = decltype(get<I>());
};

export template<typename T>
struct ImplWithPtr {
    usizeptr ptr_;

    friend struct ImplHelper;

    template<typename P>
    constexpr ImplWithPtr(P* p) noexcept: ptr_(rstd::bit_cast<usizeptr>(p)) {}

protected:
    constexpr auto self() noexcept -> T& { return *rstd::bit_cast<T*>(ptr_); }
    constexpr auto self() const noexcept -> T const& { return *rstd::bit_cast<T const*>(ptr_); }
};

export template<template<typename, typename> class T, typename A, typename B>
struct TraitApiTraits<T<A, B>> {
    using type          = A;
    using delegate_type = B;
};

export template<typename Trait, typename A, typename Delegate = void>
using TraitApi = typename Trait::template Api<A, Delegate>;

export template<typename Trait, typename A>
using TraitCheckApi = TraitApi<Trait, A, api_check_tag>;

export template<typename Trait, typename A>
using TraitFuncs = typename Trait::template Funcs<A>;

export template<typename Trait, typename A>
using TraitApiHelper = TraitFuncsHelper<TraitFuncs<Trait, A>>;

export template<typename F>
using trait_func_return_t = typename mtp::func_traits<mtp::rm_cv<F>>::ret;

export template<usize I, typename Trait, typename A>
using trait_api_return_t =
    trait_func_return_t<decltype(TraitApiHelper<Trait, A>::template get<I>())>;

template<typename Trait>
concept trait_allows_const_member_impl =
    requires { Trait::allow_const_member_impl; } && Trait::allow_const_member_impl;

template<typename Trait, typename Expected, typename Actual>
constexpr bool trait_member_primary_compatible =
    mtp::is_ref_lv<Expected> == mtp::is_ref_lv<Actual> &&
    mtp::is_ref_rv<Expected> == mtp::is_ref_rv<Actual> &&
    (mtp::is_const<mtp::rm_ref<Expected>> == mtp::is_const<mtp::rm_ref<Actual>> ||
     (trait_allows_const_member_impl<Trait> && ! mtp::is_const<mtp::rm_ref<Expected>> &&
      mtp::is_const<mtp::rm_ref<Actual>>));

export template<typename... Api, template<class...> typename Tuple>
consteval auto to_dyn(Tuple<Api...>) {
    return Tuple { (typename mtp::func_traits<Api>::to_dyn)(nullptr)... };
}
export struct DynHelper {
    template<typename TDyn>
    static auto get_self(TDyn* t) noexcept {
        return t->p;
    }

    template<typename TDyn>
    static const auto* get_apis(TDyn const* t) noexcept {
        return rstd::addressof(t->vtable->apis);
    }
};

export struct ImplHelper {
    template<typename T>
    static auto& get_self(T* t) noexcept {
        return t->self();
    }
};

template<typename...>
constexpr bool dependent_false = false;

export enum class trait_impl_kind
{
    None,
    Dyn,
    Direct,
    External,
    InClass
};

enum class trait_impl_failure_reason
{
    None,
    NoImpl,
    ExternalUnavailable,
    ExternalApiMismatch,
    InClassApiMismatch,
    DirectApiMismatch
};

template<typename Trait, typename A>
using external_trait_impl_t = Impl<Trait, A>;

export template<typename T>
constexpr bool trait_default_tag_v = false;

export template<typename T>
constexpr bool trait_default_tag_v<default_tag<T>> = true;

export template<typename T>
constexpr bool trait_default_tag_v<in_class_default_tag<T>> = true;

export template<typename T>
concept trait_default_tag = trait_default_tag_v<T>;

export template<typename T>
struct trait_default_self;

export template<typename T>
struct trait_default_self<default_tag<T>> {
    using type = T;
};

export template<typename T>
struct trait_default_self<in_class_default_tag<T>> {
    using type = T;
};

export template<typename T>
using trait_default_self_t = typename trait_default_self<T>::type;

template<typename Trait, typename A>
concept has_external_trait_impl = mtp::complete<external_trait_impl_t<Trait, A>>;

template<typename Trait, typename A>
concept trait_funcs_formable = mtp::has_trait_api<Trait> && requires {
    typename TraitApiHelper<Trait, A>;
    TraitApiHelper<Trait, A>::size();
};

template<typename Trait, typename A, typename ToCheck>
concept trait_apis_formable = trait_funcs_formable<Trait, ToCheck> && requires {
    typename TraitCheckApi<Trait, A>;
    typename TraitApiHelper<Trait, TraitCheckApi<Trait, A>>;
    typename TraitApiHelper<Trait, ToCheck>;
    TraitApiHelper<Trait, TraitCheckApi<Trait, A>>::size();
    TraitApiHelper<Trait, ToCheck>::size();
};

template<typename Trait, typename A>
concept has_in_class_trait_candidate =
    mtp::complete<A> && trait_funcs_formable<Trait, A> && (TraitApiHelper<Trait, A>::size() > 0);

template<typename Trait, typename Expected, typename Actual>
consteval bool trait_api_entry_matches() {
    using F1 = mtp::func_traits<Expected>;
    using F2 = mtp::func_traits<Actual>;

    if constexpr (F1::is_member != F2::is_member) {
        return false;
    } else if constexpr (F1::is_member) {
        using P1 = typename F1::primary;
        using P2 = typename F2::primary;

        return trait_member_primary_compatible<Trait, P1, P2> &&
               mtp::same_as<typename F1::to_dyn, typename F2::to_dyn>;
    } else {
        return mtp::same_as<Expected, Actual>;
    }
}

template<bool Diagnose, typename Trait, typename ExpectedApi, typename ToCheck, usize Index = 0>
consteval bool check_apis_match() {
    using ApiHelperA = TraitApiHelper<Trait, ExpectedApi>;
    using ApiHelperB = TraitApiHelper<Trait, ToCheck>;

    if constexpr (Index == 0) {
        if constexpr (ApiHelperA::size() != ApiHelperB::size()) {
            if constexpr (Diagnose) {
                static_assert(ApiHelperA::size() == ApiHelperB::size(),
                              "Please implement all Trait api");
            }
            return false;
        }
    }

    if constexpr (Index < ApiHelperA::size()) {
        using T1 = ApiHelperA::template type_at<Index>;
        using T2 = ApiHelperB::template type_at<Index>;

        if constexpr (! trait_api_entry_matches<Trait, T1, T2>()) {
            if constexpr (Diagnose) {
                static_assert(trait_api_entry_matches<Trait, T1, T2>(), "Trait api not satisfy");
            }
            return false;
        }
        return check_apis_match<Diagnose, Trait, ExpectedApi, ToCheck, Index + 1>();
    }
    return true;
}

template<typename Trait, typename ExpectedApi, typename ToCheck>
consteval bool check_apis() {
    return check_apis_match<true, Trait, ExpectedApi, ToCheck>();
}

/// Validates that ToCheck correctly implements all API methods for trait T with type A.
/// \tparam T The trait type.
/// \tparam A The type parameter for the trait API.
/// \tparam ToCheck The type whose API methods are validated.
/// \return true if the APIs match.
export template<typename T, typename A, typename ToCheck>
consteval bool check_trait_apis() {
    static_assert(mtp::has_trait_api<T>);
    return check_apis<T, TraitCheckApi<T, A>, ToCheck>();
}

template<typename Trait, typename ExpectedApi, typename ToCheck>
consteval bool check_apis_quiet() {
    return check_apis_match<false, Trait, ExpectedApi, ToCheck>();
}

template<typename Trait, typename A, typename ToCheck>
consteval bool check_trait_apis_quiet() {
    if constexpr (! mtp::has_trait_api<Trait>) {
        return true;
    } else if constexpr (! trait_apis_formable<Trait, A, ToCheck>) {
        return false;
    } else {
        return check_apis_quiet<Trait, TraitCheckApi<Trait, A>, ToCheck>();
    }
}

template<typename Trait, typename A>
consteval auto select_trait_impl_kind() -> trait_impl_kind {
    if constexpr (mtp::same_as<A, dyn_tag>) {
        return trait_impl_kind::Dyn;
    } else if constexpr (mtp::is_direct_trait<Trait>) {
        return trait_impl_kind::Direct;
    } else if constexpr (has_external_trait_impl<Trait, A>) {
        return trait_impl_kind::External;
    } else if constexpr (has_in_class_trait_candidate<Trait, A>) {
        return trait_impl_kind::InClass;
    } else {
        return trait_impl_kind::None;
    }
}

template<typename Trait, typename A, trait_impl_kind Kind>
struct trait_impl_source_for;

template<typename Trait, typename A>
struct trait_impl_source_for<Trait, A, trait_impl_kind::Dyn> {
    using api_owner              = dyn_tag;
    static constexpr auto kind   = trait_impl_kind::Dyn;
    static constexpr bool value  = true;
    static constexpr auto reason = trait_impl_failure_reason::None;
};

template<typename Trait, typename A>
struct trait_impl_source_for<Trait, A, trait_impl_kind::Direct> {
    using api_owner             = A;
    static constexpr auto kind  = trait_impl_kind::Direct;
    static constexpr bool value = check_trait_apis_quiet<Trait, A, A>();
    static constexpr auto reason =
        value ? trait_impl_failure_reason::None : trait_impl_failure_reason::DirectApiMismatch;
};

template<typename Trait, typename A>
struct trait_impl_source_for<Trait, A, trait_impl_kind::External> {
    using api_owner                 = Impl<Trait, A>;
    static constexpr auto kind      = trait_impl_kind::External;
    static constexpr bool available = mtp::drop<Impl<Trait, A>>;
    static constexpr bool api_ok = available && check_trait_apis_quiet<Trait, A, Impl<Trait, A>>();
    static constexpr bool value  = available && api_ok;
    static constexpr auto reason =
        value ? trait_impl_failure_reason::None
              : (available ? trait_impl_failure_reason::ExternalApiMismatch
                           : trait_impl_failure_reason::ExternalUnavailable);
};

template<typename Trait, typename A>
struct trait_impl_source_for<Trait, A, trait_impl_kind::InClass> {
    using api_owner             = A;
    static constexpr auto kind  = trait_impl_kind::InClass;
    static constexpr bool value = check_trait_apis_quiet<Trait, A, A>();
    static constexpr auto reason =
        value ? trait_impl_failure_reason::None : trait_impl_failure_reason::InClassApiMismatch;
};

template<typename Trait, typename A>
struct trait_impl_source_for<Trait, A, trait_impl_kind::None> {
    using api_owner              = void;
    static constexpr auto kind   = trait_impl_kind::None;
    static constexpr bool value  = false;
    static constexpr auto reason = trait_impl_failure_reason::NoImpl;
};

export template<typename Trait, typename A>
struct trait_impl_source : trait_impl_source_for<Trait, A, select_trait_impl_kind<Trait, A>()> {};

template<typename Trait, typename A, trait_impl_failure_reason Reason>
struct trait_impl_failure;

template<typename Trait, typename A>
struct trait_impl_failure<Trait, A, trait_impl_failure_reason::NoImpl> {
    static_assert(dependent_false<Trait, A>,
                  "rstd::Impled failed: no external Impl or complete in-class trait API");
    static constexpr bool value = false;
};

template<typename Trait, typename A>
struct trait_impl_failure<Trait, A, trait_impl_failure_reason::ExternalUnavailable> {
    static_assert(dependent_false<Trait, A>,
                  "rstd::Impled failed: external Impl exists but is not usable");
    static constexpr bool value = false;
};

template<typename Trait, typename A>
struct trait_impl_failure<Trait, A, trait_impl_failure_reason::ExternalApiMismatch> {
    static_assert(dependent_false<Trait, A>,
                  "rstd::Impled failed: external Impl does not satisfy trait API");
    static constexpr bool value = false;
};

template<typename Trait, typename A>
struct trait_impl_failure<Trait, A, trait_impl_failure_reason::InClassApiMismatch> {
    static_assert(dependent_false<Trait, A>,
                  "rstd::Impled failed: in-class implementation does not satisfy trait API");
    static constexpr bool value = false;
};

template<typename Trait, typename A>
struct trait_impl_failure<Trait, A, trait_impl_failure_reason::DirectApiMismatch> {
    static_assert(dependent_false<Trait, A>,
                  "rstd::Impled failed: direct trait implementation does not satisfy trait API");
    static constexpr bool value = false;
};

export template<typename Trait, typename A>
consteval bool check_trait_or_diagnose() {
    using source = trait_impl_source<Trait, A>;
    if constexpr (source::value) {
        return true;
    } else {
        return trait_impl_failure<Trait, A, source::reason>::value;
    }
}

/// Checks whether type A implements trait T, either directly or via Impl.
/// \tparam T The trait type.
/// \tparam A The type to check.
/// \return true if A implements T.
export template<typename T, typename A>
consteval bool check_trait() {
    return trait_impl_source<T, A>::value;
}

} // namespace mtp

/// Base class for Impl specializations, providing a pointer-based self accessor.
/// \tparam A The concrete type that the Impl operates on.
export template<typename A>
struct ImplBase : mtp::ImplWithPtr<A> {};

template<typename A>
struct ImplBase<default_tag<A>> : ImplBase<A> {};

// This is used as a base class for the user class, not for an external Impl.
template<typename A>
struct ImplBase<in_class_default_tag<A>> {
    template<typename, typename>
    friend struct Impl;

private:
    auto self() -> A& { return *static_cast<A*>(this); }
    auto self() const -> A const& { return *static_cast<A const*>(this); }
};

/// Checks whether type A implements all the given traits T.
/// \tparam A The type to check.
/// \tparam T The trait types to verify.
export template<typename A, typename... T>
concept Impled = (mtp::check_trait<T, mtp::rm_cvf<A>>() && ...);

/// Default methods inherited by a class that wants them as members.
/// \tparam Self The concrete type.
/// \tparam T The trait that owns the default methods.
export template<typename Self, typename T>
using DefaultInClass = Impl<T, in_class_default_tag<Self>>;

/// Default methods inherited by an Impl specialization.
/// \tparam T The trait that owns the default methods.
/// \tparam Self The concrete type.
export template<typename T, typename Self>
using DefaultInImpl = Impl<T, default_tag<Self>>;

/// Adapts a concrete type's members through an external Impl wrapper.
/// \tparam T The trait type.
/// \tparam A The concrete type whose methods satisfy the trait.
export template<typename T, typename A>
struct LinkClassMethod : mtp::ImplWithPtr<A>, mtp::rm_cv<T>::template Api<A, in_class_tag> {};

/// Dispatches a trait method call to the appropriate Impl, handling static, dynamic, and in-class
/// dispatch.
/// \tparam I The index of the method in the trait's API function list.
/// \tparam TApi The trait API type (deduced from self).
/// \param self Pointer to the trait API object.
/// \param args Arguments forwarded to the trait method.
/// \return The result of the dispatched method call.
export template<usize I, typename TApi, typename... Args>
    requires mtp::is_trait_api<mtp::rm_cv<TApi>>
[[gnu::always_inline]]
inline constexpr decltype(auto) trait_call(TApi* self, Args&&... args) {
    using TApi_    = mtp::rm_cv<TApi>;
    using Trait    = typename TApi_::Trait;
    using TClass   = typename mtp::TraitApiTraits<TApi_>::type;
    using Delegate = typename mtp::TraitApiTraits<TApi_>::delegate_type;
    using TImpl    = Impl<Trait, TClass>;

    if constexpr (mtp::same_as<Delegate, mtp::api_check_tag>) {
        using Ret = mtp::trait_api_return_t<I, Trait, TApi_>;
        return mtp::trait_check_return<Ret>();
    } else if constexpr (mtp::same_as<TClass, dyn_tag>) {
        auto dyn = static_cast<mtp::follow_const_t<TApi, ptr_::dyn_delegate<Trait>>*>(self);

        const auto* apis = mtp::DynHelper::get_apis(dyn);
        return rstd::get<I>(*apis)(mtp::DynHelper::get_self(dyn), rstd::forward<Args>(args)...);
    } else if constexpr (mtp::same_as<Delegate, in_class_tag>) {
        constexpr const auto api { mtp::TraitApiHelper<Trait, TClass>::template get<I>() };

        auto impl_in_class =
            static_cast<mtp::follow_const_t<TApi, LinkClassMethod<Trait, TClass>>*>(self);

        const auto self_ = rstd::addressof(mtp::ImplHelper::get_self(impl_in_class));
        return (self_->*api)(rstd::forward<Args>(args)...);
    } else {
        constexpr const auto api { mtp::TraitApiHelper<Trait, TImpl>::template get<I>() };

        TImpl const self_ { static_cast<mtp::follow_const_t<TApi, TClass>*>(self) };
        return (self_.*api)(rstd::forward<Args>(args)...);
    }
}

/// Dispatches a static (non-member) trait method call to the appropriate Impl.
/// \tparam I The index of the method in the trait's API function list.
/// \tparam TApi The trait API type.
/// \param args Arguments forwarded to the static trait method.
/// \return The result of the dispatched method call.
export template<usize I, typename TApi, typename... Args>
    requires mtp::is_trait_api<mtp::rm_cv<TApi>>
[[gnu::always_inline]]
inline constexpr decltype(auto) trait_static_call(Args&&... args) {
    using TApi_    = mtp::rm_cv<TApi>;
    using Trait    = typename TApi_::Trait;
    using TClass   = typename mtp::TraitApiTraits<TApi_>::type;
    using Delegate = typename mtp::TraitApiTraits<TApi_>::delegate_type;
    using TImpl    = Impl<Trait, TClass>;

    if constexpr (mtp::same_as<Delegate, mtp::api_check_tag>) {
        using Ret = mtp::trait_api_return_t<I, Trait, TApi_>;
        return mtp::trait_check_return<Ret>();
    } else {
        constexpr const auto api { mtp::TraitApiHelper<Trait, TImpl>::template get<I>() };
        return api(rstd::forward<Args>(args)...);
    }
}

/// Casts an lvalue reference to a trait view, returning the Impl wrapper for trait T.
/// Only accepts lvalues to prevent stack-use-after-scope.
/// \tparam T The trait type to cast to.
/// \tparam A The concrete type (deduced).
/// \param t The object to view as the trait.
/// \return A trait Impl wrapper, or a reference to t for direct and in-class implementations.
export template<typename T, typename A>
[[gnu::always_inline]]
inline constexpr decltype(auto) as(A& t) noexcept {
    using class_t = mtp::rm_cvf<A>;
    using source  = mtp::trait_impl_source<T, class_t>;
    if constexpr (! source::value) {
        static_assert(mtp::check_trait_or_diagnose<T, class_t>());
    } else if constexpr (source::kind == mtp::trait_impl_kind::Direct ||
                         source::kind == mtp::trait_impl_kind::InClass) {
        return t;
    } else {
        using ret_t = mtp::follow_const_t<A, typename source::api_owner>;
        return ret_t { rstd::addressof(t) };
    }
}

} // namespace rstd

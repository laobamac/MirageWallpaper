export module rstd.basic:basic;
export import :mtp;

namespace rstd
{

/// Obtains the actual address of an object, even if `operator&` is overloaded.
/// \param val Reference to the object.
/// \return Pointer to the object.
export template<typename T>
[[gnu::always_inline]]
constexpr auto addressof(T& val) noexcept -> T* {
    return __builtin_addressof(val);
}

/// Replaces `obj` with `new_val`, returning the old value.
/// \param obj The object whose value is replaced.
/// \param new_val The new value to assign.
/// \return The previous value of `obj`.
export template<typename T, typename U = T>
constexpr inline T exchange(T&  obj,
                            U&& new_val) noexcept(noexcept(T(rstd::move(obj))) &&
                                                 noexcept(obj = rstd::forward<U>(new_val))) {
    T old = rstd::move(obj);
    obj   = rstd::forward<U>(new_val);
    return old;
}

/// Constructs an object of type `T` at the given memory location.
/// \param location Pointer to uninitialized storage.
/// \param args Arguments forwarded to the constructor.
/// \return Pointer to the constructed object.
export template<typename T, typename... Args>
    requires(! mtp::is_array_dst<T>) && requires { new T { mtp::declval<Args>()... }; }
constexpr T* construct_at(T* location,
                          Args&&... args) noexcept(noexcept(new T { mtp::declval<Args>()... })) {
    void* loc = location;
    if constexpr (mtp::is_array<T>) {
        static_assert(sizeof...(Args) == 0,
                      "std::construct_at for array "
                      "types must not use any arguments to initialize the "
                      "array");
        return new (loc) T[1]();
    } else
        return new (loc) T { rstd::forward<Args>(args)... };
}
/// Destroys the object at the given location by calling its destructor.
/// \param location Pointer to the object to destroy.
export template<typename T>
[[gnu::always_inline]]
constexpr inline void destroy_at(T* location) {
    if constexpr (__cplusplus > 201703L && mtp::is_array<T>) {
        for (auto& x : *location) rstd::destroy_at(rstd::addressof(x));
    } else
        location->~T();
}

/// Obtains a pointer to an object created in storage occupied by another object of the same type.
/// \param __p Pointer to launder.
/// \return A pointer to the same memory with the correct type provenance.
export template<typename T>
[[nodiscard]]
constexpr T* launder(T* __p) noexcept {
    if constexpr (__is_same(const volatile T, const volatile void))
        static_assert(! __is_same(const volatile T, const volatile void),
                      "std::launder argument must not be a void pointer");
    else if constexpr (__is_function(T))
        static_assert(! __is_function(T), "std::launder argument must not be a function pointer");
    else
        return __builtin_launder(__p);
    return nullptr;
}

/// Returns the length of a null-terminated byte string.
/// \param s Pointer to a null-terminated string.
/// \return The number of characters before the null terminator.
export constexpr auto strlen(char const* s) noexcept -> usize {
    if (mtp::is_constant_evaluated()) {
        usize i = 0;
        while (s[i] != '\0') ++i;
        return i;
    } else {
        return __builtin_strlen(s);
    }
}

/// Reinterprets the bits of `from` as a value of type `To`.
/// \tparam To A trivially-copyable destination type.
/// \tparam From A trivially-copyable source type with the same size as `To`.
/// \param from The source value.
/// \return An object of type `To` with the same bit representation.
export template<typename To, typename From>
[[nodiscard]]
constexpr To bit_cast(const From& from) noexcept
    requires(sizeof(To) == sizeof(From)) && mtp::triv_copy<To> && mtp::triv_copy<From>
{
    return __builtin_bit_cast(To, from);
}

/// Informs the compiler that this point in the code is unreachable.
export [[noreturn]]
inline void unreachable() {
    // Uses compiler specific extensions if possible.
    // Even if no extension is used, undefined behavior is still raised by
    // an empty function body and the noreturn attribute.
#if defined(_MSC_VER) && ! defined(__clang__) // MSVC
    __assume(false);
#else // GCC, Clang
    __builtin_unreachable();
#endif
}

/// Swaps the values of `a` and `b`.
export template<typename T>
void swap(T& a, T& b) noexcept(mtp::noex_copy<T>) {
    T t(a);
    a = b;
    b = t;
}

/// Pointer overload of `swap`.
///
/// 任何以 rstd 类型为基类/模板实参的类型都会让 `rstd` 进入 ADL 关联命名空间；MSVC STL 的
/// 容器搬迁代码写的是 `swap(p, q)`（注释为 intentional ADL），此时与本文件上面的通用
/// `swap` 与 `std::swap` 同为候选且难分优劣，会被判为二义调用。指针重载更特化，能稳定
/// 胜出，从而让这类 ADL 调用保持无歧义。
export template<typename T>
void swap(T*& a, T*& b) noexcept {
    T* temporary = a;
    a            = b;
    b            = temporary;
}

/// Returns the smaller of two values.
export template<typename T>
constexpr auto min(T a, T b) noexcept -> T {
    return a > b ? b : a;
}

template<typename InputIter1, typename InputIter2, typename Compare>
[[nodiscard]]
constexpr auto lexicographical_compare_three_way(InputIter1 first1,
                                                 InputIter1 last1,
                                                 InputIter2 first2,
                                                 InputIter2 last2,
                                                 Compare    comp) {
    if constexpr (mtp::is_ptr<InputIter1> && mtp::is_ptr<InputIter2>) {
        if (! mtp::is_constant_evaluated()) {
            using T1 = std::iter_value_t<InputIter1>;
            using T2 = std::iter_value_t<InputIter2>;

            if constexpr (mtp::same<T1, T2> && mtp::is_int<T1> && sizeof(T1) == 1) {
                auto len1    = last1 - first1;
                auto len2    = last2 - first2;
                auto min_len = rstd::min(len1, len2);

                if (min_len > 0) {
                    int result = __builtin_memcmp(first1, first2, min_len);
                    if (result != 0) {
                        return result <=> 0;
                    }
                }
                return len1 <=> len2;
            }
        }
    }

    while (first1 != last1) {
        if (first2 == last2) {
            return std::strong_ordering::greater;
        }

        if (auto cmp = comp(*first1, *first2); cmp != 0) {
            return cmp;
        }

        ++first1;
        ++first2;
    }

    return (first1 == last1) <=> (first2 == last2);
}

/// Performs lexicographic three-way comparison of two ranges using `<=>`.
/// \return The ordering result of the first differing element pair, or a length comparison.
export template<typename InputIter1, typename InputIter2>
constexpr auto lexicographical_compare_three_way(InputIter1 first1,
                                                 InputIter1 last1,
                                                 InputIter2 first2,
                                                 InputIter2 last2) {
    return rstd::lexicographical_compare_three_way(
        first1, last1, first2, last2, std::compare_three_way {});
}

} // namespace rstd

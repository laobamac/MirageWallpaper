module;
#if defined(_MSC_VER)
#include <intrin.h>
#endif
export module rstd.core:hint;

export namespace rstd::hint
{
/// Emits a hardware hint to the processor that the current thread is in a spin loop.
inline void spin_loop() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
    __yield();
#else
    __asm__ __volatile__("yield");
#endif
#endif
}

/// Hides a value from the optimizer while preserving normal ownership semantics.
template<typename T>
auto black_box(T&& value) noexcept -> T&& {
#if defined(_MSC_VER)
    // MSVC: use _ReadWriteBarrier + volatile to prevent optimization
    volatile auto* ptr = &value;
    (void)ptr;
#else
    asm volatile("" : : "g"(&value) : "memory");
#endif
    return static_cast<T&&>(value);
}
} // namespace rstd::hint

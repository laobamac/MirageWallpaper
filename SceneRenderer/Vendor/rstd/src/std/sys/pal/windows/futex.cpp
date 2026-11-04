module;
#include <rstd/macro.hpp>
#if RSTD_OS_WINDOWS
#pragma comment(lib, "synchronization")
#endif
module rstd;
import :sys.pal.windows.futex;

#if RSTD_OS_WINDOWS
import :sys.libc.windows;
using namespace rstd::sys::libc;

namespace rstd::sys::pal::windows::futex
{
using rstd::sync::atomic::Atomic;

bool wait_on_address(const void* address, const void* compare, usize size, DWORD timeout) {
    return WaitOnAddress(const_cast<void*>(address), const_cast<void*>(compare), size, timeout) ==
           M_TRUE;
}

void wake_by_address_single(const void* address) {
    WakeByAddressSingle(const_cast<void*>(address));
}

void wake_by_address_all(const void* address) {
    WakeByAddressAll(const_cast<void*>(address));
}

template<typename T>
bool futex_wait(const Atomic<T>* futex, T expected, Option<Duration> timeout) {
    DWORD timeout_ms = M_INFINITE;
    if (timeout) {
        const auto milliseconds = timeout->as_millis();
        timeout_ms = milliseconds >= M_INFINITE ? M_INFINITE - 1 : static_cast<DWORD>(milliseconds);
    }

    bool result = wait_on_address(futex, &expected, sizeof(T), timeout_ms);
    return result || GetLastError() != M_ERROR_TIMEOUT;
}

template<typename T>
bool futex_wake(const Atomic<T>* futex) {
    wake_by_address_single(futex);
    return false;
}

template<typename T>
void futex_wake_all(const Atomic<T>* futex) {
    wake_by_address_all(futex);
}

// Explicit template instantiations
template bool futex_wait<u32>(const Atomic<u32>*, u32, Option<Duration>);
template bool futex_wait<u8>(const Atomic<u8>*, u8, Option<Duration>);
template bool futex_wake<u32>(const Atomic<u32>*);
template bool futex_wake<u8>(const Atomic<u8>*);
template void futex_wake_all<u32>(const Atomic<u32>*);
template void futex_wake_all<u8>(const Atomic<u8>*);
} // namespace rstd::sys::pal::windows::futex

#endif

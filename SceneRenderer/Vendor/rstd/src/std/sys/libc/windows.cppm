module;
#include <rstd/macro.hpp>

#ifdef RSTD_OS_WINDOWS
#include <windows.h>
#include <synchapi.h>
#include <time.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

inline constexpr auto _RSTD_SEEK_SET = SEEK_SET;
inline constexpr auto _RSTD_SEEK_CUR = SEEK_CUR;
inline constexpr auto _RSTD_SEEK_END = SEEK_END;
#undef SEEK_SET
#undef SEEK_CUR
#undef SEEK_END

inline constexpr auto _ERROR_FILE_NOT_FOUND             = ERROR_FILE_NOT_FOUND;
inline constexpr auto _ERROR_PATH_NOT_FOUND             = ERROR_PATH_NOT_FOUND;
inline constexpr auto _ERROR_ACCESS_DENIED              = ERROR_ACCESS_DENIED;
inline constexpr auto _ERROR_CONNECTION_REFUSED         = ERROR_CONNECTION_REFUSED;
inline constexpr auto _ERROR_CONNECTION_ABORTED         = ERROR_CONNECTION_ABORTED;
inline constexpr auto _ERROR_NETNAME_DELETED            = ERROR_NETNAME_DELETED;
inline constexpr auto _ERROR_HOST_UNREACHABLE           = ERROR_HOST_UNREACHABLE;
inline constexpr auto _ERROR_NETWORK_UNREACHABLE        = ERROR_NETWORK_UNREACHABLE;
inline constexpr auto _ERROR_ADDRESS_ALREADY_ASSOCIATED = ERROR_ADDRESS_ALREADY_ASSOCIATED;
inline constexpr auto _ERROR_BROKEN_PIPE                = ERROR_BROKEN_PIPE;
inline constexpr auto _ERROR_NO_DATA                    = ERROR_NO_DATA;
inline constexpr auto _ERROR_FILE_EXISTS                = ERROR_FILE_EXISTS;
inline constexpr auto _ERROR_ALREADY_EXISTS             = ERROR_ALREADY_EXISTS;
inline constexpr auto _WAIT_TIMEOUT                     = WAIT_TIMEOUT;
inline constexpr auto _ERROR_SEM_TIMEOUT                = ERROR_SEM_TIMEOUT;
inline constexpr auto _ERROR_INVALID_PARAMETER          = ERROR_INVALID_PARAMETER;
inline constexpr auto _ERROR_INVALID_DATA               = ERROR_INVALID_DATA;
inline constexpr auto _ERROR_DIR_NOT_EMPTY              = ERROR_DIR_NOT_EMPTY;
inline constexpr auto _ERROR_DISK_FULL                  = ERROR_DISK_FULL;
inline constexpr auto _ERROR_SEEK                       = ERROR_SEEK;
inline constexpr auto _ERROR_NOT_READY                  = ERROR_NOT_READY;
inline constexpr auto _ERROR_BUSY                       = ERROR_BUSY;
inline constexpr auto _ERROR_POSSIBLE_DEADLOCK          = ERROR_POSSIBLE_DEADLOCK;
inline constexpr auto _ERROR_NOT_SAME_DEVICE            = ERROR_NOT_SAME_DEVICE;
inline constexpr auto _ERROR_TOO_MANY_LINKS             = ERROR_TOO_MANY_LINKS;
inline constexpr auto _ERROR_FILENAME_EXCED_RANGE       = ERROR_FILENAME_EXCED_RANGE;
inline constexpr auto _ERROR_NOT_ENOUGH_MEMORY          = ERROR_NOT_ENOUGH_MEMORY;
inline constexpr auto _ERROR_OUTOFMEMORY                = ERROR_OUTOFMEMORY;
inline constexpr auto _ERROR_NOT_SUPPORTED              = ERROR_NOT_SUPPORTED;
inline constexpr auto _ERROR_CALL_NOT_IMPLEMENTED       = ERROR_CALL_NOT_IMPLEMENTED;
inline constexpr auto _ERROR_IO_PENDING                 = ERROR_IO_PENDING;

#undef ERROR_FILE_NOT_FOUND
#undef ERROR_PATH_NOT_FOUND
#undef ERROR_ACCESS_DENIED
#undef ERROR_CONNECTION_REFUSED
#undef ERROR_CONNECTION_ABORTED
#undef ERROR_NETNAME_DELETED
#undef ERROR_HOST_UNREACHABLE
#undef ERROR_NETWORK_UNREACHABLE
#undef ERROR_ADDRESS_ALREADY_ASSOCIATED
#undef ERROR_BROKEN_PIPE
#undef ERROR_NO_DATA
#undef ERROR_FILE_EXISTS
#undef ERROR_ALREADY_EXISTS
#undef WAIT_TIMEOUT
#undef ERROR_SEM_TIMEOUT
#undef ERROR_INVALID_PARAMETER
#undef ERROR_INVALID_DATA
#undef ERROR_DIR_NOT_EMPTY
#undef ERROR_DISK_FULL
#undef ERROR_SEEK
#undef ERROR_NOT_READY
#undef ERROR_BUSY
#undef ERROR_POSSIBLE_DEADLOCK
#undef ERROR_NOT_SAME_DEVICE
#undef ERROR_TOO_MANY_LINKS
#undef ERROR_FILENAME_EXCED_RANGE
#undef ERROR_NOT_ENOUGH_MEMORY
#undef ERROR_OUTOFMEMORY
#undef ERROR_NOT_SUPPORTED
#undef ERROR_CALL_NOT_IMPLEMENTED
#undef ERROR_IO_PENDING
#endif
export module rstd:sys.libc.windows;

#ifdef RSTD_OS_WINDOWS
export namespace rstd::sys::libc
{

// ── Types ────────────────────────────────────────────────────────────────
using ::HANDLE;
using ::DWORD;
using ::BOOL;
using ::LARGE_INTEGER;
using ::FILETIME;
using ::SRWLOCK;
using ::CONDITION_VARIABLE;

// ── Constants ────────────────────────────────────────────────────────────
constexpr auto    M_TRUE                              = TRUE;
constexpr auto    M_FALSE                             = FALSE;
constexpr auto    M_INFINITE                          = INFINITE;
constexpr auto    M_ERROR_TIMEOUT                     = ERROR_TIMEOUT;
inline const auto M_INVALID_HANDLE_VALUE              = INVALID_HANDLE_VALUE;
constexpr auto    M_WAIT_FAILED                       = WAIT_FAILED;
constexpr auto    M_STD_INPUT_HANDLE                  = STD_INPUT_HANDLE;
constexpr auto    M_STD_OUTPUT_HANDLE                 = STD_OUTPUT_HANDLE;
constexpr auto    M_STD_ERROR_HANDLE                  = STD_ERROR_HANDLE;
constexpr auto    M_STACK_SIZE_PARAM_IS_A_RESERVATION = STACK_SIZE_PARAM_IS_A_RESERVATION;
constexpr auto    M_CP_UTF8                           = CP_UTF8;

// ── Error ────────────────────────────────────────────────────────────────
using ::GetLastError;

inline constexpr auto ERROR_FILE_NOT_FOUND             = _ERROR_FILE_NOT_FOUND;
inline constexpr auto ERROR_PATH_NOT_FOUND             = _ERROR_PATH_NOT_FOUND;
inline constexpr auto ERROR_ACCESS_DENIED              = _ERROR_ACCESS_DENIED;
inline constexpr auto ERROR_CONNECTION_REFUSED         = _ERROR_CONNECTION_REFUSED;
inline constexpr auto ERROR_CONNECTION_ABORTED         = _ERROR_CONNECTION_ABORTED;
inline constexpr auto ERROR_NETNAME_DELETED            = _ERROR_NETNAME_DELETED;
inline constexpr auto ERROR_HOST_UNREACHABLE           = _ERROR_HOST_UNREACHABLE;
inline constexpr auto ERROR_NETWORK_UNREACHABLE        = _ERROR_NETWORK_UNREACHABLE;
inline constexpr auto ERROR_ADDRESS_ALREADY_ASSOCIATED = _ERROR_ADDRESS_ALREADY_ASSOCIATED;
inline constexpr auto ERROR_BROKEN_PIPE                = _ERROR_BROKEN_PIPE;
inline constexpr auto ERROR_NO_DATA                    = _ERROR_NO_DATA;
inline constexpr auto ERROR_FILE_EXISTS                = _ERROR_FILE_EXISTS;
inline constexpr auto ERROR_ALREADY_EXISTS             = _ERROR_ALREADY_EXISTS;
inline constexpr auto WAIT_TIMEOUT                     = _WAIT_TIMEOUT;
inline constexpr auto ERROR_SEM_TIMEOUT                = _ERROR_SEM_TIMEOUT;
inline constexpr auto ERROR_INVALID_PARAMETER          = _ERROR_INVALID_PARAMETER;
inline constexpr auto ERROR_INVALID_DATA               = _ERROR_INVALID_DATA;
inline constexpr auto ERROR_DIR_NOT_EMPTY              = _ERROR_DIR_NOT_EMPTY;
inline constexpr auto ERROR_DISK_FULL                  = _ERROR_DISK_FULL;
inline constexpr auto ERROR_SEEK                       = _ERROR_SEEK;
inline constexpr auto ERROR_NOT_READY                  = _ERROR_NOT_READY;
inline constexpr auto ERROR_BUSY                       = _ERROR_BUSY;
inline constexpr auto ERROR_POSSIBLE_DEADLOCK          = _ERROR_POSSIBLE_DEADLOCK;
inline constexpr auto ERROR_NOT_SAME_DEVICE            = _ERROR_NOT_SAME_DEVICE;
inline constexpr auto ERROR_TOO_MANY_LINKS             = _ERROR_TOO_MANY_LINKS;
inline constexpr auto ERROR_FILENAME_EXCED_RANGE       = _ERROR_FILENAME_EXCED_RANGE;
inline constexpr auto ERROR_NOT_ENOUGH_MEMORY          = _ERROR_NOT_ENOUGH_MEMORY;
inline constexpr auto ERROR_OUTOFMEMORY                = _ERROR_OUTOFMEMORY;
inline constexpr auto ERROR_NOT_SUPPORTED              = _ERROR_NOT_SUPPORTED;
inline constexpr auto ERROR_CALL_NOT_IMPLEMENTED       = _ERROR_CALL_NOT_IMPLEMENTED;
inline constexpr auto ERROR_IO_PENDING                 = _ERROR_IO_PENDING;

// ── Synchronization — SRWLock ────────────────────────────────────────────
using ::AcquireSRWLockExclusive;
using ::TryAcquireSRWLockExclusive;
using ::ReleaseSRWLockExclusive;

// ── Synchronization — Condition Variable ─────────────────────────────────
using ::SleepConditionVariableSRW;
using ::WakeConditionVariable;
using ::WakeAllConditionVariable;

// ── Synchronization — WaitOnAddress (futex) ──────────────────────────────
using ::WaitOnAddress;
using ::WakeByAddressSingle;
using ::WakeByAddressAll;

// ── Time ─────────────────────────────────────────────────────────────────
using ::QueryPerformanceFrequency;
using ::QueryPerformanceCounter;
using ::GetSystemTimeAsFileTime;

inline auto gmtime_utc(::time_t secs) noexcept -> ::tm {
    ::tm out {};
    ::gmtime_s(&out, &secs);
    return out;
}

// ── Threading ────────────────────────────────────────────────────────────
using ::CreateThread;
using ::WaitForSingleObject;
using ::CloseHandle;
using ::GetCurrentThreadId;
using ::Sleep;
using ::SwitchToThread;
using ::GetCurrentThread;
using ::SetThreadDescription;

// ── IO ───────────────────────────────────────────────────────────────────
inline constexpr auto SEEK_SET = _RSTD_SEEK_SET;
inline constexpr auto SEEK_CUR = _RSTD_SEEK_CUR;
inline constexpr auto SEEK_END = _RSTD_SEEK_END;
using ::GetStdHandle;
using ::WriteFile;
using ::ReadFile;
using ::GetConsoleMode;
using ::_isatty;
using ::_fileno;

// ── String ───────────────────────────────────────────────────────────────
using ::MultiByteToWideChar;

// ── Process ──────────────────────────────────────────────────────────────
using ::RaiseFailFastException;
using ::ExitProcess;
using ::GetCurrentProcessId;
using ::GetEnvironmentVariableA;
using ::SetEnvironmentVariableA;

using ::_putenv_s;
inline auto get_errno() noexcept -> int& { return *::_errno(); }

} // namespace rstd::sys::libc
#endif

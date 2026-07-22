module;
#include <rstd/macro.hpp>
export module rstd:io.stdio;
export import :io.traits;
export import :sys.io.stdio;
export import :sys.sync.mutex;
import rstd.alloc;

namespace rstd::io
{

// ── Internal helpers ──────────────────────────────────────────────────────

namespace detail
{

// Global mutexes — inline avoids local-static guards (__cxa_guard_*) which
// are unavailable in -nostdlib++ builds.  Mutex is trivially destructible.
inline sys::sync::mutex::Mutex g_stdout_mutex = sys::sync::mutex::Mutex::make();
inline sys::sync::mutex::Mutex g_stdin_mutex  = sys::sync::mutex::Mutex::make();

inline auto& stdout_mutex() noexcept {
    return g_stdout_mutex;
}
inline auto& stdin_mutex() noexcept {
    return g_stdin_mutex;
}

// RAII lock guard over sys::sync::mutex::Mutex.
struct ScopedLock {
    sys::sync::mutex::Mutex& m;
    explicit ScopedLock(sys::sync::mutex::Mutex& m) noexcept: m(m) { m.lock(); }
    ~ScopedLock() noexcept { m.unlock(); }
    ScopedLock(const ScopedLock&) = delete;
};

// fmt::Write adapter that writes directly to a file descriptor.
// Used to avoid a String heap allocation inside print_fmt/eprint_fmt.
struct FdWriter {
    int               fd;
    io::Result<empty> result { Ok(empty {}) };
};

} // namespace detail

} // namespace rstd::io

namespace rstd
{
template<>
struct Impl<fmt::Write, io::detail::FdWriter> : ImplBase<io::detail::FdWriter> {
    auto write_str(const u8* p, usize len) -> bool {
        auto& self = this->self();
        if (self.result.is_err()) return false;
        while (len > 0) {
            auto res = sys::io::stdio::write_fd(self.fd, p, len);
            if (res.is_err()) {
                self.result = Err(res.unwrap_err_unchecked());
                return false;
            }
            usize n = res.unwrap_unchecked();
            if (n == 0) {
                self.result = Err(io::error::Error_WRITE_ALL_EOF);
                return false;
            }
            p += n;
            len -= n;
        }
        return true;
    }
};
} // namespace rstd

namespace rstd::io
{

// ── Stdout ────────────────────────────────────────────────────────────────

/// A locked reference to the stdout stream, providing exclusive write access.
export struct StdoutLock {
    detail::ScopedLock guard { detail::stdout_mutex() };
};

/// Handle to the standard output stream.
/// Thread-safe: concurrent writes are serialised by an internal mutex.
export struct Stdout {
    USE_TRAIT_WIN(Stdout)

    /// Acquire a lock and return a handle whose writes are atomic.
    auto lock() -> StdoutLock { return {}; }
};

/// Constructs a new handle to the standard output of the current process.
export inline auto stdout() noexcept -> Stdout {
    return {};
}

// ── Stderr ────────────────────────────────────────────────────────────────

/// Handle to the standard error stream.
/// Intentionally lock-free so it is usable from panic handlers.
export struct Stderr {};

/// Constructs a new handle to the standard error of the current process.
export inline auto stderr() noexcept -> Stderr {
    return {};
}

// ── Stdin ─────────────────────────────────────────────────────────────────

/// A locked reference to the stdin stream, providing exclusive read access.
export struct StdinLock {
    detail::ScopedLock guard { detail::stdin_mutex() };
};

/// Handle to the standard input stream.
export struct Stdin {
    USE_TRAIT_WIN(Stdin)

    /// Locks stdin and returns a guard providing exclusive read access.
    auto lock() -> StdinLock { return {}; }
};

/// Constructs a new handle to the standard input of the current process.
export inline auto stdin() noexcept -> Stdin {
    return {};
}

// ── Free formatting functions ─────────────────────────────────────────────

/// Format `args` and write to stdout (with lock).
export inline auto print_fmt(fmt::Arguments args) -> Result<empty> {
    detail::ScopedLock guard(detail::stdout_mutex());
    detail::FdWriter   w { 1 };
    fmt::Formatter     f(w);
    f.write_fmt(args);
    return rstd::move(w.result);
}

/// Format `args` and write to stderr (no lock — panic-safe).
export inline auto eprint_fmt(fmt::Arguments args) -> Result<empty> {
    detail::FdWriter w { 2 };
    fmt::Formatter   f(w);
    f.write_fmt(args);
    return rstd::move(w.result);
}

// ── print / println / eprint / eprintln ──────────────────────────────────
// Rust-style call-syntax structs using CTAD deduction guides.

/// Prints formatted text to stdout.
/// \tparam Args The types of format arguments.
export template<typename... Args>
struct print {
    print(fmt::format_string<Args...> fmt_str, Args&&... args) {
        fmt::Argument arg_array[] = { fmt::Argument::make(args)... };
        print_fmt({ fmt_str.data(), fmt_str.size(), arg_array, sizeof...(Args) });
    }
};
// Zero-arg specialisation avoids zero-length array.
template<>
struct print<> {
    print(fmt::format_string<> fmt_str) {
        print_fmt({ fmt_str.data(), fmt_str.size(), nullptr, 0 });
    }
};
template<typename... Args>
print(fmt::format_string<Args...>, Args&&...) -> print<Args...>;

/// Prints formatted text to stdout, followed by a newline.
/// \tparam Args The types of format arguments.
export template<typename... Args>
struct println {
    println(fmt::format_string<Args...> fmt_str, Args&&... args) {
        fmt::Argument arg_array[] = { fmt::Argument::make(args)... };
        print_fmt({ fmt_str.data(), fmt_str.size(), arg_array, sizeof...(Args) });
        print_fmt({ (const u8*)"\n", 1, nullptr, 0 });
    }
};
template<>
struct println<> {
    println() { print_fmt({ (const u8*)"\n", 1, nullptr, 0 }); }
    explicit println(fmt::format_string<> fmt_str) {
        print_fmt({ fmt_str.data(), fmt_str.size(), nullptr, 0 });
        print_fmt({ (const u8*)"\n", 1, nullptr, 0 });
    }
};
template<typename... Args>
println(fmt::format_string<Args...>, Args&&...) -> println<Args...>;

/// Prints formatted text to stderr.
/// \tparam Args The types of format arguments.
export template<typename... Args>
struct eprint {
    eprint(fmt::format_string<Args...> fmt_str, Args&&... args) {
        fmt::Argument arg_array[] = { fmt::Argument::make(args)... };
        eprint_fmt({ fmt_str.data(), fmt_str.size(), arg_array, sizeof...(Args) });
    }
};
template<>
struct eprint<> {
    eprint(fmt::format_string<> fmt_str) {
        eprint_fmt({ fmt_str.data(), fmt_str.size(), nullptr, 0 });
    }
};
template<typename... Args>
eprint(fmt::format_string<Args...>, Args&&...) -> eprint<Args...>;

/// Prints formatted text to stderr, followed by a newline.
/// \tparam Args The types of format arguments.
export template<typename... Args>
struct eprintln {
    eprintln(fmt::format_string<Args...> fmt_str, Args&&... args) {
        fmt::Argument arg_array[] = { fmt::Argument::make(args)... };
        eprint_fmt({ fmt_str.data(), fmt_str.size(), arg_array, sizeof...(Args) });
        eprint_fmt({ (const u8*)"\n", 1, nullptr, 0 });
    }
};
template<>
struct eprintln<> {
    eprintln() { eprint_fmt({ (const u8*)"\n", 1, nullptr, 0 }); }
    explicit eprintln(fmt::format_string<> fmt_str) {
        eprint_fmt({ fmt_str.data(), fmt_str.size(), nullptr, 0 });
        eprint_fmt({ (const u8*)"\n", 1, nullptr, 0 });
    }
};
template<typename... Args>
eprintln(fmt::format_string<Args...>, Args&&...) -> eprintln<Args...>;

} // namespace rstd::io

// ── Impl<io::Write, Stdout/Stderr/StdoutLock/StdinLock> ──────────────────
namespace rstd
{

template<>
struct Impl<io::Write, io::Stdout> : ImplBase<io::Stdout> {
    auto write(const u8* buf, usize len) -> io::Result<usize> {
        io::detail::ScopedLock guard(io::detail::stdout_mutex());
        return sys::io::stdio::write_fd(1, buf, len);
    }
    auto flush() -> io::Result<empty> { return Ok(empty {}); }
};

template<>
struct Impl<io::Write, io::StdoutLock> : ImplBase<io::StdoutLock> {
    auto write(const u8* buf, usize len) -> io::Result<usize> {
        // Mutex is already held by the StdoutLock.
        return sys::io::stdio::write_fd(1, buf, len);
    }
    auto flush() -> io::Result<empty> { return Ok(empty {}); }
};

template<>
struct Impl<io::Write, io::Stderr> : ImplBase<io::Stderr> {
    auto write(const u8* buf, usize len) -> io::Result<usize> {
        return sys::io::stdio::write_fd(2, buf, len);
    }
    auto flush() -> io::Result<empty> { return Ok(empty {}); }
};

template<>
struct Impl<io::Read, io::Stdin> : ImplBase<io::Stdin> {
    auto read(u8* buf, usize len) -> io::Result<usize> {
        io::detail::ScopedLock guard(io::detail::stdin_mutex());
        return sys::io::stdio::read_fd(0, buf, len);
    }
};

template<>
struct Impl<io::Read, io::StdinLock> : ImplBase<io::StdinLock> {
    auto read(u8* buf, usize len) -> io::Result<usize> {
        return sys::io::stdio::read_fd(0, buf, len);
    }
};

} // namespace rstd

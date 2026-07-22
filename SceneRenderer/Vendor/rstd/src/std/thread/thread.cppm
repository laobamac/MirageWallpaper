module;
#include <rstd/macro.hpp>
export module rstd:thread.thread;
export import :thread.id;
export import :thread.main_thread;
export import :sys.sync.thread_parking;
export import :ffi;
import :forward;
import rstd.alloc;

using alloc::sync::Arc;
using alloc::sync::ArcRaw;
using rstd::boxed::Box;
using rstd::string::String;
using rstd::sys::sync::thread_parking::Parker;

namespace rstd
{

namespace sys::thread::thread_name_string
{

// Like a `String` it's guaranteed UTF-8 and like a `CString` it's null terminated.
// (C++ side we assume std::string is UTF-8 by convention, and CString validates no interior '\0')
class ThreadNameString {
public:
    ffi::CString inner;

    USE_TRAIT_WIN(ThreadNameString)

    explicit ThreadNameString(ffi::CString c): inner(rstd::move(c)) {}

    ThreadNameString(Self&&) noexcept            = default;
    ThreadNameString& operator=(Self&&) noexcept = default;

    static auto from(String s) -> Self {
        return Self { ffi::CString::make(rstd::move(s))
                          .expect("thread name may not contain interior null bytes") };
    }

    auto as_cstr() const noexcept { return inner.as_ref(); }

    auto as_str() const noexcept -> ref<str> { return inner.to_bytes(); }
};

} // namespace sys::thread::thread_name_string

using rstd_alloc::string::String;
using sys::thread::thread_name_string::ThreadNameString;

template<>
struct Impl<convert::From<String>, ThreadNameString>
    : LinkClassMethod<convert::From<String>, ThreadNameString> {};

template<>
struct Impl<clone::Clone, ThreadNameString> : DefaultInImpl<clone::Clone, ThreadNameString> {
    auto clone() const -> ThreadNameString {
        return ThreadNameString { as<clone::Clone>(this->self().inner).clone() };
    }
};

namespace thread
{

/// The internal representation of a `Thread` handle
/// We explicitly set the alignment for our guarantee in Thread::into_raw.
struct alignas(8) Inner {
    Option<ThreadNameString> name;
    ThreadId                 id;
    Parker                   parker;
};

/// A handle to a thread.
///
/// Threads are represented by the `Thread` type. Each thread has a unique `ThreadId`,
/// an optional name, and a parker for blocking/unblocking.
export class Thread : public DefaultInClass<Thread, clone::Clone> {
    Arc<Inner> inner;

    explicit Thread(Arc<Inner> inner): inner(rstd::move(inner)) {}

public:
    USE_TRAIT_WIN(Thread)

    Thread(const Thread& other) noexcept: inner(as<clone::Clone>(other.inner).clone()) {}
    Thread(Thread&&) noexcept = default;
    Thread& operator=(const Thread& other) noexcept {
        if (this != &other) {
            inner = as<clone::Clone>(other.inner).clone();
        }
        return *this;
    }
    Thread& operator=(Thread&&) noexcept = default;

    auto clone() const -> Thread { return Thread { as<clone::Clone>(inner).clone() }; }

    /// Creates a new thread handle.
    static auto make(ThreadId id, Option<String> name) -> Thread {
        // Convert String to ThreadNameString if present
        auto thread_name = name.map([](String s) {
            return ThreadNameString::from(rstd::move(s));
        });

        auto arc = Arc<Inner>::make_uninit();
        auto ptr = (void*)arc->as_ptr();

        ::new (ptr) Inner { .name { rstd::move(thread_name) }, .id { id }, .parker {} };

        return Thread { arc.assume_init() };
    }

    /// Gets the thread's unique identifier.
    auto id() const noexcept -> ThreadId { return inner->id; }

    /// Gets the thread's name as a string reference if available.
    /// Returns None if no name was set.
    auto name() const -> Option<ThreadNameString> {
        if (! inner) {
            rstd::panic { "Thread::name() called with null Arc" };
        }
        if (auto& name = inner->name; name) {
            return name.clone();
        } else if (main_thread::get() == Some(id())) {
            return Some(ThreadNameString { ffi::CString::from_raw_parts("main") });
        } else {
            return None();
        }
    }

    /// Atomically makes the handle's token available if it is not already.
    ///
    /// Every thread is equipped with some basic low-level blocking support, via
    /// the park() function and the unpark() method. These can be used as a more
    /// CPU-efficient implementation of a spinlock.
    void unpark() const noexcept { inner->parker.unpark(); }

    /// Like the public park, but callable on any handle.
    ///
    /// # Safety
    /// May only be called from the thread to which this handle belongs.
    void park() const { inner->parker.park(); }

    /// Like the public park_timeout, but callable on any handle.
    ///
    /// # Safety
    /// May only be called from the thread to which this handle belongs.
    void park_timeout(rstd::time::Duration timeout) const { inner->parker.park_timeout(timeout); }

    /// Gets the C string representation of the thread name, if available.
    auto cname() const noexcept -> Option<ref<ffi::CStr>> {
        if (inner->name.is_some()) {
            return Some(inner->name.as_ref().unwrap().as_cstr());
        }
        if (main_thread::get() == Some(id())) {
            return Some(ffi::CStr::from_ptr("main"));
        }
        return None();
    }

    auto into_raw() { return inner.into_raw().into_raw(); }

    static auto from_raw(voidp p) -> Thread {
        return Thread { Arc<Inner>::from_raw(ArcRaw<Inner>::from_raw(p)) };
    }

    auto set_current() && -> Result<empty, Thread>;
};

} // namespace thread

namespace thread
{

/// Initialization payload for spawning a new thread, containing the handle and start function.
export struct ThreadInit {
    Thread handle;
    // cppstd::move_only_function<void()> start;
    Box<dyn<FnMut<void()>>> start;

    void init() const;
};

static_assert(mtp::drop<ThreadInit>);

} // namespace thread
} // namespace rstd

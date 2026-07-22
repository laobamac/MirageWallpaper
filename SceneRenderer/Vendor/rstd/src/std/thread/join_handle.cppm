module;
#include <rstd/macro.hpp>
export module rstd:thread.join_handle;
export import :thread.lifecycle;
export import :thread.thread;
export import rstd.core;

namespace rstd::thread
{

/// An owned handle to a thread, which joins the thread on drop.
/// \tparam T The return type of the thread's closure.
template<typename T>
class JoinHandle {
    lifecycle::JoinInner<T> inner;

    JoinHandle(lifecycle::JoinInner<T> inner): inner(rstd::move(inner)) {}

public:
    USE_TRAIT_WIN(JoinHandle)

    using ret_t = mtp::void_empty_t<T>;

    JoinHandle() = delete;

    static auto make(lifecycle::JoinInner<T> inner) -> Self { return Self { rstd::move(inner) }; }

    /// Extracts a handle to the underlying thread.
    auto thread() const -> Thread { return inner.thread(); }

    /// Waits for the associated thread to finish and returns its result.
    auto join() && -> Result<ret_t> { return rstd::move(*this).inner.join(); }

    /// Checks whether the associated thread has finished running.
    auto is_finished() const -> bool { return inner.is_finished(); }

    friend struct lifecycle::JoinInner<T>;
};

} // namespace rstd::thread

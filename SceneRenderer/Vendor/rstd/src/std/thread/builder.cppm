module;
#include <rstd/macro.hpp>
export module rstd:thread.builder;
import :forward;
export import :thread.join_handle;
export import :thread.lifecycle;
export import :sys;
export import rstd.core;

using rstd_alloc::string::String;
namespace imp = rstd::sys::thread;

namespace rstd::thread::builder
{

/// Thread factory, which can be used in order to configure the properties of
/// a new thread.
export struct Builder {
    struct Fields {
        /// A name for the thread-to-be, for identification in panic messages
        Option<String> name;
        /// The size of the stack for the spawned thread in bytes
        Option<usize> stack_size;
        /// Skip running and inheriting the thread spawn hooks
        bool no_hooks;
    };

    Fields d;

    USE_TRAIT_WIN(Builder)

    /// Creates a new Builder with default configuration.
    static auto make() -> Builder {
        return { .d = {
                     .name {},
                     .stack_size {},
                     .no_hooks = false,
                 } };
    }

    /// Sets the name of the thread-to-be.
    /// \param name The thread name for identification in panic messages.
    auto name(String name) -> Builder& {
        d.name = Some(rstd::move(name));
        return *this;
    }

    /// Sets the size of the stack for the new thread.
    /// \param size The stack size in bytes.
    auto stack_size(usize size) -> Builder& {
        d.stack_size = Some(size);
        return *this;
    }

    /// Disables running and inheriting thread spawn hooks.
    auto no_hooks() -> Builder& {
        d.no_hooks = true;
        return *this;
    }

    /// Spawns a new thread by taking ownership of the Builder and calling `f`.
    /// \tparam F The callable type for the thread entry point.
    /// \param f The closure or function to execute in the new thread.
    /// \return A JoinHandle on success, or an I/O error on failure.
    template<typename F>
    auto spawn(F&& f) -> io::Result<JoinHandle<mtp::invoke_result_t<F>>>
    // requires Impled<mtp::rm_cvf<F>, FnOnce<void()>> &&
    // mtp::same_as<mtp::rm_cvf<F>, Box<dyn<FnOnce<void()>>>
    {
        auto stack_size = d.stack_size.unwrap_or(0);

        auto inner =
            lifecycle::spawn_unchecked(rstd::move(d.name), stack_size, None(), rstd::forward<F>(f));

        if (inner.is_ok()) {
            return Ok(JoinHandle<mtp::invoke_result_t<F>>::make(inner.unwrap_unchecked()));
        } else {
            return Err(inner.unwrap_err_unchecked());
        }
    }
};

} // namespace rstd::thread::builder
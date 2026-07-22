module;
#include <rstd/macro.hpp>
export module rstd:thread.lifecycle;
export import :thread.thread;
export import :thread.scoped;
export import :thread.current;
export import :thread.forward;
export import :sys;
export import rstd.core;
export import rstd.alloc;

using alloc::sync::Arc;
using rstd::thread::scoped::ScopeData;
namespace imp = rstd::sys::thread;

namespace rstd::thread::lifecycle
{

template<typename T>
struct Packet {
    Option<Arc<ScopeData>> scope;
    Option<Result<T>>      result;
};

template<>
struct Packet<void> {
    Option<Arc<ScopeData>> scope;
    Option<Result<empty>>  result;
};

template<typename T>
struct JoinInner {
    using ret_t = mtp::void_empty_t<T>;

    Thread         thread_;
    Arc<Packet<T>> packet;
    imp::Thread    native;

    USE_TRAIT_WIN(JoinInner)

    auto is_finished() const -> bool { return packet->strong_count() == 1; }

    auto thread() const -> Thread const& { return thread_; }

    auto join() && -> Result<ret_t> {
        native.join();

        return packet.get_mut()
            .expect("threads should not terminate unexpectedly")
            ->result.take()
            .unwrap();
    }
};

template<typename F>
auto spawn_unchecked(Option<String>         name,
                     usize                  stack_size,
                     Option<Arc<ScopeData>> scope_data,
                     F&&                    f) -> io::Result<JoinInner<mtp::invoke_result_t<F>>>
// requires Impled<F, FnOnce<void()>> && mtp::spec_of<mtp::rm_cvf<F>,
// rstd_alloc::boxed::Box>
{
    using namespace rstd_alloc::boxed;
    using ret_t = mtp::invoke_result_t<F>;

    auto id     = ThreadId::make();
    auto thread = Thread::make(id, rstd::move(name));

    auto packet = Arc<Packet<ret_t>>::make_uninit();
    auto ptr    = (void*)packet->as_ptr();

    ::new (ptr) Packet<ret_t> { .scope = rstd::move(scope_data), .result = None() };

    auto my_packet    = packet.assume_init();
    auto their_packet = as<clone::Clone>(my_packet).clone();

    auto start = [f = rstd::move(f), p = as<clone::Clone>(my_packet).clone()]() mutable {
        if constexpr (mtp::same_as<ret_t, void>) {
            f();
            p->result = option::Some(Ok<empty, int>(empty {}));
        } else {
            p->result = option::Some(Ok<ret_t, int>(rstd::move(f)()));
        }
    };

    auto&& init = Box<ThreadInit>::make(
        ThreadInit { .handle = as<clone::Clone>(thread).clone(),
                     .start  = Box<dyn<FnMut<void()>>>::make(rstd::move(start)) });

    return imp::Thread::make(stack_size, rstd::move(init)).map([&](auto&& native) {
        return JoinInner<ret_t> {
            .thread_ = rstd::move(thread),
            .packet  = rstd::move(their_packet),
            .native  = rstd::move(native),
        };
    });
}

} // namespace rstd::thread::lifecycle

namespace rstd::thread
{
void ThreadInit::init() const {
    if (set_current(as<clone::Clone>(handle).clone()).is_err()) {
        panic { "current thread handle already set during thread spawn" };
    }

    if (auto name = handle.cname()) {
        imp::Thread::set_name(*name);
    }
}
} // namespace rstd::thread

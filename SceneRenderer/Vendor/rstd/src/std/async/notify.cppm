module;
#include <rstd/macro.hpp>

export module rstd:async.notify;
export import :async.forward;
export import :async.reactor;
export import :io;
import :sys.fd;
import :sys.io;
import :sys.libc;
import :sync;

using namespace rstd;
namespace libc = rstd::sys::libc;

namespace rstd::async
{

export class NotifyHandle;
export class NotifyFuture;

struct NotifyState {
    sys::fd::OwnedFd read_fd;
    sys::fd::OwnedFd write_fd;
    Registration     registration;

    NotifyState(sys::fd::OwnedFd read_fd, sys::fd::OwnedFd write_fd, Registration registration)
        : read_fd(rstd::move(read_fd)),
          write_fd(rstd::move(write_fd)),
          registration(rstd::move(registration)) {}

    NotifyState(const NotifyState&)                        = delete;
    auto operator=(const NotifyState&) -> NotifyState&     = delete;
    NotifyState(NotifyState&&) noexcept                    = default;
    auto operator=(NotifyState&&) noexcept -> NotifyState& = default;

    static auto last_os_error() noexcept -> io::Error {
        return io::Error::from_raw_os_error(sys::io::last_os_error());
    }

    auto drain() -> io::Result<bool> {
#if RSTD_OS_LINUX
        bool any = false;
        u8   buf[64] {};
        for (;;) {
            auto n = libc::read(read_fd.as_raw_fd(), buf, sizeof(buf));
            if (n > 0) {
                any = true;
                continue;
            }
            if (n == 0) {
                return Ok(any);
            }

            auto err = libc::get_errno();
            if (err == libc::EINTR) {
                continue;
            }
            if (err == libc::EAGAIN || err == libc::EWOULDBLOCK) {
                return Ok(any);
            }
            return Err(io::Error::from_raw_os_error(err));
        }
#else
        return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::Unsupported }));
#endif
    }

    auto notify() -> io::Result<empty> {
#if RSTD_OS_LINUX
        u8 byte = 1;
        for (;;) {
            auto n = libc::write(write_fd.as_raw_fd(), &byte, 1);
            if (n == 1) {
                return Ok(empty {});
            }

            auto err = libc::get_errno();
            if (err == libc::EINTR) {
                continue;
            }
            if (err == libc::EAGAIN || err == libc::EWOULDBLOCK) {
                return Ok(empty {});
            }
            return Err(io::Error::from_raw_os_error(err));
        }
#else
        return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::Unsupported }));
#endif
    }
};

export class Notify {
    sync::Arc<NotifyState> m_state;

    explicit Notify(sync::Arc<NotifyState> state): m_state(rstd::move(state)) {}

public:
    Notify(const Notify&)                        = delete;
    auto operator=(const Notify&) -> Notify&     = delete;
    Notify(Notify&&) noexcept                    = default;
    auto operator=(Notify&&) noexcept -> Notify& = default;
    ~Notify()                                    = default;

    auto clone() const -> Notify { return Notify { m_state.clone() }; }

    static auto make() -> io::Result<Notify> {
#if RSTD_OS_LINUX
        int fds[2] {};
        if (libc::pipe2(fds, libc::O_NONBLOCK | libc::O_CLOEXEC) < 0) {
            return Err(NotifyState::last_os_error());
        }

        auto read_fd  = sys::fd::OwnedFd::from_raw_fd(fds[0]);
        auto write_fd = sys::fd::OwnedFd::from_raw_fd(fds[1]);

        auto registration = Registration::register_fd(read_fd.as_raw_fd());
        if (registration.is_err()) {
            return Err(rstd::move(registration).unwrap_err_unchecked());
        }

        auto state = sync::Arc<NotifyState>::make(
            rstd::move(read_fd), rstd::move(write_fd), rstd::move(registration).unwrap_unchecked());
        return Ok(Notify { rstd::move(state) });
#else
        return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::Unsupported }));
#endif
    }

    auto notifier() const -> NotifyHandle;
    auto notified() const -> NotifyFuture;
};

export class NotifyHandle {
    sync::Arc<NotifyState> m_state;

    explicit NotifyHandle(sync::Arc<NotifyState> state): m_state(rstd::move(state)) {}

    friend class Notify;

public:
    NotifyHandle(const NotifyHandle&)                        = delete;
    auto operator=(const NotifyHandle&) -> NotifyHandle&     = delete;
    NotifyHandle(NotifyHandle&&) noexcept                    = default;
    auto operator=(NotifyHandle&&) noexcept -> NotifyHandle& = default;
    ~NotifyHandle()                                          = default;

    auto clone() const -> NotifyHandle { return NotifyHandle { m_state.clone() }; }

    auto notify() const -> io::Result<empty> {
        if (! m_state) {
            return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::NotConnected }));
        }
        return m_state->notify();
    }
};

export class NotifyFuture {
    sync::Arc<NotifyState> m_state;
    ReadinessFuture        m_readiness;
    Option<ReadyEvent>     m_event;
    bool                   m_completed { false };

    explicit NotifyFuture(sync::Arc<NotifyState> state)
        : m_state(rstd::move(state)),
          m_readiness(m_state->registration, Interest::readable()),
          m_event(None()) {}

    friend class Notify;

public:
    using Output = io::Result<empty>;

    NotifyFuture(const NotifyFuture&)                        = delete;
    auto operator=(const NotifyFuture&) -> NotifyFuture&     = delete;
    NotifyFuture(NotifyFuture&&) noexcept                    = default;
    auto operator=(NotifyFuture&&) noexcept -> NotifyFuture& = default;
    ~NotifyFuture()                                          = default;

    auto poll(mut_ref<NotifyFuture> self, task::Context& cx) -> task::Poll<Output> {
        auto& future = *self;
        if (future.m_completed) {
            rstd::panic { "async::NotifyFuture polled after completion" };
        }

        for (;;) {
            auto drained = future.m_state->drain();
            if (drained.is_err()) {
                future.m_completed = true;
                return task::Poll<Output>::Ready(Err(rstd::move(drained).unwrap_err_unchecked()));
            }
            if (rstd::move(drained).unwrap_unchecked()) {
                if (future.m_event.is_some()) {
                    future.m_state->registration.clear_readiness(
                        future.m_event.take().unwrap_unchecked());
                }
                future.m_completed = true;
                return task::Poll<Output>::Ready(Ok(empty {}));
            }

            if (future.m_event.is_some()) {
                future.m_state->registration.clear_readiness(
                    future.m_event.take().unwrap_unchecked());
            }

            auto ready = future.m_readiness.poll(future::as_mut_ref(future.m_readiness), cx);
            if (ready.is_pending()) {
                return task::Poll<Output>::Pending();
            }

            auto ready_result = rstd::move(ready).take();
            if (ready_result.is_err()) {
                future.m_completed = true;
                return task::Poll<Output>::Ready(
                    Err(rstd::move(ready_result).unwrap_err_unchecked()));
            }

            future.m_event.insert(rstd::move(ready_result).unwrap_unchecked());
        }
    }
};

inline auto Notify::notifier() const -> NotifyHandle {
    return NotifyHandle { m_state.clone() };
}

inline auto Notify::notified() const -> NotifyFuture {
    return NotifyFuture { m_state.clone() };
}

} // namespace rstd::async

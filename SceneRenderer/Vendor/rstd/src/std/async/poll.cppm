module;
#include <rstd/macro.hpp>

export module rstd:async.poll;
export import :io.error;
export import :async.readiness;
export import :time;
export import rstd.core;
import :sys.fd;
import :sys.libc;
import :sync;
import rstd.alloc;

using namespace rstd;
using ::alloc::vec::Vec;
namespace libc = rstd::sys::libc;

namespace rstd::async
{

inline constexpr u64 POLL_WAKE_KEY  = 0;
inline constexpr u64 POLL_TIMER_KEY = u64(-1);

export enum class PollStateKind {
    Active,
    Waiting,
    Draining,
    Closed,
};

export enum class PollTimeout {
    Immediate,
    Infinite,
};

export enum class PollEventKind {
    Wake,
    Readiness,
    Completion,
    Timer,
    BackendError,
};

export enum class PollCapability : u8 {
    Readiness  = 1,
    Completion = 2,
    Timer      = 4,
    Wake       = 8,
};

export class PollCapabilities {
    u8 m_bits {};

    explicit constexpr PollCapabilities(u8 bits) noexcept: m_bits(bits) {}

public:
    constexpr PollCapabilities() noexcept = default;

    static constexpr auto none() noexcept -> PollCapabilities { return PollCapabilities {}; }

    static constexpr auto of(PollCapability capability) noexcept -> PollCapabilities {
        return PollCapabilities { static_cast<u8>(capability) };
    }

    constexpr auto contains(PollCapability capability) const noexcept -> bool {
        auto bit = static_cast<u8>(capability);
        return (m_bits & bit) == bit;
    }

    friend constexpr auto operator|(PollCapabilities capabilities,
                                    PollCapability   capability) noexcept -> PollCapabilities {
        return PollCapabilities { u8(capabilities.m_bits | static_cast<u8>(capability)) };
    }
};

export enum class PollKeyKind {
    Registration,
    Operation,
    Timer,
};

export struct PollKey {
    PollKeyKind kind { PollKeyKind::Registration };
    u64         value {};

    constexpr auto is_valid() const noexcept -> bool {
        return value != POLL_WAKE_KEY && value != POLL_TIMER_KEY;
    }

    friend constexpr auto operator==(PollKey, PollKey) noexcept -> bool = default;
};

export enum class PollOperationKind {
    Read,
    Write,
};

export class PollOperation {
    PollOperationKind m_kind { PollOperationKind::Read };
    sys::fd::RawFd    m_fd { sys::fd::INVALID_RAW_FD };
    void*             m_mut_data { nullptr };
    const void*       m_const_data { nullptr };
    usize             m_len {};
    Option<u64>       m_offset {};
    u32               m_flags {};

public:
    static auto
    read(sys::fd::RawFd fd, void* data, usize len, Option<u64> offset = None(), u32 flags = 0)
        -> PollOperation {
        auto operation       = PollOperation {};
        operation.m_kind     = PollOperationKind::Read;
        operation.m_fd       = fd;
        operation.m_mut_data = data;
        operation.m_len      = len;
        operation.m_offset   = rstd::move(offset);
        operation.m_flags    = flags;
        return operation;
    }

    static auto write(sys::fd::RawFd fd,
                      const void*    data,
                      usize          len,
                      Option<u64>    offset = None(),
                      u32            flags  = 0) -> PollOperation {
        auto operation         = PollOperation {};
        operation.m_kind       = PollOperationKind::Write;
        operation.m_fd         = fd;
        operation.m_const_data = data;
        operation.m_len        = len;
        operation.m_offset     = rstd::move(offset);
        operation.m_flags      = flags;
        return operation;
    }

    auto kind() const noexcept -> PollOperationKind { return m_kind; }
    auto fd() const noexcept -> sys::fd::RawFd { return m_fd; }
    auto mutable_data() const noexcept -> void* { return m_mut_data; }
    auto const_data() const noexcept -> const void* { return m_const_data; }
    auto len() const noexcept -> usize { return m_len; }
    auto offset() const noexcept -> const Option<u64>& { return m_offset; }
    auto flags() const noexcept -> u32 { return m_flags; }
};

export class PollCompletion {
    isize             m_result {};
    u32               m_flags {};
    Option<io::Error> m_error {};

    PollCompletion(isize result, u32 flags, Option<io::Error> error)
        : m_result(result), m_flags(flags), m_error(rstd::move(error)) {}

public:
    static auto success(isize result, u32 flags = 0) -> PollCompletion {
        return PollCompletion { result, flags, None() };
    }

    static auto failure(io::Error error, u32 flags = 0) -> PollCompletion {
        return PollCompletion { 0, flags, Some(rstd::move(error)) };
    }

    auto is_error() const noexcept -> bool { return m_error.is_some(); }
    auto result() const noexcept -> isize { return m_result; }
    auto flags() const noexcept -> u32 { return m_flags; }
    auto take_error() -> io::Error { return m_error.take().unwrap_unchecked(); }
};

export class PollEventData {
    PollEventKind          m_kind { PollEventKind::Wake };
    PollKey                m_key {};
    Option<Ready>          m_readiness {};
    Option<PollCompletion> m_completion {};
    Option<io::Error>      m_backend_error {};

    explicit PollEventData(PollEventKind kind, PollKey key = {}): m_kind(kind), m_key(key) {}

public:
    PollEventData(): PollEventData(PollEventKind::Wake) {}

    static auto wake() -> PollEventData { return PollEventData {}; }

    static auto readiness(PollKey key, Ready ready) -> PollEventData {
        auto data        = PollEventData { PollEventKind::Readiness, key };
        data.m_readiness = Some(ready);
        return data;
    }

    static auto completion(PollKey key, PollCompletion completion) -> PollEventData {
        auto data         = PollEventData { PollEventKind::Completion, key };
        data.m_completion = Some(rstd::move(completion));
        return data;
    }

    static auto timer(PollKey key) -> PollEventData {
        return PollEventData { PollEventKind::Timer, key };
    }

    static auto backend_error(PollKey key, io::Error error) -> PollEventData {
        auto data            = PollEventData { PollEventKind::BackendError, key };
        data.m_backend_error = Some(rstd::move(error));
        return data;
    }

    auto kind() const noexcept -> PollEventKind { return m_kind; }
    auto key() const noexcept -> PollKey { return m_key; }
    auto readiness() const -> Ready { return *m_readiness; }
    auto take_completion() -> PollCompletion { return m_completion.take().unwrap_unchecked(); }
    auto has_backend_error() const noexcept -> bool { return m_backend_error.is_some(); }
    auto take_backend_error() -> io::Error { return m_backend_error.take().unwrap_unchecked(); }
};

export struct RawPollEventOwner;

export struct RawPollEventOwnerVTable {
    using CloneFn    = RawPollEventOwner (*)(voidp);
    using DispatchFn = void (*)(voidp, PollEventData);
    using DropFn     = void (*)(voidp);

    CloneFn    clone;
    DispatchFn dispatch;
    DropFn     drop;
};

export struct RawPollEventOwner {
    voidp                          data { nullptr };
    const RawPollEventOwnerVTable* vtable { nullptr };

    static constexpr auto from_raw_parts(voidp data, const RawPollEventOwnerVTable* vtable) noexcept
        -> RawPollEventOwner {
        return RawPollEventOwner { data, vtable };
    }
};

export class PollEventOwner {
    RawPollEventOwner m_raw {};

    explicit PollEventOwner(RawPollEventOwner raw): m_raw(raw) {}

public:
    PollEventOwner() noexcept                                = default;
    PollEventOwner(const PollEventOwner&)                    = delete;
    auto operator=(const PollEventOwner&) -> PollEventOwner& = delete;

    PollEventOwner(PollEventOwner&& other) noexcept: m_raw(rstd::exchange(other.m_raw, {})) {}

    auto operator=(PollEventOwner&& other) noexcept -> PollEventOwner& {
        if (this != &other) {
            reset();
            m_raw = rstd::exchange(other.m_raw, {});
        }
        return *this;
    }

    ~PollEventOwner() { reset(); }

    static auto from_raw(RawPollEventOwner raw) -> PollEventOwner { return PollEventOwner { raw }; }

    void reset() noexcept {
        if (m_raw.vtable != nullptr) {
            auto current = rstd::exchange(m_raw, {});
            current.vtable->drop(current.data);
        }
    }

    auto clone() const -> PollEventOwner {
        if (m_raw.vtable == nullptr) return PollEventOwner {};
        return PollEventOwner::from_raw(m_raw.vtable->clone(m_raw.data));
    }

    void dispatch(PollEventData data) const {
        if (m_raw.vtable != nullptr) {
            m_raw.vtable->dispatch(m_raw.data, rstd::move(data));
        }
    }

    explicit operator bool() const noexcept { return m_raw.vtable != nullptr; }
};

export class PollEvent {
    PollEventData          m_data;
    Option<PollEventOwner> m_owner;

    PollEvent(PollEventData data, Option<PollEventOwner> owner)
        : m_data(rstd::move(data)), m_owner(rstd::move(owner)) {}

public:
    PollEvent(): m_data(PollEventData::wake()), m_owner(None()) {}

    static auto wake() -> PollEvent { return PollEvent {}; }

    static auto owned(PollEventData data, PollEventOwner owner) -> PollEvent {
        return PollEvent { rstd::move(data), Some(rstd::move(owner)) };
    }

    PollEvent(const PollEvent&)                        = delete;
    auto operator=(const PollEvent&) -> PollEvent&     = delete;
    PollEvent(PollEvent&&) noexcept                    = default;
    auto operator=(PollEvent&&) noexcept -> PollEvent& = default;

    auto kind() const noexcept -> PollEventKind { return m_data.kind(); }
    auto key() const noexcept -> PollKey { return m_data.key(); }

    void dispatch() {
        if (m_owner.is_some()) {
            m_owner->dispatch(rstd::move(m_data));
        }
    }
};

export enum class PollCommandKind {
    RegisterSource,
    UpdateInterest,
    DeregisterSource,
    SubmitOperation,
    CancelOperation,
    ArmTimer,
    CancelTimer,
};

export class PollCommand {
    PollCommandKind        m_kind { PollCommandKind::RegisterSource };
    PollKey                m_key {};
    sys::fd::RawFd         m_fd { sys::fd::INVALID_RAW_FD };
    Interest               m_interest {};
    Option<PollOperation>  m_operation {};
    time::Instant          m_deadline {};
    Option<PollEventOwner> m_owner {};

    PollCommand(PollCommandKind kind, PollKey key, PollEventOwner owner)
        : m_kind(kind), m_key(key), m_owner(Some(rstd::move(owner))) {}

public:
    PollCommand(const PollCommand&)                        = delete;
    auto operator=(const PollCommand&) -> PollCommand&     = delete;
    PollCommand(PollCommand&&) noexcept                    = default;
    auto operator=(PollCommand&&) noexcept -> PollCommand& = default;

    static auto
    register_source(PollKey key, sys::fd::RawFd fd, Interest interest, PollEventOwner owner)
        -> PollCommand {
        auto command = PollCommand { PollCommandKind::RegisterSource, key, rstd::move(owner) };
        command.m_fd = fd;
        command.m_interest = interest;
        return command;
    }

    static auto update_interest(PollKey key, Interest interest, PollEventOwner owner)
        -> PollCommand {
        auto command = PollCommand { PollCommandKind::UpdateInterest, key, rstd::move(owner) };
        command.m_interest = interest;
        return command;
    }

    static auto deregister_source(PollKey key, PollEventOwner owner) -> PollCommand {
        return PollCommand { PollCommandKind::DeregisterSource, key, rstd::move(owner) };
    }

    static auto submit_operation(PollKey key, PollOperation operation, PollEventOwner owner)
        -> PollCommand {
        auto command = PollCommand { PollCommandKind::SubmitOperation, key, rstd::move(owner) };
        command.m_operation = Some(rstd::move(operation));
        return command;
    }

    static auto cancel_operation(PollKey key, PollEventOwner owner) -> PollCommand {
        return PollCommand { PollCommandKind::CancelOperation, key, rstd::move(owner) };
    }

    static auto arm_timer(PollKey key, time::Instant deadline, PollEventOwner owner)
        -> PollCommand {
        auto command       = PollCommand { PollCommandKind::ArmTimer, key, rstd::move(owner) };
        command.m_deadline = deadline;
        return command;
    }

    static auto cancel_timer(PollKey key, PollEventOwner owner) -> PollCommand {
        return PollCommand { PollCommandKind::CancelTimer, key, rstd::move(owner) };
    }

    auto kind() const noexcept -> PollCommandKind { return m_kind; }
    auto key() const noexcept -> PollKey { return m_key; }
    auto fd() const noexcept -> sys::fd::RawFd { return m_fd; }
    auto interest() const noexcept -> Interest { return m_interest; }
    auto operation() const -> const PollOperation& { return *m_operation; }
    auto take_operation() -> PollOperation { return m_operation.take().unwrap_unchecked(); }
    auto deadline() const noexcept -> time::Instant { return m_deadline; }
    auto owner() const -> const PollEventOwner& { return *m_owner; }
    auto take_owner() -> PollEventOwner { return m_owner.take().unwrap_unchecked(); }

    auto into_error_event(io::Error error) -> PollEvent {
        return PollEvent::owned(PollEventData::backend_error(m_key, rstd::move(error)),
                                take_owner());
    }
};

export enum class PollApplyStatus {
    Accepted,
    Rejected,
    Unsupported,
};

export class PollApplyResult {
    PollApplyStatus     m_status { PollApplyStatus::Accepted };
    Option<PollCommand> m_command {};
    Option<io::Error>   m_error {};

    explicit PollApplyResult(PollApplyStatus status): m_status(status) {}

    PollApplyResult(PollApplyStatus status, PollCommand command, io::Error error)
        : m_status(status),
          m_command(Some(rstd::move(command))),
          m_error(Some(rstd::move(error))) {}

public:
    static auto accepted() -> PollApplyResult {
        return PollApplyResult { PollApplyStatus::Accepted };
    }

    static auto rejected(PollCommand command, io::Error error) -> PollApplyResult {
        return PollApplyResult { PollApplyStatus::Rejected,
                                 rstd::move(command),
                                 rstd::move(error) };
    }

    static auto unsupported(PollCommand command) -> PollApplyResult {
        return PollApplyResult { PollApplyStatus::Unsupported,
                                 rstd::move(command),
                                 io::Error::from_kind(
                                     io::ErrorKind { io::ErrorKind::Unsupported }) };
    }

    auto status() const noexcept -> PollApplyStatus { return m_status; }
    auto take_command() -> PollCommand { return m_command.take().unwrap_unchecked(); }
    auto take_error() -> io::Error { return m_error.take().unwrap_unchecked(); }
};

export class PollBatch {
    Vec<PollEvent> m_events;

public:
    PollBatch(): m_events(Vec<PollEvent>::make()) {}

    auto is_empty() const noexcept -> bool { return m_events.is_empty(); }
    auto len() const noexcept -> usize { return m_events.len(); }
    void push(PollEvent event) { m_events.push(rstd::move(event)); }

    auto pop_front() -> Option<PollEvent> {
        if (m_events.is_empty()) return None();
        return Some(m_events.remove(0));
    }
};

struct PollWakeState {
    sys::fd::OwnedFd fd;

    explicit PollWakeState(sys::fd::OwnedFd fd): fd(rstd::move(fd)) {}
};

export class PollWake {
    sync::Arc<PollWakeState> m_state;

    explicit PollWake(sync::Arc<PollWakeState> state): m_state(rstd::move(state)) {}

#if !defined(RSTD_OS_WINDOWS)
    friend class Poll;
#endif

public:
    PollWake(const PollWake&)                        = delete;
    auto operator=(const PollWake&) -> PollWake&     = delete;
    PollWake(PollWake&&) noexcept                    = default;
    auto operator=(PollWake&&) noexcept -> PollWake& = default;

    auto clone() const -> PollWake { return PollWake { m_state.clone() }; }

    auto wake() const -> io::Result<empty> {
#if RSTD_OS_LINUX
        auto value = u64 { 1 };
        auto rc    = libc::write(m_state->fd.as_raw_fd(), &value, sizeof(value));
        if (rc == isize(sizeof(value)) || libc::get_errno() == libc::EAGAIN) {
            return Ok(empty {});
        }
        return Err(io::Error::from_raw_os_error(sys::io::last_os_error()));
#else
        return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::Unsupported }));
#endif
    }
};

struct PollRegistration {
    PollKey        key;
    sys::fd::RawFd fd;
    Interest       interest;
    PollEventOwner owner;
    u32            backend_events {};
    bool           backend_registered { false };

    PollRegistration(PollKey key, sys::fd::RawFd fd, Interest interest, PollEventOwner owner)
        : key(key), fd(fd), interest(interest), owner(rstd::move(owner)) {}
};

struct PollTimer {
    PollKey        key;
    time::Instant  deadline;
    PollEventOwner owner;

    PollTimer(PollKey key, time::Instant deadline, PollEventOwner owner)
        : key(key), deadline(deadline), owner(rstd::move(owner)) {}
};

export class PollState {
#if defined(RSTD_OS_WINDOWS)
public:
#endif
    PollStateKind         m_kind { PollStateKind::Closed };
    sys::fd::OwnedFd      m_poll_fd {};
    sys::fd::OwnedFd      m_wake_fd {};
    sys::fd::OwnedFd      m_timer_fd {};
    Vec<PollRegistration> m_registrations;
    Vec<PollTimer>        m_timers;
#if RSTD_OS_LINUX
    Vec<libc::epoll_event> m_backend_events;
#endif

    PollState(sys::fd::OwnedFd poll_fd, sys::fd::OwnedFd wake_fd, sys::fd::OwnedFd timer_fd)
        : m_kind(PollStateKind::Active),
          m_poll_fd(rstd::move(poll_fd)),
          m_wake_fd(rstd::move(wake_fd)),
          m_timer_fd(rstd::move(timer_fd)),
          m_registrations(Vec<PollRegistration>::make()),
          m_timers(Vec<PollTimer>::make())
#if RSTD_OS_LINUX
          ,
          m_backend_events(Vec<libc::epoll_event>::make())
#endif
    {
#if RSTD_OS_LINUX
        m_backend_events.resize(64, libc::epoll_event {});
#endif
    }

#if !defined(RSTD_OS_WINDOWS)
    friend class Poll;
#endif

public:
    PollState()                                        = default;
    PollState(const PollState&)                        = delete;
    auto operator=(const PollState&) -> PollState&     = delete;
    PollState(PollState&&) noexcept                    = default;
    auto operator=(PollState&&) noexcept -> PollState& = default;

    auto kind() const noexcept -> PollStateKind { return m_kind; }
};

export struct PollInit {
    PollState state;
    PollWake  wake;

    PollInit(PollState state, PollWake wake): state(rstd::move(state)), wake(rstd::move(wake)) {}
};

export class Poll {
    static auto last_os_error() noexcept -> io::Error {
        return io::Error::from_raw_os_error(sys::io::last_os_error());
    }

    static void drain_wake(sys::fd::RawFd fd) noexcept {
#if RSTD_OS_LINUX
        auto value = u64 {};
        while (libc::read(fd, &value, sizeof(value)) > 0) {
        }
#else
        (void)fd;
#endif
    }

    static void drain_timer(sys::fd::RawFd fd) noexcept {
#if RSTD_OS_LINUX
        auto value = u64 {};
        while (libc::read(fd, &value, sizeof(value)) > 0) {
        }
#else
        (void)fd;
#endif
    }

#if !defined(RSTD_OS_WINDOWS)
    static auto duration_to_timespec(time::Duration duration) noexcept -> libc::timespec_t {
        return libc::timespec_t {
            .tv_sec  = static_cast<libc::time_t>(duration.as_secs()),
            .tv_nsec = static_cast<long>(duration.subsec_nanos()),
        };
    }
#endif

    static auto backend_interest(Interest interest) noexcept -> u32 {
        auto events = u32 {};
#if RSTD_OS_LINUX
        if (interest.is_readable()) {
            events |= libc::EPOLLIN;
            if (libc::HAS_EPOLLRDHUP) events |= libc::EPOLLRDHUP;
        }
        if (interest.is_writable()) events |= libc::EPOLLOUT;
#else
        (void)interest;
#endif
        return events;
    }

    static auto backend_ready(u32 events) noexcept -> Ready {
        auto ready = Ready {};
#if RSTD_OS_LINUX
        if ((events & libc::EPOLLIN) != 0) ready |= Ready::readable();
        if ((events & libc::EPOLLOUT) != 0) ready |= Ready::writable();
        if (libc::HAS_EPOLLRDHUP && (events & libc::EPOLLRDHUP) != 0) {
            ready |= Ready::read_closed();
        }
        if ((events & libc::EPOLLHUP) != 0) {
            ready |= Ready::read_closed() | Ready::write_closed();
        }
        if ((events & libc::EPOLLERR) != 0) ready |= Ready::error();
#else
        (void)events;
#endif
        return ready;
    }

    static auto find_registration(PollState& state, PollKey key) -> PollRegistration* {
        for (usize i = 0; i < state.m_registrations.len(); ++i) {
            if (state.m_registrations[i].key == key)
                return rstd::addressof(state.m_registrations[i]);
        }
        return nullptr;
    }

    static auto update_registration(PollState&        state,
                                    PollRegistration& registration,
                                    Interest          interest) -> io::Result<empty> {
#if RSTD_OS_LINUX
        auto events = backend_interest(interest);
        if (events == registration.backend_events && registration.backend_registered) {
            registration.interest = interest;
            return Ok(empty {});
        }

        if (events == 0) {
            if (registration.backend_registered && libc::epoll_ctl(state.m_poll_fd.as_raw_fd(),
                                                                   libc::EPOLL_CTL_DEL,
                                                                   registration.fd,
                                                                   nullptr) < 0) {
                return Err(last_os_error());
            }
            registration.interest           = interest;
            registration.backend_events     = 0;
            registration.backend_registered = false;
            return Ok(empty {});
        }

        auto event     = libc::epoll_event {};
        event.events   = events | libc::EPOLLERR | libc::EPOLLHUP;
        event.data.u64 = registration.key.value;
        auto operation =
            registration.backend_registered ? libc::EPOLL_CTL_MOD : libc::EPOLL_CTL_ADD;
        if (libc::epoll_ctl(state.m_poll_fd.as_raw_fd(), operation, registration.fd, &event) < 0) {
            return Err(last_os_error());
        }

        registration.interest           = interest;
        registration.backend_events     = events;
        registration.backend_registered = true;
        return Ok(empty {});
#else
        (void)state;
        (void)registration;
        (void)interest;
        return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::Unsupported }));
#endif
    }

    static auto update_timer(PollState& state) -> io::Result<empty> {
#if RSTD_OS_LINUX
        auto spec = libc::itimerspec_t {};
        if (! state.m_timers.is_empty()) {
            auto deadline = state.m_timers[0].deadline;
            for (usize i = 1; i < state.m_timers.len(); ++i) {
                if (state.m_timers[i].deadline < deadline) {
                    deadline = state.m_timers[i].deadline;
                }
            }

            auto now      = time::Instant::now();
            auto duration = deadline <= now ? time::Duration::from_nanos(1) : deadline - now;
            spec.it_value = duration_to_timespec(duration);
        }

        if (libc::timerfd_settime(state.m_timer_fd.as_raw_fd(), 0, &spec, nullptr) < 0) {
            return Err(last_os_error());
        }
        return Ok(empty {});
#else
        (void)state;
        return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::Unsupported }));
#endif
    }

    static auto collect_expired_timers(PollState& state, PollBatch& batch) -> io::Result<empty> {
        auto now = time::Instant::now();
        for (usize i = 0; i < state.m_timers.len();) {
            if (state.m_timers[i].deadline <= now) {
                auto timer = state.m_timers.remove(i);
                batch.push(
                    PollEvent::owned(PollEventData::timer(timer.key), rstd::move(timer.owner)));
            } else {
                ++i;
            }
        }
        return update_timer(state);
    }

public:
    static auto init() -> io::Result<PollInit> {
#if RSTD_OS_LINUX
        auto poll_fd = libc::epoll_create1(libc::EPOLL_CLOEXEC);
        if (poll_fd < 0) return Err(last_os_error());
        auto owned_poll = sys::fd::OwnedFd::from_raw_fd(poll_fd);

        auto wake_fd = libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC);
        if (wake_fd < 0) return Err(last_os_error());
        auto owned_wake = sys::fd::OwnedFd::from_raw_fd(wake_fd);
        auto wake_send  = owned_wake.try_clone();
        if (wake_send.is_err()) return Err(rstd::move(wake_send).unwrap_err_unchecked());

        auto timer_fd =
            libc::timerfd_create(libc::CLOCK_MONOTONIC, libc::TFD_NONBLOCK | libc::TFD_CLOEXEC);
        if (timer_fd < 0) return Err(last_os_error());
        auto owned_timer = sys::fd::OwnedFd::from_raw_fd(timer_fd);

        auto event     = libc::epoll_event {};
        event.events   = libc::EPOLLIN;
        event.data.u64 = POLL_WAKE_KEY;
        if (libc::epoll_ctl(poll_fd, libc::EPOLL_CTL_ADD, wake_fd, &event) < 0) {
            return Err(last_os_error());
        }

        auto timer_event     = libc::epoll_event {};
        timer_event.events   = libc::EPOLLIN;
        timer_event.data.u64 = POLL_TIMER_KEY;
        if (libc::epoll_ctl(poll_fd, libc::EPOLL_CTL_ADD, timer_fd, &timer_event) < 0) {
            return Err(last_os_error());
        }

        auto wake_state = sync::Arc<PollWakeState>::make(rstd::move(wake_send).unwrap_unchecked());
        return Ok(PollInit {
            PollState { rstd::move(owned_poll), rstd::move(owned_wake), rstd::move(owned_timer) },
            PollWake { rstd::move(wake_state) } });
#else
        return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::Unsupported }));
#endif
    }

    static auto capabilities(const PollState&) noexcept -> PollCapabilities {
#if RSTD_OS_LINUX
        return PollCapabilities::of(PollCapability::Readiness) | PollCapability::Timer |
               PollCapability::Wake;
#else
        return PollCapabilities::none();
#endif
    }

    static auto apply(PollState& state, PollCommand command) -> PollApplyResult {
        if (state.m_kind == PollStateKind::Closed || ! command.key().is_valid()) {
            return PollApplyResult::rejected(
                rstd::move(command),
                io::Error::from_kind(io::ErrorKind { io::ErrorKind::InvalidInput }));
        }

        switch (command.kind()) {
        case PollCommandKind::RegisterSource: {
            if (command.key().kind != PollKeyKind::Registration ||
                find_registration(state, command.key()) != nullptr) {
                return PollApplyResult::rejected(
                    rstd::move(command),
                    io::Error::from_kind(io::ErrorKind { io::ErrorKind::InvalidInput }));
            }

            auto registration = PollRegistration {
                command.key(), command.fd(), Interest {}, command.owner().clone()
            };
            auto updated = update_registration(state, registration, command.interest());
            if (updated.is_err()) {
                return PollApplyResult::rejected(rstd::move(command),
                                                 rstd::move(updated).unwrap_err_unchecked());
            }
            state.m_registrations.push(rstd::move(registration));
            return PollApplyResult::accepted();
        }
        case PollCommandKind::UpdateInterest: {
            auto* registration = find_registration(state, command.key());
            if (registration == nullptr) {
                return PollApplyResult::rejected(
                    rstd::move(command),
                    io::Error::from_kind(io::ErrorKind { io::ErrorKind::NotFound }));
            }
            auto updated = update_registration(state, *registration, command.interest());
            if (updated.is_err()) {
                return PollApplyResult::rejected(rstd::move(command),
                                                 rstd::move(updated).unwrap_err_unchecked());
            }
            return PollApplyResult::accepted();
        }
        case PollCommandKind::DeregisterSource: {
            for (usize i = 0; i < state.m_registrations.len(); ++i) {
                if (state.m_registrations[i].key != command.key()) continue;
                auto updated = update_registration(state, state.m_registrations[i], Interest {});
                if (updated.is_err()) {
                    return PollApplyResult::rejected(rstd::move(command),
                                                     rstd::move(updated).unwrap_err_unchecked());
                }
                state.m_registrations.remove(i);
                return PollApplyResult::accepted();
            }
            return PollApplyResult::accepted();
        }
        case PollCommandKind::SubmitOperation:
        case PollCommandKind::CancelOperation:
            if (command.key().kind != PollKeyKind::Operation) {
                return PollApplyResult::rejected(
                    rstd::move(command),
                    io::Error::from_kind(io::ErrorKind { io::ErrorKind::InvalidInput }));
            }
            return PollApplyResult::unsupported(rstd::move(command));
        case PollCommandKind::ArmTimer: {
            if (command.key().kind != PollKeyKind::Timer) {
                return PollApplyResult::rejected(
                    rstd::move(command),
                    io::Error::from_kind(io::ErrorKind { io::ErrorKind::InvalidInput }));
            }
            for (usize i = 0; i < state.m_timers.len(); ++i) {
                if (state.m_timers[i].key == command.key()) {
                    return PollApplyResult::rejected(
                        rstd::move(command),
                        io::Error::from_kind(io::ErrorKind { io::ErrorKind::InvalidInput }));
                }
            }

            state.m_timers.push(
                PollTimer { command.key(), command.deadline(), command.owner().clone() });
            auto updated = update_timer(state);
            if (updated.is_err()) {
                state.m_timers.pop();
                return PollApplyResult::rejected(rstd::move(command),
                                                 rstd::move(updated).unwrap_err_unchecked());
            }
            return PollApplyResult::accepted();
        }
        case PollCommandKind::CancelTimer:
            for (usize i = 0; i < state.m_timers.len(); ++i) {
                if (state.m_timers[i].key != command.key()) continue;
                state.m_timers.remove(i);
                auto updated = update_timer(state);
                if (updated.is_err()) {
                    return PollApplyResult::rejected(rstd::move(command),
                                                     rstd::move(updated).unwrap_err_unchecked());
                }
                return PollApplyResult::accepted();
            }
            return PollApplyResult::accepted();
        }
        return PollApplyResult::unsupported(rstd::move(command));
    }

    static auto poll(PollState& state, PollTimeout timeout) -> io::Result<PollBatch> {
#if RSTD_OS_LINUX
        if (state.m_kind == PollStateKind::Closed) {
            return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::NotConnected }));
        }

        state.m_kind = PollStateKind::Waiting;
        auto wait_ms = timeout == PollTimeout::Immediate ? 0 : -1;
        int  count {};
        do {
            count = libc::epoll_wait(state.m_poll_fd.as_raw_fd(),
                                     state.m_backend_events.data(),
                                     int(state.m_backend_events.len()),
                                     wait_ms);
        } while (count < 0 && libc::get_errno() == libc::EINTR);
        state.m_kind = PollStateKind::Active;

        if (count < 0) return Err(last_os_error());

        auto batch = PollBatch {};
        for (int i = 0; i < count; ++i) {
            auto& event = state.m_backend_events[usize(i)];
            if (event.data.u64 == POLL_WAKE_KEY) {
                drain_wake(state.m_wake_fd.as_raw_fd());
                batch.push(PollEvent::wake());
                continue;
            }
            if (event.data.u64 == POLL_TIMER_KEY) {
                drain_timer(state.m_timer_fd.as_raw_fd());
                auto collected = collect_expired_timers(state, batch);
                if (collected.is_err()) {
                    return Err(rstd::move(collected).unwrap_err_unchecked());
                }
                continue;
            }

            auto  key          = PollKey { PollKeyKind::Registration, event.data.u64 };
            auto* registration = find_registration(state, key);
            if (registration != nullptr) {
                auto ready = backend_ready(event.events);
                if (! ready.is_empty()) {
                    batch.push(PollEvent::owned(PollEventData::readiness(key, ready),
                                                registration->owner.clone()));
                }
            }
        }
        return Ok(rstd::move(batch));
#else
        (void)state;
        (void)timeout;
        return Err(io::Error::from_kind(io::ErrorKind { io::ErrorKind::Unsupported }));
#endif
    }

    static auto shutdown(PollState& state) -> PollBatch {
        auto batch = PollBatch {};
        if (state.m_kind == PollStateKind::Closed) return batch;
        state.m_kind = PollStateKind::Draining;
        while (! state.m_registrations.is_empty()) {
            auto registration = state.m_registrations.pop().unwrap_unchecked();
            batch.push(PollEvent::owned(
                PollEventData::backend_error(
                    registration.key,
                    io::Error::from_kind(io::ErrorKind { io::ErrorKind::NotConnected })),
                rstd::move(registration.owner)));
        }
        while (! state.m_timers.is_empty()) {
            auto timer = state.m_timers.pop().unwrap_unchecked();
            batch.push(PollEvent::owned(
                PollEventData::backend_error(
                    timer.key, io::Error::from_kind(io::ErrorKind { io::ErrorKind::NotConnected })),
                rstd::move(timer.owner)));
        }
        state.m_poll_fd  = sys::fd::OwnedFd {};
        state.m_wake_fd  = sys::fd::OwnedFd {};
        state.m_timer_fd = sys::fd::OwnedFd {};
        state.m_kind     = PollStateKind::Closed;
        return batch;
    }
};

} // namespace rstd::async

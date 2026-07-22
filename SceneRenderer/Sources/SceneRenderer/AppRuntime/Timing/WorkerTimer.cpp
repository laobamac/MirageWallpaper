module;

#include <rstd/macro.hpp>

module sr.timer;
import rstd.cppstd;

using namespace sr;
using micros = std::chrono::microseconds;

ThreadTimer::ThreadTimer(std::function<void()> cb)
    : m_callback(cb),
      m_interval(std::chrono::microseconds(1'000'000 / 15)),
      m_running(false),
      m_interval_revision(0) {}
ThreadTimer::~ThreadTimer() { Stop(); }

bool ThreadTimer::Running() const { return m_running; }

void ThreadTimer::SetInterval(micros v) {
    if (v <= micros::zero()) v = micros(1);
    m_interval = v;
    m_interval_revision.fetch_add(1, std::memory_order_release);
    m_condition.notify_all();
}

void ThreadTimer::Start() {
    std::unique_lock<std::mutex> lock(m_op_mutex);

    if (Running()) return;
    m_running = true;
    m_timer_thread = std::thread([this]() {
        using clock = std::chrono::steady_clock;
        auto revision = m_interval_revision.load(std::memory_order_acquire);
        auto interval = m_interval.load();
        auto deadline = clock::now() + interval;
        while (Running()) {
            {
                std::unique_lock<std::mutex> lock(m_cond_mutex);
                m_condition.wait_until(lock, deadline, [this, revision]() {
                    return ! Running() ||
                           m_interval_revision.load(std::memory_order_acquire) != revision;
                });
            }
            if (! Running()) break;
            const auto current_revision =
                m_interval_revision.load(std::memory_order_acquire);
            if (current_revision != revision) {
                revision = current_revision;
                interval = m_interval.load();
                deadline = clock::now() + interval;
                continue;
            }
            if (m_callback) m_callback();
            interval = m_interval.load();
            const auto now = clock::now();
            do {
                deadline += interval;
            } while (deadline <= now);
        }
    });
}

void ThreadTimer::Stop() {
    std::unique_lock<std::mutex> lock(m_op_mutex);
    rstd_assert(std::this_thread::get_id() != m_timer_thread.get_id());

    if (! Running()) return;
    m_running = false;

    {
        std::unique_lock<std::mutex> lock(m_cond_mutex);
        m_condition.notify_all();
    }

    if (m_timer_thread.joinable()) {
        m_timer_thread.join();
    }
}

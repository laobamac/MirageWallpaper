module;

module sr.timer;
import sr.core;
import rstd.cppstd;

using namespace sr;
using micros = std::chrono::microseconds;
using namespace std::chrono;

FrameTimer::FrameTimer(std::function<void()> cb)
    : m_callback(cb), m_frame_busy_count(0), m_timer([this]() {
          i32 expected = 0;
          if (m_callback &&
              m_frame_busy_count.compare_exchange_strong(expected, 1,
                                                         std::memory_order_acq_rel)) {
              m_callback();
          }
      }) {
    SetRequiredFps(15);
}

FrameTimer::~FrameTimer() {};

u16 FrameTimer::RequiredFps() const { return m_req_fps; }

double FrameTimer::FrameTime() const {
    return duration_cast<duration<double>>(m_frametime.load()).count();
}

double FrameTimer::IdeaTime() const {
    auto frametime = m_frametime.load();
    auto ideatime  = m_ideatime.load();
    auto time      = frametime > ideatime ? frametime : ideatime;
    return duration_cast<duration<double>>(time).count();
}

void FrameTimer::UpdateFrametime() {
    m_frametime.store(
        std::accumulate(m_frametime_queue.begin(), m_frametime_queue.end(), microseconds(0)) /
        m_frametime_queue.size());
}

void FrameTimer::SetRequiredFps(u16 value) {
    if (value == 0) value = 1;
    m_req_fps             = value;
    microseconds ideatime = microseconds(1'000'000 / m_req_fps);
    m_ideatime            = ideatime;
    m_timer.SetInterval(ideatime);
    for (usize i = 0; i < FrameTimer::FRAMETIME_QUEUE_SIZE; i++) {
        AddFrametime(ideatime);
    }
    UpdateFrametime();
}

void FrameTimer::AddFrametime(micros t) {
    m_frametime_queue.push_back(t);
    while (m_frametime_queue.size() > FrameTimer::FRAMETIME_QUEUE_SIZE) {
        m_frametime_queue.pop_front();
    }
}

void FrameTimer::FrameBegin() { m_clock = steady_clock::now(); }
void FrameTimer::FrameEnd() {
    auto now = steady_clock::now();
    AddFrametime(duration_cast<microseconds>(now - m_clock));
    UpdateFrametime();

    m_frame_busy_count.store(0, std::memory_order_release);
}

void FrameTimer::SetCallback(const std::function<void()>& cb) {
    if (! Running()) m_callback = cb;
}
void FrameTimer::Run() { m_timer.Start(); }
void FrameTimer::Stop() { m_timer.Stop(); }
bool FrameTimer::Running() const { return m_timer.Running(); }

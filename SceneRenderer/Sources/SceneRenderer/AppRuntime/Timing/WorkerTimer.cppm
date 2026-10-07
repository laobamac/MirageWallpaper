module;

export module sr.timer:thread_timer;
import sr.core;
import rstd.cppstd;

export namespace sr
{

class ThreadTimer : NoCopy, NoMove {
public:
    // Callback returns true when it actually ran the frame, false when it
    // declined (e.g. the previous frame is still in flight). NotifyReady()
    // wakes a declined callback when its resources become available.
    ThreadTimer(std::function<bool()> callback);
    ~ThreadTimer();

    void Start();
    void Stop();

    bool Running() const;

    void SetInterval(std::chrono::microseconds);
    void NotifyReady();

private:
    std::function<bool()> m_callback;

    std::mutex m_op_mutex;

    std::thread             m_timer_thread;
    std::mutex              m_cond_mutex;
    std::condition_variable m_condition;
    std::condition_variable m_ready_condition;

    std::atomic<std::chrono::microseconds> m_interval;
    std::atomic<bool>                      m_running;
    std::atomic<std::uint64_t>             m_interval_revision;
    std::atomic<std::uint64_t>             m_completion_revision { 0 };
};

} // namespace sr

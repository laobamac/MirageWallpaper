module;

export module sr.timer:thread_timer;
import sr.core;
import rstd.cppstd;

export namespace sr
{

class ThreadTimer : NoCopy, NoMove {
public:
    ThreadTimer(std::function<void()> callback);
    ~ThreadTimer();

    void Start();
    void Stop();

    bool Running() const;

    void SetInterval(std::chrono::microseconds);

private:
    std::function<void()> m_callback;

    std::mutex m_op_mutex;

    std::thread             m_timer_thread;
    std::mutex              m_cond_mutex;
    std::condition_variable m_condition;

    std::atomic<std::chrono::microseconds> m_interval;
    std::atomic<bool>                      m_running;
    std::atomic<std::uint64_t>             m_interval_revision;
};

} // namespace sr

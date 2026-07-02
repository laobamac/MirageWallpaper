module;

#include <rstd/macro.hpp>
export module sr.message_loop;
import sr.core;
import rstd.log;
import rstd.cppstd;

export namespace sr::msgloop
{

template<class T>
class MessageLoop : NoCopy {
public:
    using Sender   = rstd::sync::mpsc::Sender<T>;
    using Receiver = rstd::sync::mpsc::Receiver<T>;

    explicit MessageLoop(std::string name): m_name(std::move(name)) {
        auto [tx, rx] = rstd::sync::mpsc::channel<T>();
        m_tx.emplace(std::move(tx));
        m_rx.emplace(std::move(rx));
    }

    ~MessageLoop() { stop(); }

    /// Returns a fresh Sender clone. Cheap; safe to call before/after start().
    Sender sender() const { return Sender(*m_tx); }

    const std::string& name() const noexcept { return m_name; }

    /// Spawns the worker thread. F is invoked as `f(T&&)` for each received
    /// message. The loop exits cleanly when every Sender has been dropped.
    template<class F>
    void start(F&& on_message) {
        if (m_thread.joinable()) return;
        m_thread = std::thread([this, fn = std::forward<F>(on_message)]() mutable {
            rstd_info("{} loop started", m_name);
            while (true) {
                auto r = m_rx->recv();
                if (! r.is_ok()) break;
                fn(std::move(r).unwrap());
            }
            rstd_info("{} loop stopped", m_name);
        });
    }

    /// Drops the engine-side Sender and joins. Any external Sender clones
    /// must already be released for recv() to actually return Err. If called
    /// from inside the worker thread we detach instead of self-joining.
    void stop() {
        m_tx.reset();
        if (! m_thread.joinable()) return;
        if (std::this_thread::get_id() == m_thread.get_id()) {
            m_thread.detach();
        } else {
            m_thread.join();
        }
    }

private:
    std::string             m_name;
    std::optional<Sender>   m_tx;
    std::optional<Receiver> m_rx;
    std::thread             m_thread;
};

} // namespace sr::msgloop

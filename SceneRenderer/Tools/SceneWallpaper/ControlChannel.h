// ControlChannel — line-oriented JSON control protocol over stdin.
//
// Lets a parent process (Mirage.app) drive a running SceneWallpaper without
// restarting it: live user-property edits, pause/resume, volume, mute, fps,
// fill mode, speed. Each line on stdin is one JSON object:
//
//   {"cmd":"setProperty","key":"clock","value":false}
//   {"cmd":"setProperty","key":"schemecolor","type":"color","value":"1 0 0"}
//   {"cmd":"pause"} / {"cmd":"resume"}
//   {"cmd":"volume","value":0.5}      // 0..1
//   {"cmd":"muted","value":true}
//   {"cmd":"fps","value":30}
//   {"cmd":"fillmode","value":"cover"|"contain"|"stretch"}
//   {"cmd":"speed","value":1.0}
//   {"cmd":"quit"}
//
// The reader runs on its own std::thread; sr::SceneWallpaper's setters post
// thread-safe messages to the render/main message loops (rstd mpsc channel),
// so calling them from this thread is safe. EOF on stdin (parent closed the
// pipe / died) triggers the on_quit callback so the wallpaper exits cleanly.
//
// NOTE: JSON lives in the sr build as a C++20 module (`import nlohmann.json;`),
// not a header — so this header stays JSON-free and all parsing is in the .cpp.

#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace sr {
class SceneWallpaper;
}

namespace mirage {

class SceneControlChannel {
public:
    // on_quit is invoked (from the reader thread) when a {"cmd":"quit"} arrives
    // or stdin hits EOF. It should stop the desktop run loop.
    SceneControlChannel(sr::SceneWallpaper& wallpaper, std::function<void()> on_quit)
        : m_wallpaper(wallpaper), m_on_quit(std::move(on_quit)) {}

    ~SceneControlChannel() { stop(); }

    SceneControlChannel(const SceneControlChannel&)            = delete;
    SceneControlChannel& operator=(const SceneControlChannel&) = delete;

    void start() {
        if (m_running.exchange(true)) return;
        m_thread = std::thread([this] { readLoop(); });
    }

    void stop() {
        m_running.store(false);
        if (m_thread.joinable()) {
            // The reader is blocked on getline(stdin); detach so shutdown never
            // hangs waiting for a line that may never come. The process is
            // exiting anyway.
            m_thread.detach();
        }
    }

private:
    void readLoop();
    void dispatchLine(const char* line);

    sr::SceneWallpaper&   m_wallpaper;
    std::function<void()> m_on_quit;
    std::atomic<bool>     m_running { false };
    std::thread           m_thread;
};

} // namespace mirage

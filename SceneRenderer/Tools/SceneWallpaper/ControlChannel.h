// ControlChannel — line-oriented JSON control protocol over stdin.
//
// Lets a parent process (Mirage.app) drive a running SceneWallpaper without
// restarting it: live user-property edits, pause/resume, volume, mute, fps,
// fill mode, speed. Each line on stdin is one JSON object:
//
//   {"cmd":"setProperty","key":"clock","value":false}
//   {"cmd":"setProperty","key":"schemecolor","type":"color","value":"1 0 0"}
//   {"cmd":"pause"} / {"cmd":"resume"}
//   {"cmd":"power","state":"run"|"throttle"|"pause","fps":30}
//   {"cmd":"volume","value":0.5}      // 0..1
//   {"cmd":"muted","value":true}
//   {"cmd":"fps","value":30}
//   {"cmd":"fillmode","value":"cover"|"contain"|"stretch"}
//   {"cmd":"speed","value":1.0}
//   {"cmd":"activate"}              // reveal a deferred-show window
//   {"cmd":"snapshot","path":"…","token":"…"}  // still frame for the desktop picture
//   {"cmd":"quit"}
//
// `power` is the authoritative playback state. All policy — occlusion, screen
// lock, display sleep, battery, thermal pressure — is decided in Mirage.app,
// which is the only process with a global view of window layering and power
// sources; this renderer never observes any of it and only obeys the result.
//
// The reader runs on its own std::thread; sr::SceneWallpaper's setters post
// thread-safe messages to the render/main message loops (rstd mpsc channel),
// so calling them from this thread is safe. EOF on stdin (parent closed the
// pipe / died) triggers the on_quit callback so the wallpaper exits cleanly.
//
// NOTE: JSON lives in the sr build as a C++20 module (`import sr.json;`),
// not a header — so this header stays JSON-free and all parsing is in the .cpp.

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace sr {
class SceneWallpaper;
}

namespace mirage {

class SceneControlChannel {
public:
    // on_quit is invoked (from the reader thread) when a {"cmd":"quit"} arrives
    // or stdin hits EOF. It should stop the desktop run loop.
    //
    // on_snapshot receives (path, token) for {"cmd":"snapshot"} and is expected
    // to write a still of the live frame and report the outcome back on stdout.
    // It runs on the reader thread and blocks it while waiting for a frame,
    // which is fine: commands are rare and ordering is preserved.
    SceneControlChannel(sr::SceneWallpaper& wallpaper, std::function<void()> on_quit,
                        std::function<void()> on_activate = {},
                        std::function<void()> on_deactivate = {},
                        std::function<void(const std::string&, const std::string&)>
                            on_snapshot = {})
        : m_wallpaper(wallpaper),
          m_on_quit(std::move(on_quit)),
          m_on_activate(std::move(on_activate)),
          m_on_deactivate(std::move(on_deactivate)),
          m_on_snapshot(std::move(on_snapshot)) {}

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
            // readLoop polls stdin with a short timeout, so shutdown can join
            // safely instead of leaving a detached thread holding references
            // to this channel and SceneWallpaper during teardown.
            m_thread.join();
        }
    }

private:
    void readLoop();
    void dispatchLine(const char* line);

    sr::SceneWallpaper&   m_wallpaper;
    std::function<void()> m_on_quit;
    std::function<void()> m_on_activate;
    std::function<void()> m_on_deactivate;
    std::function<void(const std::string&, const std::string&)> m_on_snapshot;
    std::atomic<bool>     m_running { false };
    std::thread           m_thread;
};

} // namespace mirage

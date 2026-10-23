#include "ControlChannel.h"

#include <cerrno>
#include <array>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>

import sr.json;
import rstd.cppstd;
import sr.scene_wallpaper; // re-exports sr.types (FillMode)

namespace mirage {

namespace {

// WINDOW_STATE flag bits (mirage-display protocol): 0x1 covered, 0x2 focus
// lost, 0x4 maximized, 0x8 fullscreen. Playback pauses while focus is lost or
// a fullscreen window covers the wallpaper.
constexpr std::uint32_t kWindowFocusLost = 0x2u;
constexpr std::uint32_t kWindowFullscreen = 0x8u;

// Maps the wire fill-mode name (matching WE / the other renderers' vocabulary)
// to sr::FillMode. cover→ASPECTCROP, contain/fit→ASPECTFIT, stretch→STRETCH.
bool ParseFillMode(const std::string& s, sr::FillMode& out) {
    if (s == "cover" || s == "aspectcrop" || s == "crop") {
        out = sr::FillMode::ASPECTCROP;
        return true;
    }
    if (s == "contain" || s == "fit" || s == "aspectfit") {
        out = sr::FillMode::ASPECTFIT;
        return true;
    }
    if (s == "stretch") {
        out = sr::FillMode::STRETCH;
        return true;
    }
    return false;
}

} // namespace

void SceneControlChannel::start() {
    if (m_running.exchange(true)) return;
    if (::pipe(m_wake_fds) != 0) {
        m_running.store(false);
        if (m_on_quit) m_on_quit();
        return;
    }
    for (int fd : m_wake_fds) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    m_thread = std::thread([this] {
        readLoop();
    });
}

void SceneControlChannel::stop() {
    if (m_running.exchange(false) && m_wake_fds[1] >= 0) {
        const char wake = 1;
        while (::write(m_wake_fds[1], &wake, 1) < 0 && errno == EINTR) {
        }
    }
    if (m_thread.joinable()) m_thread.join();
    for (int& fd : m_wake_fds) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
}

void SceneControlChannel::dispatchLine(const char* line) {
    if (line == nullptr) return;

    auto parsed = sr::ParseJson(line, { .allow_comments = true });
    if (parsed.is_err()) return;
    auto msg = parsed.unwrap();
    if (! msg.is_object()) return;
    auto command = msg.get("cmd");
    if (command.is_none()) return;
    auto command_text = (*command)->as_str();
    if (command_text.is_none()) return;

    const std::string cmd = rstd::cppstd::to_string(*command_text);

    if (cmd == "setProperty") {
        auto key_value = msg.get("key");
        if (key_value.is_none()) return;
        auto key_text = (*key_value)->as_str();
        if (key_text.is_none()) return;
        const std::string key = rstd::cppstd::to_string(*key_text);

        // Build the property descriptor the runtime expects. If an explicit
        // "type" is present (e.g. color), wrap {type,value}; otherwise pass the
        // raw value (bool/number/string) — CoerceUserPropertyValue infers it.
        auto type = msg.get("type");
        auto value = msg.get("value");
        sr::Json prop = sr::Json::Null();
        if (type.is_some() && (*type)->is_string()) {
            auto object = rstd::json::Map::make();
            object.insert(::alloc::string::String::make(rstd::cppstd::as_str("type")),
                          (*type)->clone());
            object.insert(::alloc::string::String::make(rstd::cppstd::as_str("value")),
                          value.is_some() ? (*value)->clone() : sr::Json::Null());
            auto icon = msg.get("icon");
            if (icon.is_some())
                object.insert(::alloc::string::String::make(rstd::cppstd::as_str("icon")),
                              (*icon)->clone());
            prop = sr::Json::Object(rstd::move(object));
        } else if (value.is_some()) {
            prop = (*value)->clone();
        } else {
            return;
        }
        m_wallpaper.setUserPropertyJson(key, std::move(prop));
    } else if (cmd == "pause") {
        m_wallpaper.pause();
    } else if (cmd == "resume" || cmd == "play") {
        m_wallpaper.play();
    } else if (cmd == "power") {
        // The app is the sole judge of occlusion, lock, sleep, battery and
        // thermal state — this renderer's window is deliberately never reported
        // as occluded, so it cannot observe any of that itself. Here we only
        // obey the final state: "pause" stops the frame timer outright,
        // "run"/"throttle" differ solely in the frame rate carried alongside.
        auto state = msg.get("state");
        if (state.is_none() || ! (*state)->is_string()) return;
        const std::string power = rstd::cppstd::to_string(*(*state)->as_str());
        if (power == "pause") {
            m_wallpaper.pause();
            return;
        }
        if (power != "run" && power != "throttle") return;
        auto fps = msg.get("fps");
        if (fps.is_some() && (*fps)->is_number()) {
            auto number = (*fps)->as_u64();
            if (number.is_some()) m_wallpaper.setFps(static_cast<std::uint32_t>(*number));
        }
        m_wallpaper.play();
    } else if (cmd == "volume") {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_number()) {
            auto number = (*value)->as_f64();
            if (number.is_some()) m_wallpaper.setVolume(static_cast<float>(*number));
        }
    } else if (cmd == "muted") {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_boolean()) {
            m_wallpaper.setMuted(*(*value)->as_bool());
        }
    } else if (cmd == "fps") {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_number()) {
            auto number = (*value)->as_u64();
            if (number.is_some()) m_wallpaper.setFps(static_cast<std::uint32_t>(*number));
        }
    } else if (cmd == "fillmode") {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_string()) {
            sr::FillMode mode {};
            if (ParseFillMode(rstd::cppstd::to_string(*(*value)->as_str()), mode)) {
                m_wallpaper.setFillMode(mode);
            }
        }
    } else if (cmd == "position") {
        auto x = msg.get("x");
        auto y = msg.get("y");
        if (x.is_some() && y.is_some() && (*x)->is_number() && (*y)->is_number()) {
            auto px = (*x)->as_f64();
            auto py = (*y)->as_f64();
            if (px.is_some() && py.is_some()) m_wallpaper.setPosition({ *px, *py });
        }
    } else if (cmd == "speed") {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_number()) {
            auto number = (*value)->as_f64();
            if (number.is_some()) m_wallpaper.setSpeed(static_cast<float>(*number));
        }
    } else if (cmd == "audioSpectrum") {
        auto data = msg.get("data");
        if (data.is_none() || !(*data)->is_array()) return;
        auto values = (*data)->as_array();
        if (values.is_none() || (*values)->len() != 128) return;
        std::array<float, 64> left {};
        std::array<float, 64> right {};
        std::size_t index = 0;
        for (const auto& value : **values) {
            auto number = value.as_f64();
            if (number.is_none()) return;
            if (index < 64)
                left[index] = static_cast<float>(*number);
            else
                right[index - 64] = static_cast<float>(*number);
            ++index;
        }
        m_wallpaper.setAudioSpectrum(std::move(left), std::move(right));
    } else if (cmd == "mediaStatus") {
        auto data = msg.get("data");
        if (data.is_none() || ! (*data)->is_object()) return;
        auto get_string = [&](const char* key) {
            auto value = (*data)->get(key);
            if (value.is_some() && (*value)->is_string())
                return rstd::cppstd::to_string(*(*value)->as_str());
            return std::string {};
        };
        auto get_number = [&](const char* key) {
            auto value = (*data)->get(key);
            if (value.is_some() && (*value)->is_number()) return (*value)->as_f64().unwrap_or(0.0);
            return 0.0;
        };
        auto get_color = [&](const char* key, std::array<float, 3> fallback) {
            auto value = (*data)->get(key);
            if (value.is_none() || ! (*value)->is_array()) return fallback;
            auto values = (*value)->as_array();
            if (values.is_none() || (*values)->len() < 3) return fallback;
            std::size_t index = 0;
            for (const auto& component : **values) {
                if (index >= fallback.size()) break;
                auto number = component.as_f64();
                if (number.is_none()) return fallback;
                fallback[index++] = static_cast<float>(*number);
            }
            return fallback;
        };
        sr::MediaStatus media;
        auto state = (*data)->get("state");
        if (state.is_some() && (*state)->is_number())
            media.state = static_cast<uint32_t>((*state)->as_u64().unwrap_or(0));
        media.title = get_string("title");
        media.artist = get_string("artist");
        media.album = get_string("album");
        media.album_artist = get_string("albumArtist");
        media.position = get_number("position");
        media.duration = get_number("duration");
        media.art_url = get_string("artURL");
        media.previous_art_url = get_string("previousArtURL");
        media.primary_color = get_color("primaryColor", media.primary_color);
        media.secondary_color = get_color("secondaryColor", media.secondary_color);
        media.tertiary_color = get_color("tertiaryColor", media.tertiary_color);
        media.text_color = get_color("textColor", media.text_color);
        media.high_contrast_color =
            get_color("highContrastColor", media.high_contrast_color);
        m_wallpaper.setMediaStatus(std::move(media));
    } else if (cmd == "snapshot") {
        // Mirage.app wants a still of the live frame for the system desktop
        // picture. The outcome must always be reported, or the requester waits
        // out its whole timeout.
        if (! m_on_snapshot) return;
        auto path = msg.get("path");
        auto token = msg.get("token");
        const std::string token_text =
            (token.is_some() && (*token)->is_string())
                ? rstd::cppstd::to_string(*(*token)->as_str())
                : std::string {};
        const std::string path_text =
            (path.is_some() && (*path)->is_string())
                ? rstd::cppstd::to_string(*(*path)->as_str())
                : std::string {};
        m_on_snapshot(path_text, token_text);
    } else if (cmd == "activate") {
        if (m_on_activate) m_on_activate();
    } else if (cmd == "deactivate") {
        m_wallpaper.setMuted(true);
        m_wallpaper.pause();
        if (m_on_deactivate) m_on_deactivate();
    } else if (cmd == "exportScriptStorage") {
        auto token = msg.get("token");
        if (m_on_storage && token.is_some() && (*token)->is_string())
            m_on_storage(rstd::cppstd::to_string(*(*token)->as_str()));
    } else if (cmd == "resetScriptStorage") {
        m_wallpaper.resetScriptStorage();
    } else if (cmd == "quit") {
        m_running.store(false);
        if (m_on_quit) m_on_quit();
    }
}

void SceneControlChannel::readLoop() {
    std::string pending;
    char        buffer[4096];
    while (m_running.load()) {
        pollfd inputs[2] {
            { .fd = STDIN_FILENO, .events = POLLIN | POLLHUP, .revents = 0 },
            { .fd = m_wake_fds[0], .events = POLLIN, .revents = 0 },
        };
        auto& input = inputs[0];
        int ready;
        do {
            ready = ::poll(inputs, 2, -1);
        } while (ready < 0 && errno == EINTR && m_running.load());
        if (! m_running.load()) break;
        if (ready == 0) continue;
        if (ready < 0 || (input.revents & (POLLERR | POLLNVAL)) != 0) break;
        if ((input.revents & (POLLIN | POLLHUP)) == 0) continue;

        const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            // Match getline semantics: accept one final command without a
            // newline before treating EOF as parent termination.
            if (! pending.empty()) dispatchLine(pending.c_str());
            break;
        }
        pending.append(buffer, (std::size_t)count);
        for (std::size_t newline; (newline = pending.find('\n')) != std::string::npos;) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (! line.empty() && line.back() == '\r') line.pop_back();
            if (! line.empty()) dispatchLine(line.c_str());
            if (! m_running.load()) break;
        }
        // Commands are tiny JSON objects. Bound an unterminated/malformed
        // stream so a broken parent cannot grow the renderer indefinitely.
        if (pending.size() > 1024 * 1024) pending.clear();
    }
    if (m_running.exchange(false)) {
        // Reached here via EOF (not an explicit quit) — still tell the host.
        if (m_on_quit) m_on_quit();
    }
}

} // namespace mirage

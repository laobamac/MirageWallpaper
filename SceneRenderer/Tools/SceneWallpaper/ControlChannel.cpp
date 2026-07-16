#include "ControlChannel.h"

#include <cerrno>
#include <array>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <string>
#include <unistd.h>

import sr.json;
import rstd.cppstd;
import sr.scene_wallpaper; // re-exports sr.types (FillMode)

namespace mirage {

namespace {

// Maps the wire fill-mode name (matching WE / the other renderers' vocabulary)
// to sr::FillMode. cover→ASPECTCROP, contain/fit→ASPECTFIT, stretch→STRETCH.
bool ParseFillMode(const std::string& s, sr::FillMode& out) {
    if (s.compare("cover") == 0 || s.compare("aspectcrop") == 0 || s.compare("crop") == 0) {
        out = sr::FillMode::ASPECTCROP;
        return true;
    }
    if (s.compare("contain") == 0 || s.compare("fit") == 0 || s.compare("aspectfit") == 0) {
        out = sr::FillMode::ASPECTFIT;
        return true;
    }
    if (s.compare("stretch") == 0) {
        out = sr::FillMode::STRETCH;
        return true;
    }
    return false;
}

} // namespace

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

    if (cmd.compare("setProperty") == 0) {
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
            prop = sr::Json::Object(rstd::move(object));
        } else if (value.is_some()) {
            prop = (*value)->clone();
        } else {
            return;
        }
        m_wallpaper.setUserPropertyJson(key, std::move(prop));
    } else if (cmd.compare("pause") == 0) {
        m_wallpaper.pause();
    } else if (cmd.compare("resume") == 0 || cmd.compare("play") == 0) {
        m_wallpaper.play();
    } else if (cmd.compare("volume") == 0) {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_number()) {
            auto number = (*value)->as_f64();
            if (number.is_some()) m_wallpaper.setVolume(static_cast<float>(*number));
        }
    } else if (cmd.compare("muted") == 0) {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_boolean()) {
            m_wallpaper.setMuted(*(*value)->as_bool());
        }
    } else if (cmd.compare("fps") == 0) {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_number()) {
            auto number = (*value)->as_u64();
            if (number.is_some()) m_wallpaper.setFps(static_cast<std::uint32_t>(*number));
        }
    } else if (cmd.compare("fillmode") == 0) {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_string()) {
            sr::FillMode mode {};
            if (ParseFillMode(rstd::cppstd::to_string(*(*value)->as_str()), mode)) {
                m_wallpaper.setFillMode(mode);
            }
        }
    } else if (cmd.compare("speed") == 0) {
        auto value = msg.get("value");
        if (value.is_some() && (*value)->is_number()) {
            auto number = (*value)->as_f64();
            if (number.is_some()) m_wallpaper.setSpeed(static_cast<float>(*number));
        }
    } else if (cmd.compare("audioSpectrum") == 0) {
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
    } else if (cmd.compare("activate") == 0) {
        if (m_on_activate) m_on_activate();
    } else if (cmd.compare("quit") == 0) {
        m_running.store(false);
        if (m_on_quit) m_on_quit();
    }
}

void SceneControlChannel::readLoop() {
    std::string pending;
    char        buffer[4096];
    while (m_running.load()) {
        pollfd input {
            .fd      = STDIN_FILENO,
            .events  = POLLIN | POLLHUP,
            .revents = 0,
        };
        int ready;
        do {
            ready = ::poll(&input, 1, 100);
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

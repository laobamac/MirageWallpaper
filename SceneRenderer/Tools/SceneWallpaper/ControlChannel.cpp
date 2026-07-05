#include "ControlChannel.h"

#include <cstdio>
#include <iostream>
#include <string>

import nlohmann.json;
import sr.scene_wallpaper; // re-exports sr.types (FillMode)

namespace mirage {

namespace {

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

void SceneControlChannel::dispatchLine(const char* line) {
    if (line == nullptr) return;

    auto msg = nlohmann::json::parse(line, /*cb*/ nullptr,
                                     /*allow_exceptions*/ false,
                                     /*ignore_comments*/ true);
    if (! msg.is_object() || ! msg.contains("cmd") || ! msg.at("cmd").is_string()) return;

    const std::string cmd = msg.at("cmd").get<std::string>();

    if (cmd == "setProperty") {
        if (! msg.contains("key") || ! msg.at("key").is_string()) return;
        const std::string key = msg.at("key").get<std::string>();

        // Build the property descriptor the runtime expects. If an explicit
        // "type" is present (e.g. color), wrap {type,value}; otherwise pass the
        // raw value (bool/number/string) — CoerceUserPropertyValue infers it.
        nlohmann::json prop;
        if (msg.contains("type") && msg.at("type").is_string()) {
            prop["type"] = msg.at("type");
            prop["value"] = msg.contains("value") ? msg.at("value") : nlohmann::json(nullptr);
        } else if (msg.contains("value")) {
            prop = msg.at("value");
        } else {
            return;
        }
        m_wallpaper.setUserPropertyJson(key, std::move(prop));
    } else if (cmd == "pause") {
        m_wallpaper.pause();
    } else if (cmd == "resume" || cmd == "play") {
        m_wallpaper.play();
    } else if (cmd == "volume") {
        if (msg.contains("value") && msg.at("value").is_number()) {
            m_wallpaper.setVolume(msg.at("value").get<float>());
        }
    } else if (cmd == "muted") {
        if (msg.contains("value") && msg.at("value").is_boolean()) {
            m_wallpaper.setMuted(msg.at("value").get<bool>());
        }
    } else if (cmd == "fps") {
        if (msg.contains("value") && msg.at("value").is_number()) {
            m_wallpaper.setFps(msg.at("value").get<std::uint32_t>());
        }
    } else if (cmd == "fillmode") {
        if (msg.contains("value") && msg.at("value").is_string()) {
            sr::FillMode mode {};
            if (ParseFillMode(msg.at("value").get<std::string>(), mode)) {
                m_wallpaper.setFillMode(mode);
            }
        }
    } else if (cmd == "speed") {
        if (msg.contains("value") && msg.at("value").is_number()) {
            m_wallpaper.setSpeed(msg.at("value").get<float>());
        }
    } else if (cmd == "quit") {
        m_running.store(false);
        if (m_on_quit) m_on_quit();
    }
}

void SceneControlChannel::readLoop() {
    std::string line;
    while (m_running.load()) {
        if (! std::getline(std::cin, line)) {
            // EOF or error: the parent closed the pipe (or died). Exit cleanly.
            break;
        }
        if (line.empty()) continue;
        dispatchLine(line.c_str());
    }
    if (m_running.exchange(false)) {
        // Reached here via EOF (not an explicit quit) — still tell the host.
        if (m_on_quit) m_on_quit();
    }
}

} // namespace mirage

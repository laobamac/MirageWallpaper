#include "SceneSnapshot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {

using LiveFrameCallback = void (*)(const std::uint8_t*, std::uint32_t, std::uint32_t, void*);
LiveFrameCallback live_frame_callback = nullptr;
void* live_frame_userdata = nullptr;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "SceneSnapshotRegression: " << message << '\n';
    return false;
}

} // namespace

extern "C" void SceneRendererSetLiveFrameCallback(LiveFrameCallback callback, void* userdata) {
    live_frame_callback = callback;
    live_frame_userdata = userdata;
}

int main() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("mirage-scene-snapshot-" + std::to_string(static_cast<long long>(::getpid())) + ".png");
    std::error_code remove_error;
    const bool previously_removed = std::filesystem::remove(path, remove_error);
    if (!Check(!remove_error, "cannot prepare snapshot path")) return 1;
    if (previously_removed) std::cout << "removed stale snapshot\n";

    const bool written = mirage::WriteSceneSnapshot(path.string(), 1.0, [] {
        const std::array<std::uint8_t, 8> pixels {
            255U, 0U, 0U, 255U,
            0U, 255U, 0U, 255U,
        };
        if (live_frame_callback != nullptr) {
            live_frame_callback(pixels.data(), 2U, 1U, live_frame_userdata);
        }
    });
    if (!Check(written, "snapshot write failed")) return 1;

    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, 24> header;
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!Check(input.gcount() == static_cast<std::streamsize>(header.size()),
               "PNG header is truncated")) return 1;
    const std::array<std::uint8_t, 8> signature {137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    if (!Check(std::equal(signature.begin(), signature.end(), header.begin()),
               "PNG signature does not match")) return 1;
    if (!Check(header[16] == 0U && header[17] == 0U && header[18] == 0U && header[19] == 2U &&
               header[20] == 0U && header[21] == 0U && header[22] == 0U && header[23] == 1U,
               "PNG dimensions do not match the live frame")) return 1;
    input.close();

    const bool timed_out = mirage::WriteSceneSnapshot(
        path.string() + ".timeout", 0.01, std::function<void()>());
    if (!Check(!timed_out, "capture without a frame must time out")) return 1;

    if (!std::filesystem::remove(path, remove_error) || remove_error) {
        std::cerr << "SceneSnapshotRegression: cannot remove test snapshot: "
                  << remove_error.message() << '\n';
        return 1;
    }
    return 0;
}

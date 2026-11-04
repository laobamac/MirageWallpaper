#include "../Vendor/wavsen/src/audio/audio_coreaudio_output.h"
#include "../Vendor/wavsen/src/audio/audio_coreaudio_capture.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>

namespace {
struct State {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned output_callbacks { 0 }, capture_callbacks { 0 };
};
void Output(float* samples, std::uint32_t frames, std::uint32_t channels, std::uint32_t, void* user) {
    std::fill(samples, samples + static_cast<std::size_t>(frames) * channels, 0.0f);
    auto& state = *static_cast<State*>(user);
    std::lock_guard lock(state.mutex);
    ++state.output_callbacks;
    state.changed.notify_all();
}
void Capture(const float*, std::uint32_t frames, std::uint32_t, std::uint32_t, void* user) {
    if (!frames) return;
    auto& state = *static_cast<State*>(user);
    std::lock_guard lock(state.mutex);
    ++state.capture_callbacks;
    state.changed.notify_all();
}
} // namespace

int main() {
    State state;
    wavsen_native_output* output = nullptr;
    wavsen_native_capture* capture = nullptr;
    int status = 0;
    std::uint32_t channels = 0, rate = 0;
    if (!wavsen_native_output_create(Output, &state, &output, &status, &channels, &rate)) {
        std::cerr << "WASAPI endpoint unavailable: " << status << '\n';
        return 77;
    }
    if (channels != 2 || rate != 48000 ||
        !wavsen_native_capture_create(Capture, &state, &capture, &status, nullptr, nullptr) ||
        !wavsen_native_output_start(output, &status)) {
        std::cerr << "WASAPI 初始化或启动失败: " << status << '\n';
        wavsen_native_capture_destroy(capture);
        wavsen_native_output_destroy(output);
        return 1;
    }
    bool active;
    {
        std::unique_lock lock(state.mutex);
        active = state.changed.wait_for(lock, std::chrono::seconds(3), [&] {
            return state.output_callbacks >= 3 && state.capture_callbacks >= 1;
        });
    }
    wavsen_native_output_stop(output);
    unsigned before;
    { std::lock_guard lock(state.mutex); before = state.output_callbacks; }
    const bool restarted = wavsen_native_output_start(output, &status);
    bool resumed;
    {
        std::unique_lock lock(state.mutex);
        resumed = state.changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return state.output_callbacks >= before + 3;
        });
    }
    wavsen_native_output_stop(output);
    wavsen_native_capture_destroy(capture);
    wavsen_native_output_destroy(output);
    if (!active || !restarted || !resumed) {
        std::cerr << "WASAPI 输出、loopback 或恢复回归失败\n";
        return 1;
    }
    std::cout << "WindowsWasapiRegression: ok output=" << state.output_callbacks
              << " capture=" << state.capture_callbacks << '\n' << std::flush;
    return 0;
}

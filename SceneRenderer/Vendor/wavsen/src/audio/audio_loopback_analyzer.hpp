#pragma once

#include <array>
#include <cstdint>

namespace wavsen::audio::loopback
{

struct SpectrumSnapshot {
    std::array<float, 64> left {};
    std::array<float, 64> right {};
    std::array<float, 64> average {};
    std::int64_t          publish_ms { 0 };

    void clear() {
        left.fill(0.0f);
        right.fill(0.0f);
        average.fill(0.0f);
        publish_ms = 0;
    }
};

void ingest(const float* src, std::uint32_t n_frames, std::uint32_t channels,
            std::uint32_t sample_rate);
bool snapshot(SpectrumSnapshot& out);

} // namespace wavsen::audio::loopback

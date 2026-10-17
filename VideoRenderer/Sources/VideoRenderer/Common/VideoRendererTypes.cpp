#include "VideoRendererTypes.h"

#include <cmath>

VRVideoEngineConfig VRDefaultVideoEngineConfig() noexcept {
    return {};
}

float VRClampVideoVolume(float value) noexcept {
    if (!std::isfinite(value)) return 1.0f;
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

bool VRParseVideoFillMode(std::string_view value, VRVideoFillMode& out) noexcept {
    if (value == "cover") {
        out = VRVideoFillModeCover;
        return true;
    }
    if (value == "contain" || value == "fit") {
        out = VRVideoFillModeContain;
        return true;
    }
    if (value == "stretch") {
        out = VRVideoFillModeStretch;
        return true;
    }
    return false;
}

#include "VideoRendererTypes.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "VideoRendererTypesTests: %s\n", message);
    return condition;
}

} // namespace

int main() {
    bool passed = true;
    const VRVideoEngineConfig config = VRDefaultVideoEngineConfig();
    passed &= expect(config.fillMode == VRVideoFillModeCover, "default fill mode");
    passed &= expect(config.initialVolume == 1.0f, "default volume");
    passed &= expect(!config.muted && config.autoplay, "default playback flags");

    passed &= expect(VRClampVideoVolume(-1.0f) == 0.0f, "negative volume clamp");
    passed &= expect(VRClampVideoVolume(2.0f) == 1.0f, "high volume clamp");
    passed &= expect(VRClampVideoVolume(0.25f) == 0.25f, "valid volume preserved");
    passed &= expect(VRClampVideoVolume(std::numeric_limits<float>::quiet_NaN()) == 1.0f,
                     "non-finite volume fallback");

    VRVideoFillMode mode = VRVideoFillModeCover;
    passed &= expect(VRParseVideoFillMode("contain", mode) && mode == VRVideoFillModeContain,
                     "contain fill mode");
    passed &= expect(VRParseVideoFillMode("fit", mode) && mode == VRVideoFillModeContain,
                     "fit alias");
    passed &= expect(VRParseVideoFillMode("stretch", mode) && mode == VRVideoFillModeStretch,
                     "stretch fill mode");
    passed &= expect(!VRParseVideoFillMode("invalid", mode), "invalid fill mode rejected");
    return passed ? 0 : 1;
}

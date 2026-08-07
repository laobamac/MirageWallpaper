// WE puppet per-bone opacity envelopes (`blend_curves` in the MDLA block).
//
// Despite the on-disk name these are not animation blend weights: the WE image
// shaders feed them to `g_BonesAlpha[]` and multiply the LBS-weighted result
// into the vertex alpha (`genericimage4.vert`). Reading them as pose-blend
// weights makes a bone snap back to its bind pose for the few frames the curve
// dips — which is how workshop 3396722575's eyelids stopped covering the pupil
// mid-blink.
//
// The fixtures mirror the real corpus:
//   * 3396722575 `眼睛_puppet.mdl` / `右眼_puppet.mdl` — 30fps 180-frame loop,
//     lid/lash bones whose curve drops to 0 for ~3 frames around frame 55.
//     Several of those bones hold their TRS completely still through the dip,
//     so under a pose-blend reading the curve is a mathematical no-op.
//   * 2910422008 `text_bg1_puppet.mdl` — single rigid bone, curve ramps 0 -> 1
//     while the sprite travels 300 units. Pose-blend would pin the sprite at
//     its bind pose for the whole animation instead of fading it in.

#include <cmath>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

import sr.puppet;
import eigen;

namespace
{

int g_failures = 0;

void Check(bool ok, std::string_view what) {
    if (ok) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << '\n';
}

bool Near(float actual, float expected, float epsilon = 0.001f) {
    return std::abs(actual - expected) <= epsilon;
}

using Puppet = sr::WPPuppet;
using Layers = std::vector<sr::WPPuppetLayer::AnimationLayer>;

Puppet::BoneFrame MakeFrame(Eigen::Vector3f pos) {
    Puppet::BoneFrame frame;
    frame.position = pos;
    frame.angle    = Eigen::Vector3f::Zero();
    frame.scale    = Eigen::Vector3f::Ones();
    return frame;
}

// One bone, `length + 1` frames. `travel` is the total x displacement spread
// linearly across the track, so callers get either a static bone (0) or a
// moving one.
Puppet::BoneTrack MakeTrack(int length, float travel) {
    Puppet::BoneTrack track;
    track.bone_index = 0;
    for (int f = 0; f <= length; ++f) {
        const float t = length == 0 ? 0.0f : static_cast<float>(f) / static_cast<float>(length);
        track.frames.push_back(MakeFrame({ travel * t, 0.0f, 0.0f }));
    }
    return track;
}

Layers OneLayer(int anim_id, double blend = 1.0) {
    sr::WPPuppetLayer::AnimationLayer alayer;
    alayer.id      = anim_id;
    alayer.rate    = 1.0;
    alayer.blend   = blend;
    alayer.visible = true;
    return Layers { alayer };
}

struct Sample {
    float pose_x;
    float alpha;
};

// The animation clock advances by the *delta* between successive genFrame
// calls, and the first call on a fresh layer only seeds that clock. So prime at
// t=0, then step straight to the frame under test — a single genFrame(t) would
// report frame 0 no matter what `t` is.
Sample SampleAt(const std::shared_ptr<Puppet>& puppet, const Layers& layers, double frame,
                double fps) {
    sr::WPPuppetLayer layer(puppet);
    Layers            mutable_layers = layers;
    layer.prepared(mutable_layers);
    (void)layer.genFrame(0.0);
    auto pose = layer.genFrame(frame / fps);
    return { pose[0].translation().x(), layer.boneAlphas()[0] };
}

std::shared_ptr<Puppet> MakePuppet(int anim_id, int length, double fps, float travel,
                                   std::span<const int> dip_frames) {
    auto puppet = std::make_shared<Puppet>();
    puppet->bones.resize(1);

    auto& anim  = puppet->anims.emplace_back();
    anim.id     = anim_id;
    anim.fps    = fps;
    anim.length = length;
    anim.mode   = Puppet::PlayMode::Loop;
    anim.bone_tracks.push_back(MakeTrack(length, travel));

    auto& curve = anim.blend_curves.emplace_back();
    curve.values.assign(static_cast<std::size_t>(length) + 1, 1.0f);
    for (int f : dip_frames) curve.values[static_cast<std::size_t>(f)] = 0.0f;

    puppet->prepared();
    return puppet;
}

} // namespace

// A curve that dips to zero must not disturb the pose, and must land on the
// bone's alpha instead.
void TestBlinkEnvelopeDrivesAlphaNotPose() {
    constexpr int    kLength = 180;
    constexpr double kFps    = 30.0;
    constexpr float  kTravel = 24.0f;
    // Real blinks span several frames; sampling mid-dip keeps the assertion off
    // a frame boundary where floating-point rounding could pick a neighbour.
    constexpr int kDip[] = { 54, 55, 56 };

    auto puppet = MakePuppet(193, kLength, kFps, kTravel, kDip);
    auto layers = OneLayer(193);
    Check(puppet->hasAlphaCurves(), "a curve that dips below 1 enables the alpha permutation");

    const auto before = SampleAt(puppet, layers, 50.0, kFps);
    Check(Near(before.alpha, 1.0f), "outside the blink the bone stays fully opaque");

    const auto during = SampleAt(puppet, layers, 55.5, kFps);
    Check(Near(during.alpha, 0.0f), "the blink frames drive the bone alpha to zero");
    Check(during.pose_x > before.pose_x,
          "the blink does not snap the pose back toward the bind pose");
    Check(Near(during.pose_x, kTravel * 55.5f / static_cast<float>(kLength), 0.01f),
          "the pose through the blink still matches the authored track");

    const auto after = SampleAt(puppet, layers, 60.0, kFps);
    Check(Near(after.alpha, 1.0f), "the bone returns to fully opaque after the blink");
}

// The bones that matter most for the eyelid case hold completely still while
// they fade. Under the old reading their curve was a no-op; it has to reach
// alpha now.
void TestStaticTrackStillFades() {
    constexpr int    kLength = 60;
    constexpr double kFps    = 30.0;
    constexpr int    kDip[]  = { 29, 30, 31 };

    auto puppet = MakePuppet(7, kLength, kFps, 0.0f, kDip);
    auto layers = OneLayer(7);

    const auto during = SampleAt(puppet, layers, 30.5, kFps);
    Check(Near(during.alpha, 0.0f),
          "a bone whose track never moves still fades out through its curve");
    Check(Near(during.pose_x, 0.0f), "fading a static bone does not move it");
}

// An all-1.0 curve is indistinguishable from no curve at all and must not
// switch the shader permutation on — that would declare g_BonesAlpha for the
// 34 of 38 installed puppets that never need it.
void TestFlatCurveLeavesPermutationAlone() {
    auto flat = MakePuppet(1, 10, 30.0, 5.0f, {});
    Check(! flat->hasAlphaCurves(), "an all-opaque curve does not enable the alpha permutation");

    auto no_curve = std::make_shared<Puppet>();
    no_curve->bones.resize(2);
    auto& plain  = no_curve->anims.emplace_back();
    plain.id     = 2;
    plain.fps    = 30.0;
    plain.length = 4;
    plain.mode   = Puppet::PlayMode::Loop;
    plain.bone_tracks.push_back(MakeTrack(4, 1.0f));
    no_curve->prepared();
    Check(! no_curve->hasAlphaCurves(), "a puppet without curves does not enable the permutation");

    // Bones the curve array doesn't cover must read as opaque, never as 0.
    sr::WPPuppetLayer layer(no_curve);
    Layers            layers = OneLayer(2);
    layer.prepared(layers);
    (void)layer.genFrame(0.0);
    (void)layer.genFrame(2.0 / 30.0);
    const auto alphas = layer.boneAlphas();
    Check(alphas.size() == 2 && Near(alphas[0], 1.0f) && Near(alphas[1], 1.0f),
          "bones with no curve default to fully opaque");
}

// A layer dialled down must not darken the sprite: blend scales the envelope
// toward opaque, so blend 0 means "this layer contributes no fade".
void TestLayerBlendScalesTowardOpaque() {
    constexpr int    kLength = 30;
    constexpr double kFps    = 30.0;
    constexpr int    kDip[]  = { 14, 15, 16 };

    auto puppet = MakePuppet(11, kLength, kFps, 3.0f, kDip);

    const auto full = SampleAt(puppet, OneLayer(11, 1.0), 15.5, kFps);
    const auto half = SampleAt(puppet, OneLayer(11, 0.5), 15.5, kFps);
    const auto none = SampleAt(puppet, OneLayer(11, 0.0), 15.5, kFps);

    Check(Near(full.alpha, 0.0f), "a fully blended layer applies its envelope verbatim");
    Check(Near(half.alpha, 0.5f), "a half-blended layer applies half the fade");
    Check(Near(none.alpha, 1.0f), "a zero-blend layer leaves the sprite opaque");
}

int main() {
    TestBlinkEnvelopeDrivesAlphaNotPose();
    TestStaticTrackStillFades();
    TestFlatCurveLeavesPermutationAlone();
    TestLayerBlendScalesTowardOpaque();
    if (g_failures == 0) std::cout << "PuppetAlphaRegression: ok\n";
    return g_failures == 0 ? 0 : 1;
}

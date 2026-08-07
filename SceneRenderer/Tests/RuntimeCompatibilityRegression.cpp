#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

import sr.scene;
import sr.fs;
import sr.json;
import sr.pkg.parse;
import sr.types;
import sr.vulkan;
import sr.vulkan_render;
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

sr::Json Parse(std::string_view source) {
    auto value = sr::ParseJson(source);
    if (value.is_ok()) return value.unwrap();
    ++g_failures;
    std::cerr << "FAIL: invalid test JSON\n";
    return sr::Json::Null();
}

class ProbeSound final : public sr::SceneSoundControl {
public:
    void Play() override {
        ++play_count;
        playing = true;
    }
    void Stop() override {
        ++stop_count;
        playing = false;
    }
    void Pause() override { playing = false; }
    bool IsPlaying() const override { return playing; }
    void SetVolume(float value) override { volume = value; }

    int   play_count { 0 };
    int   stop_count { 0 };
    bool  playing { true };
    float volume { 1.0f };
};

void TestExplicitCameraFactories() {
    auto ortho = sr::SceneCamera::MakeOrthographic(1920.5, 1080.25, -1.0, 1.0);
    Check(! ortho.IsPerspective(), "orthographic factory selects orthographic projection");
    Check(ortho.Width() == 1920.5, "orthographic factory preserves fractional width");
    Check(ortho.Height() == 1080.25, "orthographic factory preserves fractional height");

    auto perspective = sr::SceneCamera::MakePerspective(16.0 / 9.0, 0.01, 1000.0, 45.0);
    Check(perspective.IsPerspective(), "perspective factory selects perspective projection");
    Check(perspective.Aspect() == 16.0 / 9.0, "perspective factory preserves aspect");
    Check(perspective.Fov() == 45.0, "perspective factory preserves field of view");
}

void TestAuthoredSceneZoom() {
    sr::Scene scene;
    scene.ortho[0] = 1920;
    scene.ortho[1] = 1080;
    scene.SetViewportScale(1.5f);
    auto extent = scene.OrthographicProjectionExtent();
    Check(extent[0] == 1280.0 && extent[1] == 720.0,
          "authored zoom scales the orthographic projection extent");
    scene.SetViewportScale(0.0f);
    extent = scene.OrthographicProjectionExtent();
    Check(extent[0] == 1920.0 && extent[1] == 1080.0,
          "invalid authored zoom falls back to unity");
}

void TestPlanarReflectionSemantics() {
    auto camera = sr::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 1000.0, 50.0);
    camera.SetLookAt({ 1.0, 2.0, 3.0 }, { 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 });
    const auto primary_eye    = camera.GetPosition();
    const auto reflection_eye = camera.GetPosition(sr::SceneRenderViewKind::Reflection);
    Check(primary_eye.x() == reflection_eye.x() && primary_eye.y() == -reflection_eye.y() &&
              primary_eye.z() == reflection_eye.z(),
          "reflection camera mirrors only the world Y coordinate");
    const auto primary_vp = camera.GetViewProjectionMatrix();
    const auto reflection_vp =
        camera.GetViewProjectionMatrix(sr::SceneRenderViewKind::Reflection);
    Check(primary_vp.allFinite() && reflection_vp.allFinite() &&
              ! primary_vp.isApprox(reflection_vp),
          "reflection view-projection is finite and distinct from the primary view");

    sr::Scene scene;
    scene.ortho[0] = 1280;
    scene.ortho[1] = 720;
    scene.renderTargets["_rt_default"] = { .width = 2560, .height = 1440 };
    scene.EnablePlanarReflection();
    scene.EnablePlanarReflection();
    const auto reflection = scene.renderTargets.find("_rt_Reflection");
    Check(scene.PlanarReflectionEnabled(), "reflection texture enables the planar render stage");
    Check(reflection != scene.renderTargets.end(), "reflection render target is registered");
    if (reflection != scene.renderTargets.end()) {
        Check(reflection->second.width == 2560 && reflection->second.height == 1440,
              "reflection render target follows the primary target extent");
        Check(reflection->second.withDepth && reflection->second.bind.screen &&
                  reflection->second.preserve_on_write,
              "reflection target keeps depth and accumulated reflected layers");
    }

    sr::SceneNode node;
    Check(! node.Reflected(), "generated scene nodes are not reflected by default");
    node.SetReflected(true);
    Check(node.Reflected(), "authored reflection state is retained on scene nodes");

    Check(sr::wpscene::ImageObject {}.reflected && sr::wpscene::TextObject {}.reflected &&
              sr::wpscene::ModelObject {}.reflected && sr::wpscene::ParticleObject {}.reflected,
          "authored renderable layer kinds preserve Wallpaper Engine's reflected default");
}

void TestWrappedAnimationCurves() {
    sr::SceneAnimationCurve tail_wrap;
    tail_wrap.fps      = 1.0f;
    tail_wrap.length   = 4;
    tail_wrap.mode     = "loop";
    tail_wrap.wraploop = true;
    tail_wrap.c0.push_back({ .frame = 0, .value = 0.0f });
    tail_wrap.c0.push_back({ .frame = 2, .value = 10.0f });
    Check(Near(tail_wrap.EvaluateScalar(0.0f, 3.0), 5.0f),
          "wraploop interpolates last key back to first");

    sr::SceneAnimationCurve head_wrap;
    head_wrap.fps      = 1.0f;
    head_wrap.length   = 4;
    head_wrap.mode     = "loop";
    head_wrap.wraploop = true;
    head_wrap.c0.push_back({ .frame = 1, .value = 10.0f });
    head_wrap.c0.push_back({ .frame = 3, .value = 30.0f });
    Check(Near(head_wrap.EvaluateScalar(0.0f, 0.0), 20.0f),
          "wraploop interpolates previous cycle before first key");

    sr::SceneAnimationCurve mirror;
    mirror.fps      = 1.0f;
    mirror.length   = 2;
    mirror.mode     = "mirror";
    mirror.wraploop = true;
    mirror.c0.push_back({ .frame = 0, .value = 0.0f });
    mirror.c0.push_back({ .frame = 2, .value = 10.0f });
    Check(Near(mirror.EvaluateScalar(0.0f, 3.0), 5.0f),
          "mirror mode takes precedence over wraploop");
}

void TestFieldAnimationPlayback() {
    auto playback = std::make_shared<sr::SceneAnimationPlayback>(
        "face", 10.0f, 10, "single", false, true);

    sr::SceneAnimationCurve origin;
    origin.fps      = 10.0f;
    origin.length   = 10;
    origin.mode     = "single";
    origin.relative = true;
    origin.playback = playback;
    origin.c0.push_back({ .frame = 0, .value = 0.0f });
    origin.c0.push_back({ .frame = 10, .value = 10.0f });

    sr::SceneAnimationCurve angles;
    angles.fps      = 10.0f;
    angles.length   = 10;
    angles.mode     = "single";
    angles.playback = playback;
    angles.c2.push_back({ .frame = 0, .value = 0.0f });
    angles.c2.push_back({ .frame = 10, .value = 1.0f });

    sr::SceneNode node;
    node.SetOriginAnimation(std::move(origin));
    node.SetRotationAnimation(std::move(angles));
    Check(node.FindAnimation("face") == playback, "named field animation is registered");

    node.TickFieldAnimations(0.0);
    node.TickFieldAnimations(1.0);
    Check(Near(node.Translate().x(), 0.0f) && Near(node.Rotation().z(), 0.0f),
          "start-paused combined animation remains at frame zero");

    playback->Play();
    node.TickFieldAnimations(1.5);
    Check(Near(playback->Frame(), 5.0f) && Near(node.Translate().x(), 5.0f) &&
              Near(node.Rotation().z(), 0.5f),
          "combined animation tracks share one playback frame");

    playback->Pause();
    node.TickFieldAnimations(2.0);
    Check(Near(playback->Frame(), 5.0f) && Near(node.Translate().x(), 5.0f),
          "paused animation holds its current frame");

    playback->SetFrame(8.0);
    node.TickFieldAnimations(2.0);
    Check(Near(node.Translate().x(), 8.0f) && Near(node.Rotation().z(), 0.8f),
          "setFrame applies to every combined track");

    playback->Stop();
    node.TickFieldAnimations(2.0);
    Check(Near(playback->Frame(), 0.0f) && Near(node.Translate().x(), 0.0f),
          "stop restores the first frame");

    playback->SetRate(2.0);
    playback->Play();
    node.TickFieldAnimations(2.25);
    node.TickFieldAnimations(2.5);
    Check(playback->Status() == sr::SceneAnimationPlaybackStatus::Completed &&
              Near(playback->Frame(), 10.0f) && Near(node.Translate().x(), 10.0f),
          "single animation completes at and holds the last frame");

    playback->Play();
    node.TickFieldAnimations(2.5);
    Check(playback->IsPlaying() && Near(playback->Frame(), 0.0f) &&
              Near(node.Translate().x(), 0.0f),
          "completed single animation replays from frame zero");
}

void TestSoundVisibilityAndVolume() {
    sr::SceneNode node;
    node.SetVolume(0.4f);
    Check(Near(node.Volume(), 0.4f), "sound volume is retained before playback is attached");
    auto          sound = std::make_shared<ProbeSound>();
    node.SetSoundControl(sound);
    Check(Near(sound->volume, 0.4f), "attaching playback applies the retained sound volume");

    node.SetVisible(false);
    Check(sound->stop_count == 1 && ! sound->playing, "hiding sound layer stops playback");
    node.SetVisible(false);
    Check(sound->stop_count == 1, "repeated hide does not restart sound state");
    node.SetVisible(true);
    Check(sound->play_count == 1 && sound->playing, "showing sound layer resumes playback");

    node.SetVolume(2.0f);
    Check(Near(node.Volume(), 1.0f) && Near(sound->volume, 1.0f),
          "sound volume actuator clamps and stores the upper bound");
    node.SetVolume(-1.0f);
    Check(Near(node.Volume(), 0.0f) && Near(sound->volume, 0.0f),
          "sound volume actuator clamps and stores the lower bound");
}

void TestCameraTransformControls() {
    auto camera = sr::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 1000.0, 50.0);
    const sr::SceneCameraTransforms authored {
        .eye = { 2.0, 3.0, 4.0 },
        .center = { -1.0, 0.5, 0.0 },
        .up = { 0.0, 1.0, 0.0 },
    };
    Check(camera.SetTransforms(authored), "camera accepts finite non-degenerate transforms");
    const auto roundtrip = camera.Transforms();
    Check(roundtrip.eye.isApprox(authored.eye) && roundtrip.center.isApprox(authored.center) &&
              roundtrip.up.isApprox(authored.up),
          "camera transform controls preserve eye, center and up");

    auto degenerate = authored;
    degenerate.center = degenerate.eye;
    Check(! camera.SetTransforms(degenerate), "camera rejects a zero-length view direction");
    degenerate        = authored;
    degenerate.up     = authored.center - authored.eye;
    Check(! camera.SetTransforms(degenerate), "camera rejects a collinear up vector");

    sr::Scene scene;
    auto      primary = std::make_shared<sr::SceneCamera>(camera);
    auto      linked  = std::make_shared<sr::SceneCamera>(camera);
    scene.cameras.emplace("primary", primary);
    scene.cameras.emplace("linked", linked);
    scene.linkedCameras["primary"].push_back("linked");
    scene.activeCamera = primary.get();

    const sr::SceneCameraTransforms changed {
        .eye = { 8.0, 7.0, 6.0 },
        .center = { 0.0, 1.0, 0.0 },
        .up = { 0.0, 1.0, 1.0 },
    };
    Check(scene.SetActiveCameraTransforms(changed),
          "scene applies transforms to the active camera");
    const auto active = scene.ActiveCameraTransforms();
    Check(active.has_value() && active->eye.isApprox(changed.eye) &&
              linked->Transforms().eye.isApprox(changed.eye),
          "active camera transforms propagate to linked cameras");
}

void TestMaterialKeyAliases() {
    sr::Scene         scene;
    sr::SceneMaterial material;
    sr::SceneShaderVariantDesc variant;
    variant.uniform_aliases["Tint"] = "g_Tint";
    material.customShader.variant   = std::move(variant);
    material.customShader.constValues["g_Tint"] =
        sr::ShaderValue(std::array<float, 3> { 1.0f, 1.0f, 1.0f });

    Check(scene.SetMaterialShaderValueByKey(
              material, "Tint", sr::ShaderValue(std::array<float, 3> { 0.2f, 0.4f, 0.6f })),
          "material key writes resolve through shader uniform aliases");
    const auto& value = material.customShader.constValues.at("g_Tint");
    Check(value.size() == 3 && Near(value[0], 0.2f) && Near(value[1], 0.4f) &&
              Near(value[2], 0.6f),
          "material alias writes update the resolved uniform");
    Check(! material.customShader.constValues.contains("Tint"),
          "material alias writes do not create an unresolved duplicate");
}

void TestLimitedStreamSeekSemantics() {
    std::vector<uint8_t> bytes(12, 0);
    auto backing = std::make_shared<sr::fs::MemBinaryStream>(std::move(bytes));
    sr::fs::LimitedBinaryStream stream(backing, 3, 6);

    Check(stream.SeekEnd(0) && stream.Tell() == 6,
          "limited stream seek-to-end reaches the position after the final byte");
    Check(stream.SeekEnd(-2) && stream.Tell() == 4,
          "limited stream negative end seek uses standard relative offsets");
    Check(! stream.SeekEnd(1) && stream.Tell() == 4,
          "limited stream rejects positions past its logical end");
}

void TestMaterialAndModelSchemaCompatibility() {
    sr::wpscene::ObjectInstance instance;
    Check(instance.FromJson(Parse(
              R"JSON({"textures":["override-a",null,"override-c"],"combos":{"MODE":2}})JSON")),
          "material instance overrides parse");
    sr::wpscene::Material instance_material;
    instance_material.textures = { "base-a", "base-b" };
    instance.ApplyTo(instance_material);
    Check(instance_material.textures.size() == 3 &&
              instance_material.textures[0] == "override-a" &&
              instance_material.textures[1] == "base-b" &&
              instance_material.textures[2] == "override-c",
          "material instances preserve empty texture slots while applying overrides");
    Check(instance_material.combos.at("MODE") == 2,
          "material instances apply authored combo overrides");

    sr::wpscene::Material scripted_material;
    Check(scripted_material.FromJson(Parse(R"JSON({
        "passes":[{
            "shader":"unit",
            "constantshadervalues":{
                "Tint":{
                    "value":[0.1,0.2,0.3,0.4],
                    "script":"export function update() { return new Vec4(1, 2, 3, 4); }",
                    "scriptproperties":{"speed":1}
                }
            }
        }]
    })JSON")),
          "scripted material constant parses");
    const auto binding = scripted_material.constantshadervalues_bindings.scripts.find("Tint");
    Check(binding != scripted_material.constantshadervalues_bindings.scripts.end() &&
              ! binding->second.source.empty(),
          "material constants retain their field script bindings");
    auto cloned_material = scripted_material.clone();
    Check(cloned_material.constantshadervalues_bindings.scripts.contains("Tint"),
          "material clones retain constant field script bindings");

    sr::fs::VFS            vfs;
    sr::wpscene::ModelObject model;
    Check(model.FromJson(Parse(R"JSON({"model":"models/unit.mdl","skin":3})JSON"), vfs) &&
              model.skin == 3,
          "model objects retain their authored material skin index");
}

void TestObjectSpaceRotation() {
    sr::SceneNode node;
    node.RotateObjectSpace({ 0.0f, 0.0f, static_cast<float>(std::numbers::pi / 2.0) });
    Check(Near(node.Rotation().x(), 0.0f) && Near(node.Rotation().y(), 0.0f) &&
              Near(node.Rotation().z(), static_cast<float>(std::numbers::pi / 2.0)),
          "object-space rotation composes onto the authored node orientation");
}

void TestShortShaderVectorShaping() {
    sr::SceneMaterial material;
    material.customShader.constValues["g_Test"] =
        sr::ShaderValue(std::array<float, 4> { 9.0f, 9.0f, 9.0f, 9.0f });
    material.SetShaderValue("g_Test", sr::ShaderValue(std::array<float, 2> { 1.0f, 2.0f }));
    const auto& vector = material.customShader.constValues.at("g_Test");
    Check(vector.size() == 4, "short shader vector retains declared width");
    Check(Near(vector[0], 1.0f) && Near(vector[1], 2.0f) && Near(vector[2], 0.0f) &&
              Near(vector[3], 0.0f),
          "short shader vector preserves supplied components and zero-fills tail");

    material.SetShaderValue("g_Test", sr::ShaderValue(0.25f));
    const auto& scalar = material.customShader.constValues.at("g_Test");
    Check(Near(scalar[0], 0.25f) && Near(scalar[1], 0.25f) && Near(scalar[2], 0.25f) &&
              Near(scalar[3], 0.25f),
          "scalar shader value still splats to declared vector width");
}

void TestAlphaToCoveragePipelineState() {
    VkPipelineColorBlendAttachmentState blend {};
    sr::vulkan::SetBlend(sr::BlendMode::AlphaToCoverage, blend);
    Check(blend.blendEnable == VK_FALSE, "alpha-to-coverage disables conventional blending");

    VkPipelineMultisampleStateCreateInfo multisample {};
    sr::vulkan::SetAlphaToCoverage(sr::BlendMode::AlphaToCoverage, multisample);
    Check(multisample.alphaToCoverageEnable == VK_TRUE,
          "alpha-to-coverage enables multisample coverage conversion");

    VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    sr::vulkan::SetAttachmentLoadOp(sr::BlendMode::AlphaToCoverage, load_op);
    Check(load_op == VK_ATTACHMENT_LOAD_OP_LOAD,
          "alpha-to-coverage preserves the existing color attachment");
    Check(sr::vulkan::IsDepthWritingBlendMode(sr::BlendMode::AlphaToCoverage),
          "alpha-to-coverage retains material depth writes");
}

void TestTextSurfaceSelection() {
    Check(sr::ResolveTextRenderMode({}) == sr::TextRenderMode::Direct,
          "ordinary text renders directly");
    Check(sr::ResolveTextRenderMode({ .has_effect = true }) == sr::TextRenderMode::Offscreen,
          "text effects retain an independent surface");
    Check(sr::ResolveTextRenderMode({ .copy_background = true }) == sr::TextRenderMode::Offscreen,
          "copy-background text retains an independent surface");
    Check(sr::ResolveTextRenderMode({ .opaque_background = true }) == sr::TextRenderMode::Offscreen,
          "opaque-background text retains an independent surface");
    Check(sr::ResolveTextRenderMode({ .linked_source = true }) == sr::TextRenderMode::Offscreen,
          "linked text retains a sampleable surface");
}

void TestDirectShapeLayerState() {
    sr::SceneNode             node;
    sr::SceneImageEffectLayer layer(&node, 1920.0f, 1080.0f, "shape_a", "shape_b");
    Check(layer.RequiresSourceDraw(), "ordinary effect layer draws its image source");
    layer.SetRequiresSourceDraw(false);
    Check(! layer.RequiresSourceDraw(), "direct shape effect layer suppresses image source draw");
    Check(sr::wpscene::ShapeObject {}.reflected,
          "shape layers participate in planar reflection by default");

    sr::SceneNode source;
    Eigen::Matrix4d frame = Eigen::Matrix4d::Identity();
    frame(0, 3)          = 42.0;
    source.SetLocalFrame(frame);
    sr::SceneNode copy;
    copy.CopyTrans(source);
    Check(copy.LocalFrame().isApprox(frame), "transform copies preserve puppet attachment frame");
}

void TestParticleRuntimeState() {
    using SpawnType = sr::ParticleSubSystem::SpawnType;
    Check(sr::ParticleSubSystem::EffectiveInstanceCapacity(12, SpawnType::STATIC) == 1,
          "static particle systems allocate one persistent instance");
    Check(sr::ParticleSubSystem::EffectiveInstanceCapacity(12, SpawnType::EVENT_SPAWN) == 12,
          "event particle systems retain their authored instance pool");
    const auto capacity =
        sr::ParticleSubSystem::MaxParticleCapacity(256, 12, SpawnType::EVENT_SPAWN);
    Check(capacity.has_value() && *capacity == 3072,
          "particle mesh capacity includes every event instance");
    Check(! sr::ParticleSubSystem::MaxParticleCapacity(
               std::numeric_limits<uint32_t>::max(), 2, SpawnType::EVENT_SPAWN)
               .has_value(),
          "particle mesh capacity rejects integer overflow");

    sr::ParticleTrail trail;
    trail.positions.resize(3);
    trail.Initialize({ 1.0f, 2.0f, 3.0f });
    Check(trail.sample_count == 1 && trail.len == 3,
          "rope trail initialization exposes one real sample without uninitialized history");
    trail.Push({ 2.0f, 3.0f, 4.0f });
    trail.Push({ 3.0f, 4.0f, 5.0f });
    Check(trail.sample_count == 3 && trail.At(0).isApprox(Eigen::Vector3f(1.0f, 2.0f, 3.0f)) &&
              trail.At(2).isApprox(Eigen::Vector3f(3.0f, 4.0f, 5.0f)),
          "rope trail ring buffer preserves oldest-to-newest sample order");

    sr::SceneNode node;
    bool          playing = true;
    float         alpha   = 1.0f;
    node.SetLayerPropertyControl(
        [&alpha](std::string_view field) {
            return field == "alpha" ? std::vector<float> { alpha } : std::vector<float> {};
        },
        [&alpha](std::string_view field, std::span<const float> values) {
            if (field == "alpha" && ! values.empty()) alpha = values.front();
        });
    node.SetPlaybackControl(
        [&playing]() { playing = true; },
        [&playing]() { playing = false; },
        [&playing]() { playing = false; },
        [&playing]() { return playing; });
    const std::array<float, 1> changed_alpha { 0.25f };
    Check(node.ApplyControlledProperty("alpha", changed_alpha),
          "particle node accepts instance override writes");
    const auto read_alpha = node.ControlledProperty("alpha");
    Check(read_alpha.has_value() && Near(read_alpha->front(), 0.25f),
          "particle node returns the current instance override value");
    node.Pause();
    Check(! node.IsPlaying(), "particle playback control pauses the shared particle tree");
    node.Play();
    Check(node.IsPlaying(), "particle playback control resumes the shared particle tree");
}

} // namespace

int main() {
    TestExplicitCameraFactories();
    TestAuthoredSceneZoom();
    TestPlanarReflectionSemantics();
    TestWrappedAnimationCurves();
    TestFieldAnimationPlayback();
    TestSoundVisibilityAndVolume();
    TestCameraTransformControls();
    TestMaterialKeyAliases();
    TestLimitedStreamSeekSemantics();
    TestMaterialAndModelSchemaCompatibility();
    TestObjectSpaceRotation();
    TestShortShaderVectorShaping();
    TestAlphaToCoveragePipelineState();
    TestTextSurfaceSelection();
    TestDirectShapeLayerState();
    TestParticleRuntimeState();
    if (g_failures == 0) std::cout << "RuntimeCompatibilityRegression: ok\n";
    return g_failures == 0 ? 0 : 1;
}

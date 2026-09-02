#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

import sr.scene;
import sr.scene_wallpaper;
import sr.fs;
import sr.json;
import sr.pkg.parse;
import sr.pkg_fs;
import sr.script;
import sr.scene_uniform_updater;
import sr.spec_texs;
import sr.types;
import sr.vulkan;
import sr.vulkan_render;
import sr.rgraph;
import eigen;
import rstd;
import wavsen.audio;

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

class ProbeImageParser final : public sr::IImageParser {
public:
    bool contains { false };

    bool Contains(const std::string&) const override { return contains; }
    std::shared_ptr<sr::Image> Parse(const std::string&) override { return nullptr; }
    sr::ImageHeader ParseHeader(const std::string&) override { return {}; }
};

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

void TestPerspectiveFillModePreservesFov() {
    sr::Scene scene;
    scene.SetProjectionKind(sr::SceneProjectionKind::Perspective3D);
    scene.ortho[0] = 1920;
    scene.ortho[1] = 1080;
    scene.cameras["global"] = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(1920.0, 1080.0, -5000.0, 5000.0));
    auto perspective = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakePerspective(16.0 / 9.0, 0.01, 10000.0, 50.0));
    sr::SceneNode camera_node;
    perspective->AttatchNode(&camera_node);
    scene.cameras["global_perspective"] = perspective;
    scene.activeCamera = perspective.get();

    sr::vulkan::UpdateCameraFillModeForExtent(
        scene, sr::FillMode::ASPECTCROP, 1920, 1080);

    Check(Near(perspective->Fov(), 50.0f),
          "perspective scene fill mode preserves authored field of view");
    Check(Near(perspective->Aspect(), 16.0f / 9.0f),
          "perspective scene fill mode updates output aspect");
}

void TestOrthographicFillModeDerivesPerspectiveFov() {
    sr::Scene scene;
    scene.SetProjectionKind(sr::SceneProjectionKind::OrthographicCanvas);
    scene.ortho[0] = 1920;
    scene.ortho[1] = 1080;
    scene.cameras["global"] = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(1920.0, 1080.0, -5000.0, 5000.0));
    auto perspective = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakePerspective(16.0 / 9.0, 5.0, 15000.0, 50.0));
    sr::SceneNode camera_node;
    perspective->AttatchNode(&camera_node);
    scene.cameras["global_perspective"] = perspective;

    sr::vulkan::UpdateCameraFillModeForExtent(
        scene, sr::FillMode::ASPECTCROP, 1920, 1080);

    const float expected = static_cast<float>(
        std::atan(1080.0 / 1000.0 / 2.0) * 2.0 * 180.0 / std::numbers::pi);
    Check(Near(perspective->Fov(), expected),
          "orthographic scene fill mode derives embedded perspective field of view");
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

sr::SceneAnimationCurve RootZoomCurve(float final_zoom) {
    sr::SceneAnimationCurve curve;
    curve.fps    = 30.0f;
    curve.length = 450;
    curve.mode   = "single";
    curve.c0.push_back({ .frame = 0, .value = 3.0f });
    curve.c0.push_back({ .frame = 300, .value = 3.0f });
    curve.c0.push_back({ .frame = 450, .value = final_zoom });
    return curve;
}

void TestAnimatedSceneZoom() {
    sr::Scene scene;
    scene.SetViewportScale(3.0f);
    scene.SetViewportScaleAnimation(RootZoomCurve(1.0f));
    auto global = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(100.0, 50.0, -1.0, 1.0));
    auto linked = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(1.0, 1.0, -1.0, 1.0));
    scene.cameras["global"] = global;
    scene.cameras["linked"] = linked;
    scene.linkedCameras["global"].push_back("linked");
    scene.CaptureCameraPathViewports();

    scene.elapsingTime = 0.0;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 100.0f) && Near(global->Height(), 50.0f),
          "root zoom animation preserves the authored initial close-up");

    scene.elapsingTime = 10.0;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 100.0f) && Near(global->Height(), 50.0f),
          "root zoom animation holds the initial value through frame 300");

    scene.elapsingTime = 15.0;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 300.0f) && Near(global->Height(), 150.0f),
          "root zoom animation expands the viewport when zoom reaches one");
    Check(Near(linked->Width(), 300.0f) && Near(linked->Height(), 150.0f),
          "root zoom animation propagates to linked cameras");

    global->SetWidth(200.0);
    global->SetHeight(100.0);
    scene.CaptureCameraPathViewports();
    scene.TickCameraPaths();
    Check(Near(global->Width(), 600.0f) && Near(global->Height(), 300.0f),
          "root zoom baseline recapture does not apply the animated ratio twice");

    scene.cameras["global_perspective"] = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 100.0, 50.0));
    sr::vulkan::UpdateCameraFillModeForExtent(
        scene, sr::FillMode::ASPECTCROP, 1920, 1080);
    Check(Near(global->Width(), 1920.0f) && Near(global->Height(), 1080.0f),
          "fill mode refresh preserves the current animated scene zoom");
}

void TestAnimatedSceneZoomWithCameraPath() {
    sr::Scene scene;
    scene.SetViewportScale(3.0f);
    scene.SetViewportScaleAnimation(RootZoomCurve(1.0f));
    auto global = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(120.0, 60.0, -1.0, 1.0));
    scene.cameras["global"] = global;

    sr::SceneNode node;
    auto path          = std::make_shared<sr::SceneCameraPath>();
    path->camera_name  = "global";
    path->camera       = global;
    path->node         = &node;
    path->zoom_base    = 2.0f;
    path->zoom_curve   = RootZoomCurve(4.0f);
    scene.camera_paths = { path };
    scene.CaptureCameraPathViewports();

    scene.elapsingTime = 15.0;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 90.0f) && Near(global->Height(), 45.0f),
          "root zoom composes multiplicatively with camera object zoom");

    path->enabled = false;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 360.0f) && Near(global->Height(), 180.0f),
          "disabled camera paths keep the root zoom animation active");
}

void TestInvalidAnimatedSceneZoom() {
    sr::Scene scene;
    scene.SetViewportScale(3.0f);
    scene.SetViewportScaleAnimation(RootZoomCurve(-1.0f));
    auto global = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(100.0, 50.0, -1.0, 1.0));
    scene.cameras["global"] = global;
    scene.CaptureCameraPathViewports();
    scene.elapsingTime = 15.0;
    scene.TickCameraPaths();
    Check(std::isfinite(global->Width()) && std::isfinite(global->Height()) &&
              global->Width() > 0.0 && global->Height() > 0.0,
          "invalid animated root zoom remains finite and positive");
}

void TestPointerUniformsIgnoreParallaxDelay() {
    sr::Scene scene;
    scene.frameTime = 1.0 / 30.0;
    auto camera = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(1920.0, 1080.0, -1.0, 1.0));
    scene.activeCamera = camera.get();

    sr::SceneUniformUpdater updater(&scene);
    updater.SetCameraParallax({ .enable = false, .amount = 0.01f, .delay = 0.9f,
                                .mouseinfluence = 0.5f });

    sr::SceneNode node;
    auto mesh = std::make_shared<sr::SceneMesh>();
    mesh->AddMaterial(sr::SceneMaterial {});
    node.AddMesh(mesh);
    updater.InitUniforms(&node, [](std::string_view name) {
        return name == "g_PointerPosition" || name == "g_PointerPositionLast";
    });

    sr::sprite_map_t sprites;
    std::array<float, 2> current {};
    std::array<float, 2> previous {};
    auto capture = [&](std::string_view name, sr::ShaderValue value) {
        if (name == "g_PointerPosition") current = { value[0], value[1] };
        if (name == "g_PointerPositionLast") previous = { value[0], value[1] };
    };

    updater.MouseInput(0.2, 0.8);
    updater.FrameBegin();
    updater.UpdateUniforms(&node, sprites, capture, sr::SceneRenderViewKind::Primary,
                           sr::SceneRenderAlphaMode::Composite);
    Check(Near(current[0], 0.2f) && Near(current[1], 0.8f) &&
              Near(previous[0], 0.2f) && Near(previous[1], 0.8f),
          "first pointer sample initializes current and previous uniforms");

    updater.MouseInput(0.85, 0.15);
    updater.FrameBegin();
    updater.UpdateUniforms(&node, sprites, capture, sr::SceneRenderViewKind::Primary,
                           sr::SceneRenderAlphaMode::Composite);
    Check(Near(current[0], 0.85f) && Near(current[1], 0.15f),
          "pointer uniform uses the current raw frame sample");
    Check(Near(previous[0], 0.2f) && Near(previous[1], 0.8f),
          "previous pointer uniform uses the preceding raw frame sample");
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

void TestJsonArraysAndSceneDocumentMetadata() {
    auto root = Parse(R"JSON({
        "camera": {},
        "general": {
            "clearcolor": [0.1, 0.2, 0.3],
            "orthogonalprojection": {"width": 1920, "height": 1080}
        },
        "objects": [
            {
                "id": 42,
                "name": "Shape",
                "shape": "rectangle",
                "visible": {"value": false, "user": {"name": "show_shape"}},
                "solid": true,
                "origin": [1.0, 2.0, 3.0]
            },
            {"name": "Container"}
        ]
    })JSON");
    auto document = sr::wpscene::ParseSceneDocumentValue(
        std::move(root), sr::wpscene::kSceneVersionUnknown);
    Check(document.has_value(), "scene document accepts authored JSON arrays");
    if (! document) return;

    Check(document->metadata.general.clearcolor == std::array<float, 3> { 0.1f, 0.2f, 0.3f },
          "fixed-size numeric fields decode from JSON arrays");
    Check(document->objects_are_array && document->objects.size() == 2,
          "scene document preserves the canonical object array");
    const auto& shape = document->objects[0];
    Check(shape.metadata.kind == sr::wpscene::SceneObjectKind::Shape,
          "scene document classifies shape objects");
    Check(shape.metadata.has_id && shape.metadata.id == 42,
          "scene document distinguishes authored IDs from default zero");
    Check(! shape.metadata.visible && shape.metadata.visible_user.name == "show_shape",
          "scene document preserves visibility user bindings");
    Check(shape.metadata.solid, "scene document preserves solid metadata");
    Check(shape.authored.get("origin").is_some(),
          "scene document retains the authored object record");
    Check(! document->objects[1].metadata.has_id,
          "scene document records a missing object ID explicitly");

    std::vector<float> dynamic { 9.0f };
    Check(sr::GetJsonValue(Parse("[1, 2.5, 3]"), dynamic) &&
              dynamic == std::vector<float> { 1.0f, 2.5f, 3.0f },
          "dynamic numeric fields decode from JSON arrays");
    std::array<float, 2> wrong_size {};
    Check(! sr::GetJsonValue(Parse("[1, 2, 3]"), wrong_size),
          "fixed-size numeric fields reject mismatched JSON arrays");

    auto invalid_objects = sr::wpscene::ParseSceneDocumentJson(
        R"JSON({"camera": {}, "general": {}, "objects": {}})JSON",
        sr::wpscene::kSceneVersionUnknown);
    Check(invalid_objects.has_value() && ! invalid_objects->objects_are_array,
          "scene document records non-array objects without inventing entries");
}

void TestEffectSelfCompositeStaysLocal() {
    const char* assets_root = std::getenv("SCENERENDERER_ASSETS_DIR");
    if (assets_root == nullptr || assets_root[0] == '\0') return;

    sr::fs::VFS vfs;
    Check(vfs.Mount("/assets", sr::fs::CreatePhysicalFs(assets_root)),
          "self-composite regression mounts the Wallpaper Engine assets");
    if (! vfs.Open("/assets/effects/godrays/effect.json")) return;

    auto document = sr::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 568,
                "name": "Self Composite",
                "image": "models/util/fullscreenlayer.json",
                "copybackground": true,
                "effects": [{
                    "file": "effects/godrays/effect.json",
                    "visible": true,
                    "passes": [{}, {}, {}, {}, {
                        "textures": [null, "_rt_imageLayerComposite_568_a"]
                    }]
                }],
                "visible": true
            }]
        })JSON",
        sr::wpscene::kSceneVersionUnknown);
    Check(document.has_value(), "self-composite regression parses its scene document");
    if (! document) return;

    wavsen::audio::SoundManager sound_manager;
    sr::WPSceneParser            parser;
    auto scene = parser.Parse("self-composite", *document, vfs, sound_manager);
    Check(scene != nullptr, "self-composite regression compiles its scene");
    if (! scene) return;

    const auto layer = sr::WallpaperLayerId { .value = 568 };
    auto       snapshot = sr::ExtractRenderSceneSnapshot(*scene);
    Check(! snapshot.HasLinkConsumer(layer),
          "an effect sampling its own layer composite stays local");
    Check(scene->renderTargets.count(sr::GenLinkTex(568)) == 0,
          "a self-composite does not allocate an external link target");
}

sr::SceneNode* FindWallpaperNode(sr::SceneNode* node, std::int32_t id) {
    if (node == nullptr) return nullptr;
    if (auto wallpaper = node->WallpaperIdentity(); wallpaper && wallpaper->value == id) return node;
    for (auto& child : node->GetChildren()) {
        if (auto* found = FindWallpaperNode(child.as_ptr(), id)) return found;
    }
    return nullptr;
}

sr::SceneNode* FindAnimationOwner(sr::SceneNode* node, std::string_view name) {
    if (node == nullptr) return nullptr;
    if (node->FindAnimation(name)) return node;
    for (auto& child : node->GetChildren()) {
        if (auto* found = FindAnimationOwner(child.as_ptr(), name)) return found;
    }
    return nullptr;
}

bool GraphEmitsLayer(sr::rg::RenderGraph& graph, const sr::RenderSceneSnapshot& snapshot,
                     std::int32_t id) {
    const auto render_items = snapshot.renderItemsFor(sr::WallpaperLayerId { .value = id });
    for (auto node_id : graph.topologicalOrder()) {
        auto* pass = static_cast<sr::vulkan::VulkanPass*>(graph.getPass(node_id));
        if (pass == nullptr) continue;
        auto pass_item = pass->renderItemId();
        if (! pass_item) continue;
        for (auto item : render_items) {
            if (item.index == pass_item->index && item.generation == pass_item->generation)
                return true;
        }
    }
    return false;
}

void TestCompositeLayerElisionAndPhysicalExtent() {
    const char* assets_root = std::getenv("SCENERENDERER_ASSETS_DIR");
    if (assets_root == nullptr || assets_root[0] == '\0') return;

    sr::fs::VFS vfs;
    Check(vfs.Mount("/assets", sr::fs::CreatePhysicalFs(assets_root)),
          "composite-layer regression mounts the shared assets");
    if (! vfs.Open("/assets/models/util/composelayer.json")) return;

    auto unlinked_document = sr::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 490,
                "name": "Hit Region",
                "image": "models/util/composelayer.json",
                "alignment": "left",
                "copybackground": true,
                "origin": [960.0, 540.0, 0.0],
                "scale": [2.0, 3.0, 1.0],
                "size": [100.0, 100.0],
                "solid": true,
                "visible": true
            }]
        })JSON",
        sr::wpscene::kSceneVersionUnknown);
    Check(unlinked_document.has_value(), "unlinked composite fixture parses");
    if (! unlinked_document) return;

    wavsen::audio::SoundManager sound_manager;
    sr::WPSceneParser            parser;
    auto unlinked = parser.Parse("unlinked-composite", *unlinked_document, vfs, sound_manager);
    Check(unlinked != nullptr, "unlinked composite fixture compiles");
    if (! unlinked) return;

    auto* unlinked_node = FindWallpaperNode(unlinked->sceneGraph.as_ptr(), 490);
    Check(unlinked_node != nullptr, "unlinked composite keeps its scene node");
    if (! unlinked_node) return;
    Check(unlinked->NodeImageEffectCount(*unlinked_node) == 0,
          "unlinked identity composite has no synthetic effect chain");
    Check(unlinked->static_elidable_layer_ids.count(490) != 0,
          "unlinked identity composite is statically elidable");
    Check(unlinked_node->Camera().empty(),
          "unlinked identity composite allocates no effect camera");
    auto unlinked_snapshot = sr::ExtractRenderSceneSnapshot(*unlinked);
    auto unlinked_graph    = sr::sceneToRenderGraph(*unlinked, unlinked_snapshot);
    Check(unlinked_graph != nullptr, "unlinked composite render graph builds");
    if (unlinked_graph) {
        Check(! GraphEmitsLayer(*unlinked_graph, unlinked_snapshot, 490),
              "unlinked identity composite emits no render pass");
    }

    auto linked_document = sr::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 490,
                "name": "Linked Region",
                "image": "models/util/composelayer.json",
                "alignment": "left",
                "copybackground": true,
                "origin": [960.0, 540.0, 0.0],
                "scale": [2.0, 3.0, 1.0],
                "size": [100.0, 100.0],
                "solid": true,
                "visible": true
            }, {
                "id": 491,
                "name": "Composite Consumer",
                "image": "models/util/composelayer.json",
                "config": {"passthrough": false},
                "origin": [200.0, 200.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "size": [100.0, 100.0],
                "instance": {
                    "textures": ["_rt_imageLayerComposite_490_a"]
                },
                "visible": true
            }]
        })JSON",
        sr::wpscene::kSceneVersionUnknown);
    Check(linked_document.has_value(), "linked composite fixture parses");
    if (! linked_document) return;

    sr::WPSceneParser linked_parser;
    auto linked = linked_parser.Parse("linked-composite", *linked_document, vfs, sound_manager);
    Check(linked != nullptr, "linked composite fixture compiles");
    if (! linked) return;

    auto* linked_node = FindWallpaperNode(linked->sceneGraph.as_ptr(), 490);
    Check(linked_node != nullptr, "linked composite keeps its scene node");
    if (! linked_node) return;
    Check(linked->NodeImageEffectCount(*linked_node) > 0,
          "linked composite retains a publishable effect chain");
    Check(! linked_node->Camera().empty(), "linked composite owns an effect camera");
    if (linked_node->Camera().empty()) return;

    auto camera = linked->cameras.find(linked_node->Camera());
    Check(camera != linked->cameras.end() && camera->second,
          "linked composite effect camera is registered");
    if (camera == linked->cameras.end() || ! camera->second) return;
    auto attached = camera->second->GetAttachedNode();
    auto global   = linked->activeCamera->GetAttachedNode();
    Check(attached.is_some() && global.is_some() && *attached == *global,
          "a linked composite captures through the shared passthrough camera node");
    Check(linked->cameras.count(linked_node->Camera() + "_group") == 0,
          "a linked composite registers no per-layer group camera");

    Check(Near(static_cast<float>(linked_node->GeometryTransform()(0, 3)), 0.0f),
          "a linked composite keeps its authored source geometry unshifted");
    auto effect_layer = camera->second->GetImgEffect();
    Check(effect_layer != nullptr, "linked composite camera retains its effect layer");
    if (effect_layer) {
        Check(Near(static_cast<float>(effect_layer->FinalMesh().GeometryTransform()(0, 3)), 50.0f),
              "linked composite final geometry carries the authored alignment offset");
    }

    const std::string pingpong =
        std::string(sr::SR_EFFECT_PPONG_PREFIX_A) + linked_node->Camera();
    auto target = linked->renderTargets.find(pingpong);
    Check(target != linked->renderTargets.end(), "linked composite allocates its source target");
    if (target != linked->renderTargets.end()) {
        Check(! target->second.bind.enable && target->second.width == 100 &&
                  target->second.height == 100,
              "a linked composite source target keeps its authored fixed extent");
        Check(! target->second.preserve_on_write,
              "a linked composite source target does not preserve previous contents");
    }

    sr::vulkan::UpdateCameraFillModeForExtent(
        *linked, sr::FillMode::ASPECTCROP, 3840, 2160);
    const auto retina_extent =
        sr::vulkan::ProjectedLayerPhysicalExtent(*linked, *linked_node, 3840, 2160);
    Check(retina_extent == std::array<std::int32_t, 2> { 400, 600 },
          "linked composite target follows Retina output pixel density");
    const auto scaled_extent =
        sr::vulkan::ProjectedLayerPhysicalExtent(*linked, *linked_node, 1920, 1080);
    Check(scaled_extent == std::array<std::int32_t, 2> { 200, 300 },
          "linked composite target follows renderer output scaling");

    auto linked_snapshot = sr::ExtractRenderSceneSnapshot(*linked);
    Check(linked_snapshot.HasLinkConsumer(sr::WallpaperLayerId { .value = 490 }),
          "linked composite remains discoverable by its texture consumer");
    auto linked_graph = sr::sceneToRenderGraph(*linked, linked_snapshot);
    Check(linked_graph != nullptr, "linked composite render graph builds");
    if (linked_graph) {
        Check(GraphEmitsLayer(*linked_graph, linked_snapshot, 490),
              "linked composite emits its required render pass");
    }
}

void TestDynamicCopySnapshotMatchesSourceRequest() {
    sr::Scene scene;
    scene.renderTargets["_rt_snapshot_source"] = {
        .width  = 640,
        .height = 360,
    };
    scene.renderTargets["_rt_snapshot_copy"] = {
        .width  = 1920,
        .height = 1080,
    };

    sr::vulkan::CopyPass pass(sr::vulkan::CopyPass::Desc {
        .src             = "_rt_snapshot_source",
        .dst             = "_rt_snapshot_copy",
        .dst_matches_src = true,
    });
    (void)pass.finalizeResourceRequests(scene);
    const auto diagnostics = pass.textureRequestDiagnostics();
    Check(diagnostics.size() == 2 && diagnostics[0].request && diagnostics[1].request,
          "dynamic snapshot copy resolves both resource requests");
    if (diagnostics.size() != 2 || ! diagnostics[0].request || ! diagnostics[1].request) return;

    const auto& src = *diagnostics[0].request;
    const auto& dst = *diagnostics[1].request;
    Check(src.cache_key && dst.cache_key && sr::vulkan::SameTextureKey(*src.cache_key, *dst.cache_key),
          "dynamic snapshot destination inherits the exact source allocation description");
    Check(dst.name == "_rt_snapshot_copy",
          "dynamic snapshot destination keeps its generated resource identity");
}

void TestFinalResolvePrecedesLinkPublication() {
    sr::Scene scene;
    scene.renderTargets[std::string(sr::SpecTex_Default)] = {
        .width  = 64,
        .height = 64,
        .bind   = { .enable = true, .screen = true },
    };
    const std::string pingpong_a = "_rt_effect_pingpong_a_final_publish_test";
    const std::string pingpong_b = "_rt_effect_pingpong_b_final_publish_test";
    scene.renderTargets[pingpong_a] = { .width = 64, .height = 64, .allowReuse = true };
    scene.renderTargets[pingpong_b] = { .width = 64, .height = 64, .allowReuse = true };
    scene.default_effect_mesh.Submeshes().emplace_back();

    auto source = rstd::sync::Arc<sr::SceneNode>::make();
    source->ID() = 42;
    source->SetCamera("final_publish_effect");
    auto source_mesh = std::make_shared<sr::SceneMesh>();
    source_mesh->Submeshes().emplace_back();
    sr::SceneMaterial source_material;
    source_material.name = "final_publish_source";
    source_mesh->AddMaterial(std::move(source_material));
    source->AddMesh(source_mesh);

    auto final_node = rstd::sync::Arc<sr::SceneNode>::make();
    auto final_mesh = std::make_shared<sr::SceneMesh>();
    final_mesh->Submeshes().emplace_back();
    sr::SceneMaterial final_material;
    final_material.name     = "final_publish_resolve";
    final_material.textures = { pingpong_a };
    final_mesh->AddMaterial(std::move(final_material));
    final_node->AddMesh(final_mesh);
    auto final_effect = std::make_shared<sr::SceneImageEffect>();
    final_effect->nodes.push_back(sr::SceneImageEffectNode {
        .output    = pingpong_b,
        .sceneNode = final_node.clone(),
    });

    auto effect_layer =
        std::make_shared<sr::SceneImageEffectLayer>(source.as_ptr(), 64.0f, 64.0f,
                                                    pingpong_a, pingpong_b);
    effect_layer->SetFullscreen(true);
    effect_layer->SetFinalResolveEffect(final_effect);
    auto effect_camera = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(64.0, 64.0, -1.0, 1.0));
    effect_camera->AttatchImgEffect(effect_layer);
    scene.cameras.emplace("final_publish_effect", effect_camera);
    scene.cameras.emplace(
        "effect",
        std::make_shared<sr::SceneCamera>(
            sr::SceneCamera::MakeOrthographic(64.0, 64.0, -1.0, 1.0)));

    auto consumer = rstd::sync::Arc<sr::SceneNode>::make();
    consumer->ID() = 43;
    auto consumer_mesh = std::make_shared<sr::SceneMesh>();
    consumer_mesh->Submeshes().emplace_back();
    sr::SceneMaterial consumer_material;
    consumer_material.name     = "final_publish_consumer";
    consumer_material.textures = { sr::GenLinkTex(42) };
    consumer_mesh->AddMaterial(std::move(consumer_material));
    consumer->AddMesh(consumer_mesh);

    scene.RegisterNode(*source, sr::WallpaperLayerId { .value = 42 });
    scene.RegisterNode(*consumer, sr::WallpaperLayerId { .value = 43 });
    scene.sceneGraph->AppendChild(source.clone());
    scene.sceneGraph->AppendChild(consumer.clone());

    auto graph = sr::sceneToRenderGraph(scene);
    Check(graph != nullptr, "final-resolve publication regression builds a render graph");
    if (! graph) return;

    bool        saw_final_resolve = false;
    bool        consumer_reads_published_final = false;
    std::size_t final_order = std::numeric_limits<std::size_t>::max();
    std::size_t consumer_order = std::numeric_limits<std::size_t>::max();
    auto        order = graph->topologicalOrder();
    for (std::size_t i = 0; i < order.size(); ++i) {
        auto state = graph->passState(order[i]);
        if (! state) continue;
        if (state->name == "final_publish_resolve") {
            saw_final_resolve = true;
            final_order       = i;
        }
        if (state->name != "final_publish_consumer") continue;
        consumer_order = i;
        auto* pass = static_cast<sr::vulkan::VulkanPass*>(graph->getPass(order[i]));
        if (pass == nullptr) continue;
        for (const auto& diagnostic : pass->textureRequestDiagnostics()) {
            if (diagnostic.role == "sampled" && diagnostic.slot == 0 &&
                diagnostic.name == sr::GenLinkTex(42)) {
                consumer_reads_published_final = true;
            }
        }
    }
    Check(saw_final_resolve, "explicit final-resolve is emitted into the render graph");
    Check(final_order < consumer_order,
          "explicit final-resolve executes before its external link consumer");
    Check(consumer_reads_published_final,
          "link consumer samples the version published after explicit final-resolve");
}

void TestAuthoredLayerOrdering() {
    sr::Scene scene;
    scene.RegisterAuthoredLayer(sr::WallpaperLayerId { .value = 10 }, 0);
    scene.RegisterAuthoredLayer(sr::WallpaperLayerId { .value = 11 }, 0);
    scene.RegisterAuthoredLayer(sr::WallpaperLayerId { .value = 12 }, 0);

    auto first = rstd::sync::Arc<sr::SceneNode>::make();
    auto third = rstd::sync::Arc<sr::SceneNode>::make();
    scene.RegisterNode(*first, sr::WallpaperLayerId { .value = 10 });
    scene.RegisterNode(*third, sr::WallpaperLayerId { .value = 12 });
    scene.sceneGraph->AppendChild(first.clone());
    scene.sceneGraph->AppendChild(third.clone());

    Check(scene.LayerIndex(*first) == std::optional<std::size_t>(0) &&
              scene.LayerIndex(*third) == std::optional<std::size_t>(2),
          "script layer indexes preserve omitted authored siblings");
}

void TestUserPropertyIndexesOwnTheirTargets() {
    sr::Scene scene;
    std::weak_ptr<sr::SceneMaterial> material_weak;
    {
        auto node     = rstd::sync::Arc<sr::SceneNode>::make();
        auto material = std::make_shared<sr::SceneMaterial>();
        material_weak = material;
        scene.image_color_user_index["tint"].push_back(
            { node.clone(), { material } });
        scene.image_alpha_user_index["fade"].push_back(
            { node.clone(), { material } });
        scene.shader_user_var_index["strength"].push_back({ material, "g_Strength" });
        scene.material_texture_user_index["image"].push_back(
            { .material = material, .slot = 0, .fallback = "util/white" });
    }

    Check(! material_weak.expired(),
          "scene user-property indexes retain materials after parse-owned references are released");
    const auto& color_binding = scene.image_color_user_index.at("tint").front();
    Check(color_binding.node && color_binding.materials.front(),
          "image user-property indexes retain both node and material targets");
    color_binding.node->SetColor({ 0.2f, 0.4f, 0.6f });
    Check(Near(color_binding.node->Color().x(), 0.2f),
          "retained image user-property node remains writable");
}

void TestMdlv23MultiCurveMorphEvents() {
    const char* workshop_root = std::getenv("SCENERENDERER_WORKSHOP_DIR");
    if (workshop_root == nullptr || workshop_root[0] == '\0') return;

    const auto pkg_path = std::filesystem::path(workshop_root) / "3686252018" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) return;

    sr::fs::VFS vfs;
    auto pkg = sr::fs::WPPkgFs::CreatePkgFs(pkg_path.string());
    Check(pkg != nullptr && vfs.Mount("/assets", std::move(pkg)),
          "multi-curve MDL sample mounts as a scene package");
    if (! vfs.Open("/assets/models/sheet_puppet.mdl")) return;

    sr::WPMdl mdl;
    Check(sr::WPMdlParser::Parse("models/sheet_puppet.mdl", vfs, mdl),
          "MDLV23 multi-curve puppet parses");
    if (! mdl.puppet || mdl.puppet->anims.empty()) return;

    Check(mdl.mdla == 6, "multi-curve puppet uses MDLA version 6");
    Check(mdl.puppet->anims.size() == 17, "multi-curve puppet preserves all animations");
    const auto& left_eye = mdl.puppet->anims.front();
    Check(left_eye.name == "Left eye" && left_eye.v4_events.size() == 1,
          "multi-curve puppet preserves the Left eye morph event");
    if (left_eye.v4_events.empty()) return;

    const auto& event = left_eye.v4_events.front();
    Check(event.flags == 0 && event.curves.size() == 6,
          "MDLV23 morph event preserves all six curves");
    for (std::size_t i = 0; i < event.curves.size(); ++i) {
        const auto& curve = event.curves[i];
        Check(curve.id == i && curve.values.size() == 211 &&
                  ! curve.values.empty() && Near(curve.values.front(), 1.0f),
              "MDLV23 morph curve preserves id, sample count, and first value");
    }
    Check(mdl.morph_sections.size() == 1 &&
              Near(mdl.morph_sections.front().event_time, event.time) &&
              mdl.morph_sections.front().sections.size() == event.curves.size(),
          "MDMP morph sections align with the MDLA event curves");
}

void TestWallpaper2887099508Interactions() {
    const char* assets_root   = std::getenv("SCENERENDERER_ASSETS_DIR");
    const char* workshop_root = std::getenv("SCENERENDERER_WORKSHOP_DIR");
    if (assets_root == nullptr || workshop_root == nullptr || assets_root[0] == '\0' ||
        workshop_root[0] == '\0')
        return;

    const auto pkg_path = std::filesystem::path(workshop_root) / "2887099508" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) return;

    sr::fs::VFS vfs;
    Check(vfs.Mount("/assets", sr::fs::CreatePhysicalFs(assets_root)),
          "2887099508 mounts the shared assets");
    auto pkg = sr::fs::WPPkgFs::CreatePkgFs(pkg_path.string());
    Check(pkg != nullptr, "2887099508 package opens");
    if (! pkg) return;
    const auto pkg_version = sr::wpscene::ParsePkgVersionStamp(pkg->pkg_version_stamp());
    Check(vfs.Mount("/assets", std::move(pkg)), "2887099508 package mounts");

    sr::WPMdl ear_model;
    Check(sr::WPMdlParser::Parse("models/r ear1_puppet.mdl", vfs, ear_model),
          "2887099508 ear puppet parses");
    if (ear_model.puppet) {
        auto find_animation = [&](std::int32_t id) {
            return std::find_if(ear_model.puppet->anims.begin(),
                                ear_model.puppet->anims.end(),
                                [id](const auto& animation) { return animation.id == id; });
        };
        const auto first  = find_animation(445);
        const auto second = find_animation(572);
        Check(first != ear_model.puppet->anims.end() &&
                  first->mode == sr::WPPuppet::PlayMode::Single &&
                  second != ear_model.puppet->anims.end() &&
                  second->mode == sr::WPPuppet::PlayMode::Single,
              "2887099508 ear click guards use completing single animations");
        sr::WPPuppetLayer ear_layer(ear_model.puppet);
        std::array<sr::WPPuppetLayer::AnimationLayer, 4> layers {
            sr::WPPuppetLayer::AnimationLayer {
                .id = 451, .visible = false, .name = "左下1" },
            sr::WPPuppetLayer::AnimationLayer {
                .id = 508, .visible = false, .name = "左上1" },
            sr::WPPuppetLayer::AnimationLayer {
                .id = 445, .visible = true, .name = "左弹跳" },
            sr::WPPuppetLayer::AnimationLayer {
                .id = 572, .visible = true, .name = "左弹跳2" },
        };
        ear_layer.prepared(layers);
        auto first_handle  = ear_layer.animationLayer("左弹跳");
        auto second_handle = ear_layer.animationLayer("左弹跳2");
        (void)ear_layer.genFrame(0.0);
        (void)ear_layer.genFrame(10.0);
        const auto first_state = first_handle ? ear_layer.animationState(*first_handle) : std::nullopt;
        const auto second_state =
            second_handle ? ear_layer.animationState(*second_handle) : std::nullopt;
        Check(first_state && ! first_state->playing && second_state && ! second_state->playing,
              "2887099508 ear click guards clear after authored single animations complete");
    }

    auto document = sr::wpscene::LoadSceneDocumentFromVfs(vfs, "/assets/scene.json", pkg_version);
    Check(document.has_value(), "2887099508 scene document loads");
    if (! document) return;
    wavsen::audio::SoundManager sound_manager;
    sr::WPSceneParser           parser;
    auto scene = parser.Parse("2887099508", *document, vfs, sound_manager);
    Check(scene != nullptr, "2887099508 scene compiles");
    if (! scene) return;
    Check(scene->RuntimeLayerVisibilityEnabled(sr::WallpaperLayerId { .value = 257 }) &&
              scene->RuntimeLayerVisibilityEnabled(sr::WallpaperLayerId { .value = 384 }) &&
              ! scene->RuntimeLayerVisibilityEnabled(sr::WallpaperLayerId { .value = 791 }),
          "2887099508 keeps only scripted book visibility in the static graph");

    auto* updater = static_cast<sr::SceneUniformUpdater*>(scene->shaderValueUpdater.get());
    auto verify_puppet_layer = [&](std::int32_t id) {
        auto* node = FindWallpaperNode(scene->sceneGraph.as_ptr(), id);
        if (node == nullptr || updater == nullptr || node->Camera().empty()) return false;
        auto logical = updater->PuppetLayerForNode(node);
        auto camera  = scene->cameras.find(node->Camera());
        if (! logical || camera == scene->cameras.end() || ! camera->second ||
            ! camera->second->HasImgEffect())
            return false;
        auto effects = camera->second->GetImgEffect();
        bool found = false;
        for (std::size_t index = 0; index < effects->EffectCount(); ++index) {
            auto effect = effects->GetEffect(index);
            if (! effect) continue;
            for (const auto& effect_node : effect->nodes) {
                auto rendered = updater->PuppetLayerForNode(effect_node.sceneNode.as_ptr());
                if (! rendered) continue;
                found = true;
                if (rendered != logical) return false;
            }
        }
        return found;
    };
    Check(verify_puppet_layer(162) && verify_puppet_layer(309),
          "2887099508 scripts and rendered ear and leg passes share puppet playback state");

    auto* page      = FindWallpaperNode(scene->sceneGraph.as_ptr(), 257);
    auto* first_page = FindWallpaperNode(scene->sceneGraph.as_ptr(), 384);
    Check(page != nullptr && first_page != nullptr, "2887099508 book nodes exist");
    if (page == nullptr || first_page == nullptr) return;
    auto* blush_in_owner = FindAnimationOwner(scene->sceneGraph.as_ptr(), "blush1");
    auto* blush_out_owner = FindAnimationOwner(scene->sceneGraph.as_ptr(), "blush22");
    auto blush_effect = blush_in_owner != nullptr
                            ? scene->FindNodeImageEffect(*blush_in_owner, "blush")
                            : std::nullopt;
    auto blush_effect_out = blush_out_owner != nullptr
                                ? scene->FindNodeImageEffect(*blush_out_owner, "blush2")
                                : std::nullopt;
    auto blush_in =
        blush_in_owner != nullptr ? blush_in_owner->FindAnimation("blush1") : nullptr;
    auto blush_out =
        blush_out_owner != nullptr ? blush_out_owner->FindAnimation("blush22") : nullptr;
    Check(blush_effect.has_value() && blush_effect_out.has_value(),
          "2887099508 hidden blush effects remain available to scripts");
    Check(blush_in != nullptr && blush_out != nullptr,
          "2887099508 blush animations remain available to scripts");
    if (blush_effect && blush_effect_out && blush_in && blush_out) {
        auto* blush_material = blush_effect->effect->nodes.front().sceneNode->Mesh()->Material();
        auto* blush_out_material =
            blush_effect_out->effect->nodes.front().sceneNode->Mesh()->Material();
        Check(blush_material != nullptr && blush_material->textures.size() > 2 &&
                  ! blush_material->textures[2].empty() &&
                  blush_out_material != nullptr && blush_out_material->textures.size() > 2 &&
                  ! blush_out_material->textures[2].empty(),
              "2887099508 blush effects retain both authored opacity masks");
        Check(blush_material != nullptr && blush_material->customShader.variant.has_value() &&
                  blush_material->customShader.variant->resolved_combos.contains("MASK") &&
                  blush_material->customShader.variant->resolved_combos.at("MASK") == "1" &&
                  blush_out_material != nullptr &&
                  blush_out_material->customShader.variant.has_value() &&
                  blush_out_material->customShader.variant->resolved_combos.contains("MASK") &&
                  blush_out_material->customShader.variant->resolved_combos.at("MASK") == "1",
              "2887099508 blush effects compile masked pulse shader variants");
        sr::script::JsRuntime blush_runtime;
        auto* blush_script = blush_runtime.MakeFieldScript(
            R"JS(
                export function update() {
                    const layer = thisScene.getLayer('back leg body');
                    const effectIn = layer.getEffect('blush');
                    const effectOut = layer.getEffect('blush2');
                    const blushIn = thisScene.getAnimation('blush1');
                    const blushOut = thisScene.getAnimation('blush22');
                    effectIn.visible = true;
                    effectOut.visible = true;
                    blushIn.setFrame(4);
                    blushOut.setFrame(6);
                    blushIn.play();
                    blushOut.play();
                    return new Vec4(
                        effectIn.visible ? 1 : 0,
                        effectOut.visible ? 1 : 0,
                        blushIn.name === 'blush1' ? 1 : 0,
                        blushOut.name === 'blush22' ? 1 : 0);
                }
            )JS",
            "test/2887099508_blush_scene_animation_lookup",
            sr::script::FieldKind::Vec4,
            Parse("{}"),
            Parse("\"0 0 0 0\""),
            blush_in_owner);
        Check(blush_script != nullptr, "2887099508 blush scene lookup script compiles");
        if (blush_script) {
            blush_runtime.SetSceneRoot(scene->sceneGraph.as_ptr());
            blush_runtime.TickAll();
            const auto* value =
                std::get_if<sr::script::Vec4Value>(&blush_script->last_value());
            Check(value && value->x == 1.0 && value->y == 1.0 && value->z == 1.0 &&
                      value->w == 1.0 && blush_in->IsPlaying() && blush_out->IsPlaying() &&
                      std::abs(blush_in->Frame() - 4.0) < 0.001 &&
                      std::abs(blush_out->Frame() - 6.0) < 0.001,
                  "2887099508 scripts reveal and control both blush effects");
        }
        scene->SetImageEffectRuntimeVisible(*blush_effect, false);
        scene->SetImageEffectRuntimeVisible(*blush_effect_out, false);
        blush_in->Stop();
        blush_out->Stop();
    }
    auto* tim_logo = FindWallpaperNode(scene->sceneGraph.as_ptr(), 500);
    Check(tim_logo != nullptr && scene->LayerIndex(*tim_logo) == std::optional<std::size_t>(78),
          "2887099508 preserves the authored TIM logo layer index");
    Check(FindWallpaperNode(scene->sceneGraph.as_ptr(), 495) == nullptr,
          "2887099508 does not load the omitted hidden particle renderer");
    Check(! page->Visible() && ! first_page->Visible(),
          "2887099508 book pages start hidden");
    Check(page->Solid() && first_page->Solid() && tim_logo != nullptr && tim_logo->Solid(),
          "2887099508 interactive layers retain authored solid state");
    auto page_animation       = page->FindAnimation("111");
    auto first_page_animation = first_page->FindAnimation("900");
    Check(page_animation != nullptr && first_page_animation != nullptr,
          "2887099508 book material animations are registered on their script nodes");
    if (! page_animation || ! first_page_animation) return;

    sr::script::JsRuntime page_runtime;
    auto* page_script = page_runtime.MakeFieldScript(
        R"JS(
            let started = false;
            export function update() {
                if (!started && thisLayer.visible) {
                    const animation = thisScene.getAnimation('111');
                    animation.play();
                    started = true;
                    return animation.name === '111' ? 1 : 0;
                }
                return 0;
            }
        )JS",
        "test/2887099508_page_scene_animation_lookup",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"),
        page);
    Check(page_script != nullptr, "2887099508 page scene lookup script compiles");
    if (! page_script) return;
    page_runtime.SetSceneRoot(scene->sceneGraph.as_ptr());

    auto snapshot = sr::ExtractRenderSceneSnapshot(*scene);
    auto graph    = sr::sceneToRenderGraph(*scene, snapshot);
    Check(graph != nullptr && GraphEmitsLayer(*graph, snapshot, 257) &&
              GraphEmitsLayer(*graph, snapshot, 384),
          "2887099508 hidden book pages remain in the scripted static render graph");

    sr::script::FrameInputs inputs;
    scene->elapsingTime = 0.0;
    scene->TickNodeFieldAnimations();
    sr::script::TickSceneScripts(*scene, inputs);

    page->SetVisible(true);
    sr::script::TickSceneScripts(*scene, inputs);
    Check(page->Visible(), "2887099508 page remains visible before its marker");
    page_runtime.TickAll();
    const auto* page_lookup =
        std::get_if<sr::script::ScalarValue>(&page_script->last_value());
    Check(page_lookup && page_lookup->v == 1.0 && page_animation->IsPlaying(),
          "2887099508 thisScene starts the descendant page animation");
    scene->elapsingTime = 1.1;
    scene->TickNodeFieldAnimations();
    sr::script::TickSceneScripts(*scene, inputs);
    Check(! page->Visible(), "2887099508 page marker hides the completed page");

    first_page->SetVisible(true);
    sr::script::TickSceneScripts(*scene, inputs);
    Check(first_page->Visible(), "2887099508 first page remains visible before its marker");
    first_page_animation->Play();
    scene->elapsingTime = 2.2;
    scene->TickNodeFieldAnimations();
    sr::script::TickSceneScripts(*scene, inputs);
    Check(! first_page->Visible(), "2887099508 first-page marker hides the completed page");
}

void TestShaderHlslSemanticCompatibility() {
    sr::SceneShaderVariantDesc desc;
    desc.scene_id    = "hlsl-semantic-compatibility-test";
    desc.shader_name = "hlsl-semantic-compatibility-test";
    desc.stages.push_back(sr::SceneShaderVariantStage {
        .stage      = sr::ShaderType::VERTEX,
        .source_key = "/assets/shaders/hlsl-semantic-compatibility-test.vert",
        .source     = R"(
attribute vec3 a_Position;
void main() {
    float value = 2.0;
    float scaled = mul(1, value / 50.0);
    gl_Position = vec4(a_Position + vec3(scaled * 0.0), 1.0);
}
)",
    });
    desc.stages.push_back(sr::SceneShaderVariantStage {
        .stage      = sr::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/hlsl-semantic-compatibility-test.frag",
        .source     = R"(
float tint;
void main() {
    tint = 0.25;
    gl_FragColor = vec4(tint, tint, tint, 1.0);
}
)",
    });

    sr::fs::VFS vfs;
    const auto result = sr::WPShaderParser::CompileSceneShaderVariant(desc, vfs);
    Check(result.ok && result.shader && result.shader->codes.size() == 2,
          "leading integer scalar mul compiles through the HLSL path");
    if (! result.ok || ! result.shader) return;

    std::vector<sr::vulkan::Uni_ShaderSpv> reflected_spvs;
    sr::vulkan::ShaderReflected            reflection;
    Check(sr::vulkan::GenReflect(result.shader->codes, reflected_spvs, reflection),
          "private shader globals reflect successfully");
    Check(std::none_of(reflection.blocks.begin(), reflection.blocks.end(), [](const auto& block) {
              return block.name == "$Global";
          }),
          "file-scope GLSL variables do not become an HLSL $Global uniform block");
}

void TestMissingTexturePlaceholderSemantics() {
    auto image = sr::vulkan::MakeMissingTexturePlaceholder("missing/test");
    Check(image && image->key == "missing/test" && image->header.width == 2 &&
              image->header.height == 2 && image->header.format == sr::TextureFormat::RGBA8 &&
              image->slots.size() == 1 && image->slots.front().mipmaps.size() == 1,
          "missing texture placeholder has a complete 2x2 RGBA8 image layout");
    if (image && ! image->slots.empty() && ! image->slots.front().mipmaps.empty()) {
        const auto& mip = image->slots.front().mipmaps.front();
        bool opaque_magenta = mip.size == 16 && mip.data != nullptr;
        const auto* pixels = mip.data.get();
        for (std::size_t i = 0; opaque_magenta && i < 4; ++i) {
            opaque_magenta = pixels[i * 4 + 0] == 0xFF && pixels[i * 4 + 1] == 0x00 &&
                             pixels[i * 4 + 2] == 0xFF && pixels[i * 4 + 3] == 0xFF;
        }
        Check(opaque_magenta, "missing texture placeholder pixels are opaque magenta");
    }

    sr::RenderSceneSnapshot snapshot;
    ProbeImageParser        parser;
    sr::vulkan::SnapshotImportedTextureProvider provider(snapshot, &parser);
    sr::vulkan::TextureRequest request {
        .kind = sr::vulkan::TextureRequestKind::Imported,
        .name = "missing/test",
    };
    Check(provider.ParseImportedTexture(request) != nullptr,
          "absent scene texture is replaced with a placeholder");
    parser.contains = true;
    Check(provider.ParseImportedTexture(request) == nullptr,
          "an existing but undecodable texture remains a hard decode failure");
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

void TestPlaybackSpeedAndAtomicCachePublication() {
    Check(sr::IsValidScenePlaybackSpeed(1.0f) && sr::IsValidScenePlaybackSpeed(4.0f),
          "positive finite scene playback speeds are accepted");
    Check(! sr::IsValidScenePlaybackSpeed(0.0f) &&
              ! sr::IsValidScenePlaybackSpeed(-1.0f) &&
              ! sr::IsValidScenePlaybackSpeed(std::numeric_limits<float>::infinity()) &&
              ! sr::IsValidScenePlaybackSpeed(std::numeric_limits<float>::quiet_NaN()),
          "non-positive and non-finite scene playback speeds are rejected");

    int marker = 0;
    const auto root = std::filesystem::temp_directory_path() /
                      ("scenerenderer-cache-publication-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(&marker)));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    Check(! ec, "atomic cache regression creates its isolated temporary directory");
    if (ec) return;

    sr::fs::VFS vfs;
    Check(vfs.Mount("/cache", sr::fs::CreatePhysicalFs(root.string()), "cache"),
          "atomic cache regression mounts a writable cache directory");
    const auto publish = [&vfs](std::string_view value, bool commit) {
        return vfs.Publish("/cache/shaders/test.spv",
                           [value, commit](sr::fs::IBinaryStreamW& stream) {
                               return stream.WriteAll(value.data(), value.size()) && commit;
                           });
    };
    Check(publish("stable", true), "initial cache artifact publishes successfully");
    Check(! publish("partial", false), "failed cache publication is reported");
    auto after_failure = vfs.Open("/cache/shaders/test.spv");
    Check(after_failure && after_failure->ReadAllStr() == "stable",
          "failed cache publication leaves the previous artifact intact");
    Check(publish("replacement", true), "replacement cache artifact publishes successfully");
    auto after_success = vfs.Open("/cache/shaders/test.spv");
    Check(after_success && after_success->ReadAllStr() == "replacement",
          "successful cache publication atomically replaces the previous artifact");
    after_failure.reset();
    after_success.reset();
    std::filesystem::remove_all(root, ec);
}

void TestSwizzledVaryingDeclCompatibility() {
    sr::SceneShaderVariantDesc desc;
    desc.scene_id    = "swizzled-varying-test";
    desc.shader_name = "swizzled-varying-test";
    desc.stages.push_back(sr::SceneShaderVariantStage {
        .stage      = sr::ShaderType::VERTEX,
        .source_key = "/assets/shaders/swizzled-varying-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying vec4 v_Size.xy;
void main() {
    v_Size = vec4(2.0, 3.0, 0.0, 0.0);
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(sr::SceneShaderVariantStage {
        .stage      = sr::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/swizzled-varying-test.frag",
        .source     = R"(
varying vec4 v_Size.xy;
void main() {
    gl_FragColor = vec4(v_Size.xy, 0.0, 1.0);
}
)",
    });

    sr::fs::VFS vfs;
    const auto  result = sr::WPShaderParser::CompileSceneShaderVariant(desc, vfs);
    Check(result.ok && result.shader && result.shader->codes.size() == 2,
          "swizzled varying declarators compile in both stages");
    if (! result.ok || ! result.shader) return;

    std::vector<sr::vulkan::Uni_ShaderSpv> reflected_spvs;
    sr::vulkan::ShaderReflected            reflection;
    Check(sr::vulkan::GenReflect(result.shader->codes, reflected_spvs, reflection),
          "a swizzle-declared varying still reflects across stages");
}

void TestScriptedInvisibleCompositeElision() {
    const char* assets_root = std::getenv("SCENERENDERER_ASSETS_DIR");
    if (assets_root == nullptr || assets_root[0] == '\0') return;

    sr::fs::VFS vfs;
    Check(vfs.Mount("/assets", sr::fs::CreatePhysicalFs(assets_root)),
          "scripted visibility regression mounts the shared assets");
    if (! vfs.Open("/assets/models/util/composelayer.json")) return;
    const auto tint_root = std::filesystem::path(assets_root) / "effects/tint";
    if (std::filesystem::exists(tint_root / "materials/effects"))
        Check(vfs.Mount("/assets/materials/effects",
                        sr::fs::CreatePhysicalFs((tint_root / "materials/effects").string())),
              "scripted visibility regression mounts the tint materials");
    if (std::filesystem::exists(tint_root / "shaders/effects"))
        Check(vfs.Mount("/assets/shaders/effects",
                        sr::fs::CreatePhysicalFs((tint_root / "shaders/effects").string())),
              "scripted visibility regression mounts the tint shaders");
    auto document = sr::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 930,
                "name": "Hover Hit Area",
                "image": "models/util/composelayer.json",
                "config": {"passthrough": false},
                "origin": [960.0, 540.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "size": [1008.0, 245.0],
                "solid": true,
                "visible": {
                    "value": false,
                    "script": "export let __workshopId = '3674038504';\nexport function cursorEnter() {}\nexport function cursorLeave() {}\n"
                }
            }, {
                "id": 931,
                "name": "Scripted Toggle",
                "image": "models/util/composelayer.json",
                "config": {"passthrough": false},
                "origin": [400.0, 300.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "size": [200.0, 200.0],
                "visible": {
                    "value": false,
                    "script": "let on = false;\nexport function cursorClick() { on = ! on; }\nexport function update() { return on; }\n"
                }
            }, {
                "id": 932,
                "name": "Hidden Link Source",
                "image": "models/util/composelayer.json",
                "copybackground": true,
                "origin": [700.0, 400.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "size": [100.0, 100.0],
                "visible": false
            }, {
                "id": 933,
                "name": "Composite Consumer",
                "image": "models/util/composelayer.json",
                "config": {"passthrough": false},
                "origin": [200.0, 200.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "size": [100.0, 100.0],
                "instance": {"textures": ["_rt_imageLayerComposite_932_a"]},
                "visible": true
            }, {
                "id": 934,
                "name": "Hidden Effect Controller",
                "image": "models/util/solidlayer.json",
                "origin": [100.0, 100.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "size": [10.0, 10.0],
                "visible": false,
                "effects": [{
                    "file": "effects/tint/effect.json",
                    "name": "Controller",
                    "visible": {
                        "value": false,
                        "script": "export function update() { shared.enabled = true; return false; }"
                    }
                }]
            }, {
                "id": 935,
                "name": "Scripted Effect Consumer",
                "image": "models/util/solidlayer.json",
                "origin": [120.0, 120.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "size": [10.0, 10.0],
                "visible": true,
                "effects": [{
                    "file": "effects/tint/effect.json",
                    "name": "Consumer",
                    "visible": {
                        "value": false,
                        "script": "export function update() { return shared.enabled === true; }"
                    }
                }]
            }]
        })JSON",
        sr::wpscene::kSceneVersionUnknown);
    Check(document.has_value(), "scripted visibility fixture parses");
    if (! document) return;

    wavsen::audio::SoundManager sound_manager;
    sr::WPSceneParser           parser;
    auto scene = parser.Parse("scripted-visibility", *document, vfs, sound_manager);
    Check(scene != nullptr, "scripted visibility fixture compiles");
    if (! scene) return;

    Check(scene->visibility_elidable_layer_ids.count(930) != 0 &&
              ! scene->RuntimeLayerVisibilityEnabled(sr::WallpaperLayerId { .value = 930 }),
          "a visible binding without update leaves the hidden layer elided");
    Check(scene->visibility_elidable_layer_ids.count(931) == 0 &&
              scene->RuntimeLayerVisibilityEnabled(sr::WallpaperLayerId { .value = 931 }),
          "a visible binding with update keeps its hidden layer in the graph");
    auto* effect_consumer = FindWallpaperNode(scene->sceneGraph.as_ptr(), 935);
    auto* effect_controller = FindWallpaperNode(scene->sceneGraph.as_ptr(), 934);
    auto controller_effect = effect_controller != nullptr
                                  ? scene->FindNodeImageEffect(*effect_controller, "Controller")
                                  : std::nullopt;
    auto consumer_effect = effect_consumer != nullptr
                                ? scene->FindNodeImageEffect(*effect_consumer, "Consumer")
                                : std::nullopt;
    Check(controller_effect.has_value(),
          "the hidden controller effect compiles");
    Check(consumer_effect.has_value(),
          "a hidden effect with a visibility script remains compiled");
    sr::script::TickSceneScripts(*scene, {});
    Check(consumer_effect && scene->ImageEffectRuntimeVisible(*consumer_effect),
          "an invisible controller effect can drive another effect through shared state");

    auto snapshot = sr::ExtractRenderSceneSnapshot(*scene);
    auto graph    = sr::sceneToRenderGraph(*scene, snapshot);
    Check(graph != nullptr, "scripted visibility render graph builds");
    if (! graph) return;

    Check(! GraphEmitsLayer(*graph, snapshot, 930),
          "the hidden hover hit area emits no render pass");
    Check(GraphEmitsLayer(*graph, snapshot, 931),
          "the scripted toggle keeps a render pass while hidden");
    Check(snapshot.HasLinkConsumer(sr::WallpaperLayerId { .value = 932 }) &&
              GraphEmitsLayer(*graph, snapshot, 932),
          "a hidden link source still publishes its composite target");

    std::size_t gated = 0, ungated = 0, mismatched = 0;
    for (auto node_id : graph->topologicalOrder()) {
        auto state = graph->passState(node_id);
        if (! state || state->type != sr::rg::PassNode::Type::CustomShader) continue;
        auto* pass = static_cast<sr::vulkan::CustomShaderPass*>(graph->getPass(node_id));
        if (pass == nullptr) continue;
        const auto& pdesc    = pass->desc();
        const bool  expected = pdesc.alpha_mode == sr::SceneRenderAlphaMode::Composite &&
                              pdesc.output == sr::SpecTex_Default;
        if (pdesc.hide_when_node_invisible != expected) ++mismatched;
        if (expected)
            ++gated;
        else
            ++ungated;
    }
    Check(mismatched == 0,
          "only main-composite passes gate their draw on runtime node visibility");
    Check(gated > 0 && ungated > 0,
          "the fixture covers both the gated composite and the ungated capture passes");
}

} // namespace

int main() {
    TestExplicitCameraFactories();
    TestPerspectiveFillModePreservesFov();
    TestOrthographicFillModeDerivesPerspectiveFov();
    TestAuthoredSceneZoom();
    TestAnimatedSceneZoom();
    TestAnimatedSceneZoomWithCameraPath();
    TestInvalidAnimatedSceneZoom();
    TestPointerUniformsIgnoreParallaxDelay();
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
    TestJsonArraysAndSceneDocumentMetadata();
    TestEffectSelfCompositeStaysLocal();
    TestCompositeLayerElisionAndPhysicalExtent();
    TestDynamicCopySnapshotMatchesSourceRequest();
    TestFinalResolvePrecedesLinkPublication();
    TestAuthoredLayerOrdering();
    TestUserPropertyIndexesOwnTheirTargets();
    TestMdlv23MultiCurveMorphEvents();
    TestWallpaper2887099508Interactions();
    TestShaderHlslSemanticCompatibility();
    TestSwizzledVaryingDeclCompatibility();
    TestScriptedInvisibleCompositeElision();
    TestMissingTexturePlaceholderSemantics();
    TestParticleRuntimeState();
    TestPlaybackSpeedAndAtomicCachePublication();
    if (g_failures == 0) std::cout << "RuntimeCompatibilityRegression: ok\n";
    return g_failures == 0 ? 0 : 1;
}

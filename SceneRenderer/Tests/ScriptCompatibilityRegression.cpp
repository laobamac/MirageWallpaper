#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

import sr.json;
import sr.pkg.parse;
import sr.script;
import sr.types;
import sr.scene;
import eigen;
import rstd;

namespace
{

int g_failures = 0;

void Check(bool ok, std::string_view what) {
    if (ok) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << '\n';
}

sr::Json Parse(std::string_view source) {
    auto value = sr::ParseJson(source);
    if (value.is_ok()) return value.unwrap();
    ++g_failures;
    std::cerr << "FAIL: invalid test JSON\n";
    return sr::Json::Null();
}

void TestVectorAngle2() {
    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            import * as V from 'WEVector';
            export function update() {
                return new Vec3(
                    V.vectorAngle2(new Vec2(1, 0)),
                    V.vectorAngle2(new Vec2(0, 1)),
                    V.vectorAngle2(V.angleVector2(-135)));
            }
        )JS",
        "test/wevector_vector_angle2",
        sr::script::FieldKind::Vec3,
        Parse("{}"),
        Parse("\"0 0 0\""));
    Check(script != nullptr, "WEVector vectorAngle2 script compiles");
    if (! script) return;
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::Vec3Value>(&script->last_value());
    Check(value != nullptr, "WEVector vectorAngle2 returns Vec3");
    if (! value) return;
    Check(std::abs(value->x) < 0.001 && std::abs(value->y - 90.0) < 0.001 &&
              std::abs(value->z + 135.0) < 0.001,
          "WEVector vectorAngle2 uses degrees and standard axes");
}

void TestMediaCompatibilityFields() {
    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            let properties = 0, thumbnail = 0;
            export function mediaPropertiesChanged(event) {
                if (event.album === 'Album' && event.albumTitle === 'Album') properties = 1;
            }
            export function mediaThumbnailChanged(event) {
                if (event.tertiaryColor.x === 0 && event.textColor.x === 0 &&
                    event.highContrastColor.x === 0) thumbnail = 1;
            }
            export function update() { return properties && thumbnail ? 1 : 0; }
        )JS",
        "test/media_compatibility_fields",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"));
    Check(script != nullptr, "media compatibility script compiles");
    if (! script) return;

    runtime.SetMediaStatus(sr::script::MediaStatus { .title = "Song",
                                                      .artist = "Artist",
                                                      .album = "Album",
                                                      .album_artist = "Album Artist",
                                                      .art_url = "/tmp/cover.png" });
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(value && value->v == 1.0, "media event exposes albumTitle and complete color set");
}

void TestColorScaleHelpers() {
    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            import { normalizeColor, expandColor } from 'WEColor';
            export function update() {
                const normalized = normalizeColor(new Vec3(255, 128, 0));
                const expanded = expandColor(normalized);
                return new Vec3(normalized.x, normalized.y, expanded.y);
            }
        )JS",
        "test/wecolor_scale_helpers",
        sr::script::FieldKind::Vec3,
        Parse("{}"),
        Parse("\"0 0 0\""));
    Check(script != nullptr, "WEColor scale helper script compiles and evaluates");
    if (! script) return;
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::Vec3Value>(&script->last_value());
    Check(value != nullptr, "WEColor scale helpers return Vec3");
    if (! value) return;
    Check(std::abs(value->x - 1.0) < 0.001 &&
              std::abs(value->y - 128.0 / 255.0) < 0.001 &&
              std::abs(value->z - 128.0) < 0.001,
          "WEColor normalizeColor and expandColor use 0-255 scaling");
}

void TestMathConversionConstants() {
    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            import { deg2rad, rad2deg } from 'WEMath';
            export function update() {
                return new Vec3(deg2rad(180), 90 * deg2rad, Math.PI * rad2deg);
            }
        )JS",
        "test/wemath_conversion_constants",
        sr::script::FieldKind::Vec3,
        Parse("{}"),
        Parse("\"0 0 0\""));
    Check(script != nullptr, "WEMath conversion functions compile and evaluate");
    if (! script) return;
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::Vec3Value>(&script->last_value());
    Check(value != nullptr, "WEMath conversion functions return Vec3");
    if (! value) return;
    Check(std::abs(value->x - std::numbers::pi) < 0.001 &&
              std::abs(value->y - std::numbers::pi / 2.0) < 0.001 &&
              std::abs(value->z - 180.0) < 0.001,
          "WEMath deg2rad and rad2deg support function and numeric-coercion forms");
}

void TestVec4Compatibility() {
    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            export function update(value) {
                return new Vec4(value.x + 1, value.y + 2, value.z + 3, value.w + 4);
            }
        )JS",
        "test/vec4_compatibility",
        sr::script::FieldKind::Vec4,
        Parse("{}"),
        Parse("\"1 2 3 4\""));
    Check(script != nullptr, "Vec4 field script compiles");
    if (! script) return;
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::Vec4Value>(&script->last_value());
    Check(value && std::abs(value->x - 2.0) < 0.001 &&
              std::abs(value->y - 4.0) < 0.001 && std::abs(value->z - 6.0) < 0.001 &&
              std::abs(value->w - 8.0) < 0.001,
          "Vec4 fields preserve four-component initial and return values");
}

void TestInvalidVectorReturns() {
    struct Case {
        sr::script::FieldKind kind;
        std::string_view      source;
        std::string_view      sha;
        std::string_view      initial;
        std::string_view      name;
    };

    const std::array cases {
        Case { sr::script::FieldKind::Vec2,
               R"JS(
                   let frame = 0;
                   export function update() {
                       ++frame;
                       if (frame === 1) return new Vec2(1, 2);
                       if (frame === 2) return [3];
                       return { x: 3, y: Infinity };
                   }
               )JS",
               "test/invalid_vec2_return",
               "\"0 0\"",
               "Vec2" },
        Case { sr::script::FieldKind::Vec3,
               R"JS(
                   let frame = 0;
                   export function update() {
                       ++frame;
                       if (frame === 1) return new Vec3(1, 2, 3);
                       if (frame === 2) return { x: 4, y: 5 };
                       return [4, 5, NaN];
                   }
               )JS",
               "test/invalid_vec3_return",
               "\"0 0 0\"",
               "Vec3" },
        Case { sr::script::FieldKind::Vec4,
               R"JS(
                   let frame = 0;
                   export function update() {
                       ++frame;
                       if (frame === 1) return new Vec4(1, 2, 3, 4);
                       if (frame === 2) return [5, 6, 7];
                       return { x: 5, y: 6, z: 7, w: -Infinity };
                   }
               )JS",
               "test/invalid_vec4_return",
               "\"0 0 0 0\"",
               "Vec4" },
    };

    for (const auto& test : cases) {
        sr::script::JsRuntime runtime;
        auto* script = runtime.MakeFieldScript(test.source,
                                               test.sha,
                                               test.kind,
                                               Parse("{}"),
                                               Parse(test.initial));
        Check(script != nullptr, std::string(test.name) + " validation script compiles");
        if (! script) continue;

        runtime.TickAll();
        Check(! std::holds_alternative<std::monostate>(script->last_value()),
              std::string(test.name) + " accepts complete finite components");
        runtime.TickAll();
        Check(std::holds_alternative<std::monostate>(script->last_value()),
              std::string(test.name) + " rejects a missing component");
        runtime.TickAll();
        Check(std::holds_alternative<std::monostate>(script->last_value()),
              std::string(test.name) + " rejects a non-finite component");
    }
}

void TestInvalidVectorPreservesTransform() {
    auto node = rstd::sync::Arc<sr::SceneNode>::make();
    node->SetTranslate(Eigen::Vector3f { 10.0f, 20.0f, 30.0f });

    sr::script::ScriptScene scripts;
    auto* script = scripts.runtime().MakeFieldScript(
        R"JS(
            let frame = 0;
            export function update() {
                ++frame;
                if (frame === 1) return new Vec3(4, 5, 6);
                return { x: 100, y: 200 };
            }
        )JS",
        "test/invalid_vec3_preserves_transform",
        sr::script::FieldKind::Vec3,
        Parse("{}"),
        Parse("\"10 20 30\""));
    Check(script != nullptr, "transform preservation script compiles");
    if (! script) return;

    scripts.AddActuator({ script,
                          sr::script::MakeNodeTransformApply(
                              node.clone(), sr::script::NodeTransformTarget::Translate) });
    scripts.Tick({});
    Check(node->Translate().isApprox(Eigen::Vector3f { 4.0f, 5.0f, 6.0f }),
          "complete Vec3 updates the transform");
    scripts.Tick({});
    Check(std::holds_alternative<std::monostate>(script->last_value()),
          "incomplete Vec3 becomes monostate");
    Check(node->Translate().isApprox(Eigen::Vector3f { 4.0f, 5.0f, 6.0f }),
          "incomplete Vec3 preserves the last valid transform");
}

void TestColorPropertyCoercion() {
    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            export const scriptProperties = createScriptProperties()
                .addColor({ name: 'tint', value: new Vec3(1, 1, 1) })
                .finish();
            export function update() { return scriptProperties.tint; }
        )JS",
        "test/color_property_coercion",
        sr::script::FieldKind::Vec3,
        Parse(R"JSON({"tint":"0.25 0.5 0.75"})JSON"),
        Parse("\"0 0 0\""));
    Check(script != nullptr, "color script property compiles");
    if (! script) return;
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::Vec3Value>(&script->last_value());
    Check(value && std::abs(value->x - 0.25) < 0.001 &&
              std::abs(value->y - 0.5) < 0.001 && std::abs(value->z - 0.75) < 0.001,
          "color script properties coerce authored strings to Vec3");
}

void TestFullwidthSemicolonNormalization() {
    const std::string source = "void main() { gl_FragColor = vec4(1.0)； }\n";
    const std::string output =
        sr::WPShaderParser::PreShaderHeader(source, {}, sr::ShaderType::FRAGMENT);
    Check(output.find("；") == std::string::npos, "fullwidth shader semicolon is removed");
    Check(output.find("gl_FragColor = vec4(1.0);") != std::string::npos,
          "fullwidth shader semicolon becomes ASCII semicolon");
}

void TestDynamicLayerCompatibility() {
    sr::script::JsRuntime runtime;
    auto                  root = rstd::sync::Arc<sr::SceneNode>::make();
    auto                  owner = rstd::sync::Arc<sr::SceneNode>::make();
    root->AppendChild(owner.clone());

    auto* script = runtime.MakeFieldScript(
        R"JS(
            engine.registerAsset('particles/test.json');
            let initial = null;
            let created = null;
            export function init() {
                initial = thisScene.getInitialLayerConfig(thisLayer);
            }
            export function update() {
                if (created === null) {
                    created = thisScene.createLayer({
                        origin: new Vec3(12, 34, 0),
                        size: new Vec2(64, 32),
                        perspective: true,
                        visible: false
                    });
                }
                let state = created.perspective ? 1 : 0;
                if (created.visible) state += 2;
                if (created.isPlaying()) state += 4;
                return new Vec3(initial.alpha, created.origin.x, state);
            }
        )JS",
        "test/dynamic_layer_compatibility",
        sr::script::FieldKind::Vec3,
        Parse("{}"),
        Parse("\"0 0 0\""),
        owner.as_ptr());
    Check(script != nullptr, "dynamic layer script compiles");
    if (! script) return;
    Check(script->RegisteredAssets().size() == 1 &&
              script->RegisteredAssets().front() == "particles/test.json",
          "registerAsset records the evaluated asset path");

    runtime.RegisterInitialLayerConfig(owner.as_ptr(), Parse(R"JSON({"alpha":0.75})JSON"));
    runtime.SetLayerConfigFactory([root_ptr = root.as_ptr()](sr::SceneNode*, sr::Json config) {
        std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
        sr::GetJsonValue(config, "origin", origin, false);
        auto node = rstd::sync::Arc<sr::SceneNode>::make(
            Eigen::Vector3f(origin.data()), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero());
        bool visible = true;
        sr::GetJsonValue(config, "visible", visible, false);
        node->SetVisible(visible);
        node->Stop();
        root_ptr->AppendChild(node.clone());
        return std::optional<rstd::sync::Arc<sr::SceneNode>>(std::move(node));
    });
    runtime.SetSceneRoot(root.as_ptr());
    runtime.TickAll();
    runtime.ClearLayerConfigFactory();

    const auto* value = std::get_if<sr::script::Vec3Value>(&script->last_value());
    Check(value != nullptr, "dynamic layer script returns Vec3");
    if (! value) return;
    Check(std::abs(value->x - 0.75) < 0.001 && std::abs(value->y - 12.0) < 0.001 &&
              value->z == 1.0,
          "initial configs and serialized configuration layers retain authored values");
}

void TestParticleInstanceCompatibility() {
    struct OverrideState {
        float                 alpha { 1.0f };
        float                 rate { 1.0f };
        std::array<float, 3>  color { 1.0f, 1.0f, 1.0f };
        bool                  playing { true };
    } state;

    sr::script::JsRuntime runtime;
    auto                  node = rstd::sync::Arc<sr::SceneNode>::make();
    node->SetLayerPropertyControl(
        [&state](std::string_view field) {
            if (field == "alpha") return std::vector<float> { state.alpha };
            if (field == "rate") return std::vector<float> { state.rate };
            if (field == "color")
                return std::vector<float> { state.color[0], state.color[1], state.color[2] };
            return std::vector<float> {};
        },
        [&state](std::string_view field, std::span<const float> values) {
            if (field == "alpha" && ! values.empty()) state.alpha = values[0];
            if (field == "rate" && ! values.empty()) state.rate = values[0];
            if (field == "color" && values.size() >= 3)
                std::copy_n(values.begin(), 3, state.color.begin());
        });
    node->SetPlaybackControl(
        [&state]() { state.playing = true; },
        [&state]() { state.playing = false; },
        [&state]() { state.playing = false; },
        [&state]() { return state.playing; });

    auto* script = runtime.MakeFieldScript(
        R"JS(
            export function init() {
                thisLayer.instance.alpha = 0.25;
                thisLayer.instance.rate = 2.5;
                thisLayer.instance.color = new Vec3(0.1, 0.2, 0.3);
                thisLayer.pause();
            }
            export function update() {
                const p = thisLayer.instance;
                return new Vec3(p.alpha + p.color.y, p.rate, thisLayer.isPlaying() ? 1 : 0);
            }
        )JS",
        "test/particle_instance_compatibility",
        sr::script::FieldKind::Vec3,
        Parse("{}"),
        Parse("\"0 0 0\""),
        node.as_ptr());
    Check(script != nullptr, "particle instance script compiles");
    if (! script) return;
    runtime.SetSceneRoot(node.as_ptr());
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::Vec3Value>(&script->last_value());
    Check(value && std::abs(value->x - 0.45) < 0.001 &&
              std::abs(value->y - 2.5) < 0.001 && value->z == 0.0,
          "particle instance properties and playback controls use live override state");
}

void TestEffectAndMaterialCompatibility() {
    sr::Scene scene;
    auto      owner = rstd::sync::Arc<sr::SceneNode>::make();
    owner->SetCamera("effects");
    scene.sceneGraph->AppendChild(owner.clone());

    auto camera = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(1920.0, 1080.0, -1.0, 1.0));
    auto effect_layer =
        std::make_shared<sr::SceneImageEffectLayer>(owner.as_ptr(), 1920.0f, 1080.0f,
                                                    "_rt_a", "_rt_b");
    auto effect  = std::make_shared<sr::SceneImageEffect>();
    effect->name = "Glow";

    auto effect_node = rstd::sync::Arc<sr::SceneNode>::make();
    auto mesh        = std::make_shared<sr::SceneMesh>();
    sr::SceneMaterial material;
    sr::SceneShaderVariantDesc variant;
    variant.uniform_aliases["Tint"] = "g_Tint";
    material.customShader.variant   = std::move(variant);
    material.customShader.constValues["g_Tint"] =
        sr::ShaderValue(std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f });
    mesh->AddMaterial(std::move(material));
    effect_node->AddMesh(mesh);
    effect->nodes.push_back({ .sceneNode = effect_node.clone() });
    effect_layer->AddEffect(effect);
    camera->AttatchImgEffect(effect_layer);
    scene.cameras.emplace("effects", camera);

    sr::script::JsRuntime runtime;
    runtime.SetScene(&scene);
    auto* script = runtime.MakeFieldScript(
        R"JS(
            export function update() {
                const effectByIndex = thisLayer.getEffect(0);
                const effectByName = thisLayer.getEffect('Glow');
                effectByIndex.visible = false;
                effectByName.getMaterial(0).Tint = new Vec4(0.1, 0.2, 0.3, 0.4);
                return new Vec4(
                    thisLayer.getEffectCount(),
                    effectByIndex.name === 'Glow' ? 1 : 0,
                    effectByName.visible ? 1 : 0,
                    1);
            }
        )JS",
        "test/effect_material_compatibility",
        sr::script::FieldKind::Vec4,
        Parse("{}"),
        Parse("\"0 0 0 0\""),
        owner.as_ptr());
    Check(script != nullptr, "effect and material script compiles");
    if (! script) return;
    runtime.SetSceneRoot(scene.sceneGraph.as_ptr());
    runtime.TickAll();

    const auto* value = std::get_if<sr::script::Vec4Value>(&script->last_value());
    Check(value && value->x == 1.0 && value->y == 1.0 && value->z == 0.0,
          "effect lookup exposes count, name and live visibility");
    Check(! effect->runtime_visible, "effect visibility writes reach the scene effect stack");
    const auto& tint = mesh->Material()->customShader.constValues.at("g_Tint");
    Check(tint.size() == 4 && std::abs(tint[0] - 0.1f) < 0.001 &&
              std::abs(tint[1] - 0.2f) < 0.001 && std::abs(tint[2] - 0.3f) < 0.001 &&
              std::abs(tint[3] - 0.4f) < 0.001,
          "effect material property writes resolve aliases and preserve Vec4 values");
}

void TestTimelineAnimationCompatibility() {
    auto playback = std::make_shared<sr::SceneAnimationPlayback>(
        "face", 10.0f, 10, "single", false, true);
    sr::SceneAnimationCurve alpha;
    alpha.fps      = 10.0f;
    alpha.length   = 10;
    alpha.mode     = "single";
    alpha.playback = playback;
    alpha.c0.push_back({ .frame = 0, .value = 0.0f });
    alpha.c0.push_back({ .frame = 10, .value = 1.0f });

    sr::SceneNode node;
    node.SetAlphaAnimation(std::move(alpha));

    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            let step = 0;
            export function update() {
                const animation = thisLayer.getAnimation('face');
                if (step === 0) {
                    animation.rate = 2;
                    animation.setFrame(4);
                    animation.play();
                    ++step;
                    return new Vec4(animation.fps, animation.frameCount,
                        animation.duration, animation.name === 'face' && animation.isPlaying()
                            ? animation.getFrame() : -1);
                }
                if (step === 1) {
                    animation.pause();
                    ++step;
                    return new Vec4(animation.getFrame(), animation.isPlaying() ? 1 : 0,
                        animation.rate, 0);
                }
                animation.stop();
                return new Vec4(animation.getFrame(), animation.isPlaying() ? 1 : 0, 0, 0);
            }
        )JS",
        "test/timeline_animation_compatibility",
        sr::script::FieldKind::Vec4,
        Parse("{}"),
        Parse("\"0 0 0 0\""),
        &node);
    Check(script != nullptr, "timeline animation script compiles");
    if (! script) return;

    runtime.TickAll();
    const auto* metadata = std::get_if<sr::script::Vec4Value>(&script->last_value());
    Check(metadata && std::abs(metadata->x - 10.0) < 0.001 &&
              std::abs(metadata->y - 10.0) < 0.001 &&
              std::abs(metadata->z - 1.0) < 0.001 &&
              std::abs(metadata->w - 4.0) < 0.001,
          "getAnimation exposes live metadata and playback state");

    node.TickFieldAnimations(0.0);
    node.TickFieldAnimations(0.25);
    Check(std::abs(node.UserAlpha() - 0.9f) < 0.001,
          "script-controlled rate and frame drive the field curve");

    runtime.TickAll();
    const auto* paused = std::get_if<sr::script::Vec4Value>(&script->last_value());
    Check(paused && std::abs(paused->x - 9.0) < 0.001 && paused->y == 0.0 &&
              std::abs(paused->z - 2.0) < 0.001,
          "animation pause preserves frame and rate");

    runtime.TickAll();
    const auto* stopped = std::get_if<sr::script::Vec4Value>(&script->last_value());
    Check(stopped && stopped->x == 0.0 && stopped->y == 0.0,
          "animation stop resets the script-visible frame");
}

void TestCursorClickOrder() {
    sr::SceneNode node({ 960.0f, 540.0f, 0.0f },
                       { 1.0f, 1.0f, 1.0f },
                       { 0.0f, 0.0f, 0.0f });
    node.SetSize({ 100.0f, 100.0f });

    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            let order = 0;
            export function cursorDown() { order = order * 10 + 1; }
            export function cursorUp() { order = order * 10 + 2; }
            export function cursorClick() { order = order * 10 + 3; }
            export function update() { return order; }
        )JS",
        "test/cursor_click_order",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"),
        &node);
    Check(script != nullptr, "cursor event script compiles");
    if (! script) return;

    sr::script::FrameInputs input;
    input.cursor_x             = 0.5f;
    input.cursor_y             = 0.5f;
    input.cursor_in_window     = true;
    input.mouse_buttons_down   = 1;
    input.mouse_buttons_pressed = 1;
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    const auto* pressed = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(pressed && pressed->v == 1.0, "mouse press dispatches cursorDown without cursorClick");

    input.mouse_buttons_down     = 0;
    input.mouse_buttons_pressed  = 0;
    input.mouse_buttons_released = 1;
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    const auto* released = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(released && released->v == 123.0,
          "same-node release dispatches cursorUp before cursorClick");

    input.mouse_buttons_down     = 1;
    input.mouse_buttons_pressed  = 1;
    input.mouse_buttons_released = 0;
    runtime.SetFrameInputs(input);
    runtime.TickAll();

    input.cursor_x               = 0.0f;
    input.cursor_y               = 0.0f;
    input.mouse_buttons_down     = 0;
    input.mouse_buttons_pressed  = 0;
    input.mouse_buttons_released = 1;
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    const auto* outside = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(outside && outside->v == 12312.0,
          "release outside the pressed node does not dispatch cursorClick");
}

} // namespace

int main() {
    TestVectorAngle2();
    TestMediaCompatibilityFields();
    TestColorScaleHelpers();
    TestMathConversionConstants();
    TestVec4Compatibility();
    TestInvalidVectorReturns();
    TestInvalidVectorPreservesTransform();
    TestColorPropertyCoercion();
    TestFullwidthSemicolonNormalization();
    TestDynamicLayerCompatibility();
    TestParticleInstanceCompatibility();
    TestEffectAndMaterialCompatibility();
    TestTimelineAnimationCompatibility();
    TestCursorClickOrder();
    if (g_failures == 0) std::cout << "ScriptCompatibilityRegression: ok\n";
    return g_failures == 0 ? 0 : 1;
}

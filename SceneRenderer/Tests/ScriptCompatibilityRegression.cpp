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

void TestDynamicLayerOrdering() {
    sr::Scene scene;
    std::array<rstd::sync::Arc<sr::SceneNode>, 3> internal {
        rstd::sync::Arc<sr::SceneNode>::make(),
        rstd::sync::Arc<sr::SceneNode>::make(),
        rstd::sync::Arc<sr::SceneNode>::make(),
    };
    for (auto& node : internal) scene.sceneGraph->AppendChild(node.clone());
    auto      ring = rstd::sync::Arc<sr::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "ring");
    auto body = rstd::sync::Arc<sr::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "body");
    scene.AttachRuntimeNode(*scene.sceneGraph, ring.clone());
    scene.AttachRuntimeNode(*scene.sceneGraph, body.clone());
    (void)scene.ConsumeRenderGraphDirty();

    std::vector<rstd::sync::Arc<sr::SceneNode>> created;
    sr::script::JsRuntime                       runtime;
    runtime.SetScene(&scene);
    runtime.SetLayerFactory([&scene, &created](sr::SceneNode*, sr::script::LayerAssetReference asset) {
        if (asset.path != "models/bar.json")
            return std::optional<rstd::sync::Arc<sr::SceneNode>> {};
        auto node = rstd::sync::Arc<sr::SceneNode>::make();
        scene.AttachRuntimeNode(*scene.sceneGraph, node.clone());
        created.push_back(node.clone());
        return std::optional<rstd::sync::Arc<sr::SceneNode>>(std::move(node));
    });
    auto* script = runtime.MakeFieldScript(
        R"JS(
            engine.registerAsset('models/bar.json');
            let result = -1;
            export function init() {
                const target = thisScene.getLayerIndex(thisLayer);
                const first = thisScene.createLayer('models/bar.json');
                thisScene.sortLayer(first, target);
                const second = thisScene.createLayer('models/bar.json');
                thisScene.sortLayer(second, target);
                result = thisScene.getLayerIndex(first) * 1000
                       + thisScene.getLayerIndex(second) * 100
                       + thisScene.getLayerIndex(thisLayer) * 10
                       + thisScene.getLayerIndex('body');
            }
            export function update() { return result; }
        )JS",
        "test/dynamic_layer_ordering",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"),
        ring.as_ptr());
    Check(script != nullptr, "dynamic layer ordering script compiles");
    if (! script) return;

    runtime.SetSceneRoot(scene.sceneGraph.as_ptr());
    runtime.TickAll();
    runtime.ClearLayerFactory();

    Check(created.size() == 2, "dynamic layer factory creates both requested layers");
    if (created.size() != 2) return;
    const auto& children = scene.sceneGraph->GetChildren();
    auto it = children.begin();
    bool order_ok = children.size() == 7;
    if (order_ok) {
        for (const auto& node : internal) order_ok = order_ok && it++->as_ptr() == node.as_ptr();
        order_ok = it++->as_ptr() == created[1].as_ptr();
        order_ok = order_ok && it++->as_ptr() == created[0].as_ptr();
        order_ok = order_ok && it++->as_ptr() == ring.as_ptr();
        order_ok = order_ok && it->as_ptr() == body.as_ptr();
    }
    Check(order_ok, "sortLayer restores the requested sibling draw order");
    const auto* result = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(result && result->v == 1023.0,
          "getLayerIndex observes reordered layers by object and by name");
    Check(scene.ConsumeRenderGraphDirty(), "dynamic layer sorting invalidates the render graph");
}

void TestWorkshopLayerAssetReference() {
    sr::script::JsRuntime runtime;
    auto                  owner = rstd::sync::Arc<sr::SceneNode>::make();
    std::vector<std::string> paths;
    std::vector<std::string> workshop_ids;
    std::vector<rstd::sync::Arc<sr::SceneNode>> created;
    runtime.SetLayerFactory(
        [&paths, &workshop_ids, &created](sr::SceneNode*, sr::script::LayerAssetReference asset) {
            paths.emplace_back(asset.path);
            workshop_ids.emplace_back(asset.workshop_id.value_or(std::string_view {}));
            auto node = rstd::sync::Arc<sr::SceneNode>::make();
            created.push_back(node.clone());
            return std::optional<rstd::sync::Arc<sr::SceneNode>>(std::move(node));
        });
    auto* script = runtime.MakeFieldScript(
        R"JS(
            export let __workshopId = '2652493753';
            let result = 0;
            export function init() {
                for (let i = 0; i < 12; ++i) {
                    if (thisScene.createLayer('models/bar.json')) ++result;
                }
            }
            export function update() { return result; }
        )JS",
        "test/workshop_layer_asset_reference",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"),
        owner.as_ptr());
    Check(script != nullptr, "workshop layer script compiles without registerAsset");
    if (! script) return;
    runtime.SetSceneRoot(owner.as_ptr());
    runtime.TickAll();
    runtime.ClearLayerFactory();
    Check(created.size() == 12 && paths.size() == 12 && workshop_ids.size() == 12,
          "direct workshop createLayer requests use the dynamic factory without fixed capacity");
    Check(! paths.empty() && paths.front() == "models/bar.json" &&
              workshop_ids.front() == "2652493753",
          "createLayer forwards the authored path and module workshop id");
    const auto* result = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(result && result->v == 12.0,
          "all direct workshop createLayer calls complete successfully");
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

void TestSceneTimelineAnimationCompatibility() {
    auto root = rstd::sync::Arc<sr::SceneNode>::make();
    auto owner = rstd::sync::Arc<sr::SceneNode>::make();
    auto group = rstd::sync::Arc<sr::SceneNode>::make();
    auto animated = rstd::sync::Arc<sr::SceneNode>::make();
    group->AppendChild(animated.clone());
    root->AppendChild(owner.clone());
    root->AppendChild(group.clone());

    auto playback = std::make_shared<sr::SceneAnimationPlayback>(
        "face", 10.0f, 10, "single", false, true);
    animated->RegisterAnimationPlayback(playback);

    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            export function update() {
                const sceneAnimation = thisScene.getAnimation('face');
                const layerAnimation = thisLayer.getAnimation('face');
                sceneAnimation.rate = 2;
                sceneAnimation.setFrame(4);
                sceneAnimation.play();
                return new Vec4(
                    sceneAnimation.name === 'face' ? 1 : 0,
                    sceneAnimation.isPlaying() ? 1 : 0,
                    sceneAnimation.getFrame(),
                    layerAnimation.name === '' && !layerAnimation.isPlaying() ? 1 : 0);
            }
        )JS",
        "test/scene_timeline_animation_compatibility",
        sr::script::FieldKind::Vec4,
        Parse("{}"),
        Parse("\"0 0 0 0\""),
        owner.as_ptr());
    Check(script != nullptr, "scene timeline animation script compiles");
    if (! script) return;

    runtime.SetSceneRoot(root.as_ptr());
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::Vec4Value>(&script->last_value());
    Check(value && value->x == 1.0 && value->y == 1.0 && value->z == 4.0 &&
              value->w == 1.0,
          "thisScene resolves descendant animations without widening thisLayer lookup");
    Check(playback->IsPlaying() && std::abs(playback->Rate() - 2.0) < 0.001 &&
              std::abs(playback->Frame() - 4.0) < 0.001,
          "thisScene animation controls reach the descendant playback");
}

void TestCursorClickOrder() {
    sr::SceneNode node({ 960.0f, 540.0f, 0.0f },
                       { 1.0f, 1.0f, 1.0f },
                       { 0.0f, 0.0f, 0.0f });
    node.SetSize({ 100.0f, 100.0f });
    node.SetSolid(true);

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

void TestSolidCursorDispatch() {
    sr::Scene scene;
    auto bottom = rstd::sync::Arc<sr::SceneNode>::make(
        Eigen::Vector3f { 960.0f, 540.0f, 0.0f },
        Eigen::Vector3f::Ones(),
        Eigen::Vector3f::Zero(),
        "bottom");
    auto top = rstd::sync::Arc<sr::SceneNode>::make(
        Eigen::Vector3f { 960.0f, 540.0f, 0.0f },
        Eigen::Vector3f::Ones(),
        Eigen::Vector3f::Zero(),
        "top");
    auto cover = rstd::sync::Arc<sr::SceneNode>::make(
        Eigen::Vector3f { 960.0f, 540.0f, 0.0f },
        Eigen::Vector3f::Ones(),
        Eigen::Vector3f::Zero(),
        "cover");
    auto parent = rstd::sync::Arc<sr::SceneNode>::make(
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Ones(),
        Eigen::Vector3f::Zero(),
        "parent");
    bottom->SetSize({ 200.0f, 200.0f });
    top->SetSize({ 200.0f, 200.0f });
    cover->SetSize({ 200.0f, 200.0f });
    bottom->SetSolid(true);
    top->SetSolid(true);
    scene.AttachRuntimeNode(*scene.sceneGraph, bottom.clone());
    scene.AttachRuntimeNode(*scene.sceneGraph, parent.clone());
    scene.AttachRuntimeNode(*parent, top.clone());
    scene.AttachRuntimeNode(*scene.sceneGraph, cover.clone());

    sr::script::JsRuntime runtime;
    runtime.SetScene(&scene);
    auto make_script = [&](std::string_view sha, sr::SceneNode* node) {
        return runtime.MakeFieldScript(
            R"JS(
                let clicks = 0;
                export function cursorClick() { ++clicks; }
                export function update() { return clicks; }
            )JS",
            sha,
            sr::script::FieldKind::Scalar,
            Parse("{}"),
            Parse("0"),
            node);
    };
    auto* bottom_script = make_script("test/solid_bottom", bottom.as_ptr());
    auto* top_script    = make_script("test/solid_top", top.as_ptr());
    auto* cover_script = runtime.MakeFieldScript(
        "export function update() { return 0; }",
        "test/non_cursor_cover",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"),
        cover.as_ptr());
    Check(bottom_script != nullptr && top_script != nullptr && cover_script != nullptr,
          "overlapping cursor scripts compile");
    if (bottom_script == nullptr || top_script == nullptr || cover_script == nullptr) return;
    runtime.SetSceneRoot(scene.sceneGraph.as_ptr());

    auto click = [&]() {
        sr::script::FrameInputs input;
        input.cursor_x              = 0.5f;
        input.cursor_y              = 0.5f;
        input.cursor_in_window      = true;
        input.mouse_buttons_down    = 1;
        input.mouse_buttons_pressed = 1;
        runtime.SetFrameInputs(input);
        runtime.TickAll();
        input.mouse_buttons_down     = 0;
        input.mouse_buttons_pressed  = 0;
        input.mouse_buttons_released = 1;
        runtime.SetFrameInputs(input);
        runtime.TickAll();
    };

    click();
    auto* bottom_clicks = std::get_if<sr::script::ScalarValue>(&bottom_script->last_value());
    auto* top_clicks    = std::get_if<sr::script::ScalarValue>(&top_script->last_value());
    Check(bottom_clicks && top_clicks && bottom_clicks->v == 1.0 && top_clicks->v == 1.0,
          "overlapping visible solid scripts each receive a click");

    top->SetVisible(false);
    click();
    bottom_clicks = std::get_if<sr::script::ScalarValue>(&bottom_script->last_value());
    top_clicks    = std::get_if<sr::script::ScalarValue>(&top_script->last_value());
    Check(bottom_clicks && top_clicks && bottom_clicks->v == 2.0 && top_clicks->v == 2.0,
          "an invisible solid cursor layer remains interactive");

    top->SetVisible(true);
    parent->SetVisible(false);
    click();
    bottom_clicks = std::get_if<sr::script::ScalarValue>(&bottom_script->last_value());
    top_clicks    = std::get_if<sr::script::ScalarValue>(&top_script->last_value());
    Check(bottom_clicks && top_clicks && bottom_clicks->v == 3.0 && top_clicks->v == 2.0,
          "a node under an invisible ancestor does not receive cursor input");

    parent->SetVisible(true);
    top->SetSolid(false);
    click();
    bottom_clicks = std::get_if<sr::script::ScalarValue>(&bottom_script->last_value());
    top_clicks    = std::get_if<sr::script::ScalarValue>(&top_script->last_value());
    Check(bottom_clicks && top_clicks && bottom_clicks->v == 4.0 && top_clicks->v == 2.0,
          "a non-solid node does not receive or block cursor input");

    top->SetSolid(true);
    sr::script::JsRuntime drag_runtime;
    drag_runtime.SetScene(&scene);
    auto make_drag_script = [&](std::string_view sha, sr::SceneNode* node) {
        return drag_runtime.MakeFieldScript(
            R"JS(
                let downs = 0;
                let moves = 0;
                let ups = 0;
                export function cursorDown() { ++downs; }
                export function cursorMove() { ++moves; }
                export function cursorUp() { ++ups; }
                export function update() { return downs * 100 + ups * 10 + moves; }
            )JS",
            sha,
            sr::script::FieldKind::Scalar,
            Parse("{}"),
            Parse("0"),
            node);
    };
    auto* bottom_drag = make_drag_script("test/drag_bottom", bottom.as_ptr());
    auto* top_drag    = make_drag_script("test/drag_top", top.as_ptr());
    Check(bottom_drag != nullptr && top_drag != nullptr,
          "overlapping drag scripts compile");
    if (bottom_drag != nullptr && top_drag != nullptr) {
        drag_runtime.SetSceneRoot(scene.sceneGraph.as_ptr());
        sr::script::FrameInputs input;
        input.cursor_x              = 0.5f;
        input.cursor_y              = 0.5f;
        input.cursor_in_window      = true;
        input.mouse_buttons_down    = 1;
        input.mouse_buttons_pressed = 1;
        drag_runtime.SetFrameInputs(input);
        drag_runtime.TickAll();

        input.cursor_x              = 0.0f;
        input.cursor_y              = 0.0f;
        input.mouse_buttons_pressed = 0;
        drag_runtime.SetFrameInputs(input);
        drag_runtime.TickAll();

        input.mouse_buttons_down     = 0;
        input.mouse_buttons_released = 1;
        drag_runtime.SetFrameInputs(input);
        drag_runtime.TickAll();

        const auto* bottom_value =
            std::get_if<sr::script::ScalarValue>(&bottom_drag->last_value());
        const auto* top_value = std::get_if<sr::script::ScalarValue>(&top_drag->last_value());
        Check(bottom_value && top_value && bottom_value->v == 113.0 && top_value->v == 113.0,
              "overlapping scripts keep independent drag capture through release outside");
    }

    sr::SceneNode property_node;
    Check(property_node.Solid(), "layers without authored solid metadata remain interactive");
    sr::script::JsRuntime property_runtime;
    auto* property_script = property_runtime.MakeFieldScript(
        R"JS(
            export function update() {
                thisLayer.solid = false;
                return thisLayer.solid;
            }
        )JS",
        "test/solid_property",
        sr::script::FieldKind::Bool,
        Parse("{}"),
        Parse("true"),
        &property_node);
    Check(property_script != nullptr, "solid property script compiles");
    if (property_script != nullptr) {
        property_runtime.TickAll();
        const auto* value = std::get_if<sr::script::BoolValue>(&property_script->last_value());
        Check(value && ! value->v && ! property_node.Solid(),
              "thisLayer.solid reads and writes the live node state");
    }
}

void TestScalarOriginAssignment() {
    sr::SceneNode node(
        { 100.0f, 200.0f, 3.0f }, Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero());
    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            export function update() {
                thisLayer.origin = (-1000, -1000, 0);
                return thisLayer.origin;
            }
        )JS",
        "test/scalar_origin_assignment",
        sr::script::FieldKind::Vec3,
        Parse("{}"),
        Parse("\"100 200 3\""),
        &node);
    Check(script != nullptr, "scalar origin assignment script compiles");
    if (script == nullptr) return;
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::Vec3Value>(&script->last_value());
    Check(value && value->x == 0.0 && value->y == 0.0 && value->z == 0.0 &&
              node.Translate() == Eigen::Vector3f::Zero(),
          "a numeric origin assignment splats to all three components");
}

void TestWritableScriptProperties() {
    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            export var scriptProperties = createScriptProperties()
                .addCheckbox({ name: 'aaa', value: false })
                .addCheckbox({ name: 'ddd', value: false })
                .addCheckbox({ name: 'ccc', value: false })
                .finish();
            function advance() {
                if (scriptProperties.aaa) {
                    scriptProperties.aaa = false;
                    scriptProperties.ddd = true;
                    return;
                }
                if (scriptProperties.ddd) {
                    scriptProperties.ddd = false;
                    scriptProperties.ccc = true;
                }
            }
            export function update() {
                advance();
                return scriptProperties.aaa ? 1 : scriptProperties.ddd ? 2 :
                       scriptProperties.ccc ? 3 : 0;
            }
        )JS",
        "test/writable_script_properties",
        sr::script::FieldKind::Scalar,
        Parse(R"({"aaa":true,"ddd":false,"ccc":false})"),
        Parse("0"));
    Check(script != nullptr, "writable script properties compile");
    if (script == nullptr) return;

    runtime.TickAll();
    const auto* first = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(first && first->v == 2.0,
          "script property assignment overrides the configured value");

    runtime.TickAll();
    const auto* second = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(second && second->v == 3.0,
          "script property assignments persist across updates");
}

void TestPuppetAnimationCompatibility() {
    struct State {
        sr::script::AnimationLayerSnapshot snapshot;
        bool                               single_called { false };
    };
    auto regular = std::make_shared<State>();
    regular->snapshot = {
        .fps         = 30.0,
        .frame_count = 60,
        .duration    = 2.0,
        .name        = "ear",
        .rate        = 1.0,
        .blend       = 1.0,
        .frame       = 0.0,
        .visible     = false,
        .playing     = false,
    };
    auto single = std::make_shared<State>();
    single->snapshot = {
        .fps         = 30.0,
        .frame_count = 30,
        .duration    = 1.0,
        .name        = "turn",
        .rate        = 1.0,
        .blend       = 1.0,
        .frame       = 0.0,
        .visible     = true,
        .playing     = true,
    };
    auto control = [](const std::shared_ptr<State>& state) {
        return sr::script::AnimationLayerControl {
            .snapshot = [state] { return std::optional(state->snapshot); },
            .set_name = [state](std::string name) { state->snapshot.name = std::move(name); },
            .set_rate = [state](double rate) { state->snapshot.rate = rate; },
            .set_blend = [state](double blend) { state->snapshot.blend = blend; },
            .set_visible = [state](bool visible) { state->snapshot.visible = visible; },
            .set_frame = [state](double frame) { state->snapshot.frame = frame; },
            .play = [state] { state->snapshot.playing = true; },
            .pause = [state] { state->snapshot.playing = false; },
            .stop = [state] {
                state->snapshot.playing = false;
                state->snapshot.frame   = 0.0;
            },
        };
    };

    sr::SceneNode node;
    sr::script::JsRuntime runtime;
    runtime.SetAnimationLayerResolvers(
        [](sr::SceneNode*) { return std::size_t { 1 }; },
        [regular, control](sr::SceneNode*, const sr::script::AnimationLayerKey& key)
            -> std::optional<sr::script::AnimationLayerControl> {
            if (const auto* index = std::get_if<std::size_t>(&key); index && *index == 0)
                return control(regular);
            if (const auto* name = std::get_if<std::string>(&key); name && *name == "ear")
                return control(regular);
            return std::nullopt;
        },
        [single, control](sr::SceneNode*, std::string_view name)
            -> std::optional<sr::script::AnimationLayerControl> {
            if (name != "turn") return std::nullopt;
            single->single_called = true;
            return control(single);
        });
    auto* script = runtime.MakeFieldScript(
        R"JS(
            let step = 0;
            export function update() {
                if (step++ === 0) {
                    const animation = thisLayer.getAnimationLayer('ear');
                    animation.visible = true;
                    animation.rate = 2;
                    animation.blend = 0.25;
                    animation.setFrame(4);
                    animation.play();
                    return new Vec4(thisLayer.getAnimationLayerCount(),
                        animation.visible ? 1 : 0, animation.getFrame(),
                        animation.isPlaying() ? 1 : 0);
                }
                const animation = thisLayer.playSingleAnimation('turn');
                animation.stop();
                return new Vec4(animation.name === 'turn' ? 1 : 0,
                    animation.isPlaying() ? 1 : 0, animation.visible ? 1 : 0,
                    animation.frameCount);
            }
        )JS",
        "test/puppet_animation_compatibility",
        sr::script::FieldKind::Vec4,
        Parse("{}"),
        Parse("\"0 0 0 0\""),
        &node);
    Check(script != nullptr, "puppet animation script compiles");
    if (! script) return;

    runtime.TickAll();
    const auto* first = std::get_if<sr::script::Vec4Value>(&script->last_value());
    Check(first && first->x == 1.0 && first->y == 1.0 && first->z == 4.0 && first->w == 1.0,
          "getAnimationLayer exposes live puppet state");
    Check(regular->snapshot.visible && regular->snapshot.playing &&
              std::abs(regular->snapshot.rate - 2.0) < 0.001 &&
              std::abs(regular->snapshot.blend - 0.25) < 0.001,
          "puppet animation setters reach the resolver");

    runtime.TickAll();
    const auto* second = std::get_if<sr::script::Vec4Value>(&script->last_value());
    Check(single->single_called && second && second->x == 1.0 && second->y == 0.0 &&
              second->z == 1.0 && second->w == 30.0,
          "playSingleAnimation returns a controllable temporary layer");
}

void TestAnimationEventDispatch() {
    sr::SceneNode node;
    auto playback = std::make_shared<sr::SceneAnimationPlayback>(
        "900",
        30.0f,
        30,
        "single",
        false,
        true,
        std::vector<sr::SceneAnimationEvent> { { .frame = 30, .name = "houye" } });
    node.RegisterAnimationPlayback(playback);

    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            let value = 0;
            export function animationEvent(event) {
                if (event.name === 'houye') value = event.frame;
            }
            export function update() { return value; }
        )JS",
        "test/animation_event_dispatch",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"),
        &node);
    Check(script != nullptr, "animation event script compiles");
    if (! script) return;

    node.TickFieldAnimations(0.0);
    playback->Play();
    node.TickFieldAnimations(0.4);
    runtime.TickAll();
    const auto* before = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(before && before->v == 0.0, "animation event waits for its marker");

    node.TickFieldAnimations(1.2);
    runtime.TickAll();
    const auto* after = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(after && after->v == 30.0, "animation event fires when a frame step crosses its marker");
}

void TestProjectedCursorHit() {
    sr::SceneNode node;
    node.SetSize({ 100.0f, 100.0f });
    node.SetSolid(true);

    sr::script::JsRuntime runtime;
    runtime.SetCursorProjectionResolver([](sr::SceneNode*) {
        Eigen::Matrix4d projection = Eigen::Matrix4d::Identity();
        projection(0, 0) = 0.01;
        projection(1, 1) = 0.01;
        projection(0, 3) = 0.5;
        return std::optional(sr::script::CursorProjection {
            .model                 = Eigen::Matrix4d::Identity(),
            .model_view_projection = projection,
        });
    });
    auto* script = runtime.MakeFieldScript(
        R"JS(
            let result = new Vec2(0, 0);
            export function cursorClick(event) {
                result = new Vec2(event.localPosition.x + 1, event.worldPosition.x + 1);
            }
            export function update() { return result; }
        )JS",
        "test/projected_cursor_hit",
        sr::script::FieldKind::Vec2,
        Parse("{}"),
        Parse("\"0 0\""),
        &node);
    Check(script != nullptr, "projected cursor script compiles");
    if (! script) return;

    sr::script::FrameInputs input;
    input.cursor_x              = 0.75f;
    input.cursor_y              = 0.5f;
    input.cursor_in_window      = true;
    input.mouse_buttons_down    = 1;
    input.mouse_buttons_pressed = 1;
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    input.mouse_buttons_down     = 0;
    input.mouse_buttons_pressed  = 0;
    input.mouse_buttons_released = 1;
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    const auto* result = std::get_if<sr::script::Vec2Value>(&script->last_value());
    Check(result && std::abs(result->x - 1.0) < 0.001 &&
              std::abs(result->y - 1.0) < 0.001,
          "cursor hit testing uses the rendered projection and reports local coordinates");
}

void TestDegenerateProjectedCursorMisses() {
    sr::SceneNode node;
    node.SetSize({ 100.0f, 100.0f });
    node.SetSolid(true);

    sr::script::JsRuntime runtime;
    runtime.SetCursorProjectionResolver([](sr::SceneNode*) {
        Eigen::Matrix4d projection = Eigen::Matrix4d::Zero();
        projection(3, 3) = 1.0;
        return std::optional(sr::script::CursorProjection {
            .model                 = Eigen::Matrix4d::Identity(),
            .model_view_projection = projection,
        });
    });
    auto* script = runtime.MakeFieldScript(
        R"JS(
            let clicks = 0;
            export function cursorClick() { clicks++; }
            export function update() { return clicks; }
        )JS",
        "test/degenerate_projected_cursor_miss",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"),
        &node);
    Check(script != nullptr, "degenerate projected cursor script compiles");
    if (! script) return;

    sr::script::FrameInputs input;
    input.cursor_x              = 0.5f;
    input.cursor_y              = 0.5f;
    input.cursor_in_window      = true;
    input.mouse_buttons_down    = 1;
    input.mouse_buttons_pressed = 1;
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    input.mouse_buttons_down     = 0;
    input.mouse_buttons_pressed  = 0;
    input.mouse_buttons_released = 1;
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    const auto* clicks = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(clicks && clicks->v == 0.0, "degenerate projected geometry cannot receive clicks");
}

void TestPrimitiveEngineUserPropertyValues() {
    sr::script::JsRuntime runtime;
    auto* script = runtime.MakeFieldScript(
        R"JS(
            export function update() {
                return engine.userProperties.quality === 2 &&
                       engine.userProperties.enabled === true ? 1 : 0;
            }
        )JS",
        "test/primitive_engine_user_properties",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"));
    Check(script != nullptr, "primitive user-property script compiles");
    if (! script) return;

    runtime.SetUserProperty("quality", Parse(R"({"type":"combo","value":2})"));
    runtime.SetUserProperty("enabled", Parse(R"({"type":"bool","value":true})"));
    runtime.TickAll();
    const auto* value = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(value && value->v == 1.0,
          "engine.userProperties exposes unwrapped primitive descriptor values");
}

void TestMixedAudioBufferResolutions() {
    sr::script::JsRuntime    runtime;
    sr::script::FrameInputs input;
    for (std::size_t i = 0; i < input.audio_average.size(); ++i)
        input.audio_average[i] = static_cast<float>(100 + i);
    runtime.SetFrameInputs(input);

    auto* low = runtime.MakeFieldScript(
        R"JS(
            const audio = engine.registerAudioBuffers(16);
            export function update() {
                return audio.average.length * 1000 + audio.average[15];
            }
        )JS",
        "test/audio_buffers_mixed_low",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"));
    auto* full = runtime.MakeFieldScript(
        R"JS(
            const audio = engine.registerAudioBuffers(64);
            export function update() {
                return audio.average.length * 1000 + audio.average[63];
            }
        )JS",
        "test/audio_buffers_mixed_full",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"));
    Check(low != nullptr && full != nullptr,
          "mixed audio buffer resolution scripts compile");
    if (! low || ! full) return;

    runtime.TickAll();
    const auto* low_first = std::get_if<sr::script::ScalarValue>(&low->last_value());
    const auto* full_first = std::get_if<sr::script::ScalarValue>(&full->last_value());
    Check(low_first && std::abs(low_first->v - 16161.5) < 0.001,
          "16-bin audio buffer keeps its independent resolution");
    Check(full_first && std::abs(full_first->v - 64163.0) < 0.001,
          "64-bin audio buffer keeps its independent resolution");

    for (std::size_t i = 0; i < input.audio_average.size(); ++i)
        input.audio_average[i] = static_cast<float>(200 + i);
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    const auto* low_second = std::get_if<sr::script::ScalarValue>(&low->last_value());
    const auto* full_second = std::get_if<sr::script::ScalarValue>(&full->last_value());
    Check(low_second && std::abs(low_second->v - 16261.5) < 0.001,
          "16-bin audio cache refreshes without changing shape");
    Check(full_second && std::abs(full_second->v - 64263.0) < 0.001,
          "64-bin audio cache refreshes without changing shape");
}

void TestSceneLayerEnumeration() {
    sr::Scene scene;
    auto      internal = rstd::sync::Arc<sr::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "internal");
    scene.sceneGraph->AppendChild(internal.clone());

    std::array<rstd::sync::Arc<sr::SceneNode>, 3> layers {
        rstd::sync::Arc<sr::SceneNode>::make(
            Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "background"),
        rstd::sync::Arc<sr::SceneNode>::make(
            Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "dock"),
        rstd::sync::Arc<sr::SceneNode>::make(
            Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "launcher"),
    };
    std::int32_t id = 601;
    for (auto& layer : layers) {
        scene.sceneGraph->AppendChild(layer.clone());
        scene.RegisterNode(*layer, sr::WallpaperLayerId { .value = id++ });
    }

    sr::script::JsRuntime runtime;
    runtime.SetScene(&scene);
    auto* script = runtime.MakeFieldScript(
        R"JS(
            let result = -1;
            export function init() {
                const count = thisScene.getLayerCount();
                let consistent = 1;
                for (let i = 0; i < count; ++i) {
                    const layer = thisScene.getLayer(i);
                    if (typeof layer.name !== 'string' || layer.name === '') consistent = 0;
                    if (thisScene.getLayerIndex(layer) !== i) consistent = 0;
                }
                const missing = thisScene.getLayer(count);
                if (typeof missing.name !== 'string' || missing.name !== '') consistent = 0;
                if (thisScene.getLayer('dock').name !== 'dock') consistent = 0;
                if (thisScene.enumerateLayers().length !== count) consistent = 0;
                result = count * 10 + consistent;
            }
            export function update() { return result; }
        )JS",
        "test/scene_layer_enumeration",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"),
        layers[1].as_ptr());
    Check(script != nullptr, "scene layer enumeration script compiles");
    if (! script) return;

    runtime.SetSceneRoot(scene.sceneGraph.as_ptr());
    runtime.TickAll();
    const auto* result = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(result && result->v == 31.0,
          "getLayerCount, numeric getLayer and enumerateLayers share the getLayerIndex order");
}

void TestFieldScriptUpdateDetection() {
    sr::script::JsRuntime runtime;
    auto                  node = rstd::sync::Arc<sr::SceneNode>::make();
    auto*                 hover = runtime.MakeFieldScript(
        R"JS(
            export let __workshopId = '3674038504';
            export function cursorEnter() {}
            export function cursorLeave() {}
        )JS",
        "test/visible_hit_area_without_update",
        sr::script::FieldKind::Bool,
        Parse("{}"),
        Parse("false"),
        node.as_ptr());
    Check(hover != nullptr && ! hover->HasUpdate(),
          "a cursor-only visible binding exports no update");

    auto* driven = runtime.MakeFieldScript(
        R"JS(
            let on = false;
            export function cursorClick() { on = ! on; }
            export function update() { return on; }
        )JS",
        "test/visible_toggle_with_update",
        sr::script::FieldKind::Bool,
        Parse("{}"),
        Parse("false"),
        node.as_ptr());
    Check(driven != nullptr && driven->HasUpdate(),
          "a toggling visible binding exports update");
}

void TestUserShortcutOpening() {
    sr::SceneNode node({ 960.0f, 540.0f, 0.0f },
                       { 1.0f, 1.0f, 1.0f },
                       { 0.0f, 0.0f, 0.0f });
    node.SetSize({ 100.0f, 100.0f });
    node.SetSolid(true);

    std::vector<std::pair<std::string, std::string>> opened;
    sr::script::JsRuntime                           runtime;
    runtime.SetUserShortcutOpener([&opened](std::string_view name, std::string_view target) {
        opened.emplace_back(std::string(name), std::string(target));
        return true;
    });
    runtime.SetUserProperty(
        "s01c", Parse(R"({"type":"usershortcut","value":"https://example.com/launch"})"));

    auto* script = runtime.MakeFieldScript(
        R"JS(
            let result = 0;
            export function cursorUp() {
                if (engine.openUserShortcut('s01c')) result += 1;
                if (engine.openUserShortcut('s01c')) result += 1000;
            }
            export function cursorClick() {
                if (engine.openUserShortcut('launcher1')) result += 100;
                if (engine.openUserShortcut(' s01c ')) result += 10;
            }
            export function update() {
                if (engine.openUserShortcut('s01c')) result += 10000;
                return result;
            }
        )JS",
        "test/user_shortcut_opening",
        sr::script::FieldKind::Scalar,
        Parse("{}"),
        Parse("0"),
        &node);
    Check(script != nullptr, "user shortcut script compiles");
    if (! script) return;

    sr::script::FrameInputs input;
    input.cursor_x              = 0.5f;
    input.cursor_y              = 0.5f;
    input.cursor_in_window      = true;
    input.mouse_buttons_down    = 1;
    input.mouse_buttons_pressed = 1;
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    Check(opened.empty(), "update and cursorDown alone open no user shortcut");

    input.mouse_buttons_down     = 0;
    input.mouse_buttons_pressed  = 0;
    input.mouse_buttons_released = 1;
    runtime.SetFrameInputs(input);
    runtime.TickAll();
    const auto* result = std::get_if<sr::script::ScalarValue>(&script->last_value());
    Check(result && result->v == 11.0,
          "openUserShortcut succeeds once per cursor callback and never outside one");
    Check(opened.size() == 2, "an unbound shortcut key keeps the click budget unspent");
    if (opened.size() == 2) {
        Check(opened.front().first == "s01c" &&
                  opened.front().second == "https://example.com/launch" &&
                  opened.back() == opened.front(),
              "the host receives the trimmed property key and its resolved target");
    }
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
    TestDynamicLayerOrdering();
    TestWorkshopLayerAssetReference();
    TestParticleInstanceCompatibility();
    TestEffectAndMaterialCompatibility();
    TestTimelineAnimationCompatibility();
    TestSceneTimelineAnimationCompatibility();
    TestCursorClickOrder();
    TestSolidCursorDispatch();
    TestScalarOriginAssignment();
    TestWritableScriptProperties();
    TestPuppetAnimationCompatibility();
    TestAnimationEventDispatch();
    TestProjectedCursorHit();
    TestDegenerateProjectedCursorMisses();
    TestPrimitiveEngineUserPropertyValues();
    TestMixedAudioBufferResolutions();
    TestSceneLayerEnumeration();
    TestFieldScriptUpdateDetection();
    TestUserShortcutOpening();
    if (g_failures == 0) std::cout << "ScriptCompatibilityRegression: ok\n";
    return g_failures == 0 ? 0 : 1;
}

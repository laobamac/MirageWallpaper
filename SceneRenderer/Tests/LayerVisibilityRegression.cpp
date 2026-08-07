#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

import sr.scene;
import sr.json;
import rstd;
import eigen;

namespace
{

int g_failures = 0;

void Check(bool ok, std::string_view what) {
    if (ok) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << '\n';
}

sr::Json Prop(std::string_view source) {
    auto parsed = sr::ParseJson(source);
    if (parsed.is_err()) {
        ++g_failures;
        std::cerr << "FAIL: bad test json: " << source << '\n';
        return sr::Json::Null();
    }
    return parsed.unwrap();
}

bool Elidable(const sr::Scene& scene, std::int32_t id) {
    return scene.elidable_layer_ids.count(id) != 0;
}

sr::SceneUserVisibilityBinding Binding(std::string key, std::string_view condition) {
    sr::SceneUserVisibilityBinding out;
    out.key = std::move(key);
    if (! condition.empty()) {
        out.condition     = Prop(condition);
        out.has_condition = true;
    }
    return out;
}

// Workshop 3351163962 / 3461971428 shape: a container layer gates a whole
// language or component group through a combo user property, and each child
// text layer carries its own font-variant binding on a different property.
struct Fixture {
    sr::Scene                          scene;
    rstd::sync::Arc<sr::SceneNode>     container = rstd::sync::Arc<sr::SceneNode>::make();
    rstd::sync::Arc<sr::SceneNode>     child     = rstd::sync::Arc<sr::SceneNode>::make();

    Fixture() {
        container->ID() = 1;
        child->ID()     = 2;
        container->SetVisibleUserBinding(Binding("language", "\"2\""));
        child->SetVisibleUserBinding(Binding("pbrfonttype", "\"1\""));
        container->AppendChild(child.clone());
        scene.sceneGraph->AppendChild(container.clone());
    }
};

void TestResolution() {
    // Combo values reach the renderer as numbers (the wire string "1" is
    // re-parsed against the descriptor type); conditions stay strings.
    auto combo = Prop(R"({"type":"combo","value":1})");
    auto match = sr::ResolveSceneUserVisibilityBinding(Binding("language", "\"1\""), combo);
    Check(match.is_some() && *match, "combo number payload matches string condition");

    auto miss = sr::ResolveSceneUserVisibilityBinding(Binding("language", "\"2\""), combo);
    Check(miss.is_some() && ! *miss, "combo number payload rejects other condition");

    auto flag = Prop(R"({"type":"bool","value":false})");
    auto off  = sr::ResolveSceneUserVisibilityBinding(Binding("week1", ""), flag);
    Check(off.is_some() && ! *off, "bool payload resolves without condition");
}

void TestElisionRepair() {
    Fixture f;
    // Parse-time code hides a node through SceneNode::SetVisible directly.
    // The render graph never reads that flag, so Scene must still be able to
    // pick the hide up on the next binding pass.
    f.container->SetVisible(false);
    Check(! Elidable(f.scene, 1), "container starts outside the elision set");

    const bool queued = f.scene.SetNodeVisible(*f.container.as_ptr(), false);
    Check(queued, "same-value hide is queued while elision disagrees");
    Check(f.scene.CommitNodeVisibilityChanges(), "commit reports a graph rebuild");
    Check(Elidable(f.scene, 1), "container is elidable after commit");

    Check(! f.scene.SetNodeVisible(*f.container.as_ptr(), false), "repeat hide is a no-op");
    Check(! f.scene.CommitNodeVisibilityChanges(), "no rebuild without a real change");

    Check(f.scene.SetNodeVisible(*f.container.as_ptr(), true), "show is queued");
    Check(f.scene.CommitNodeVisibilityChanges(), "show reports a graph rebuild");
    Check(! Elidable(f.scene, 1), "container leaves the elision set when shown");
}

void TestContainerSurvivesChildBinding() {
    Fixture f;
    f.container->SetVisible(false);
    f.scene.MarkLayerVisibilityElidable(sr::WallpaperLayerId { .value = 1 });
    f.child->SetVisible(false);
    f.scene.MarkLayerVisibilityElidable(sr::WallpaperLayerId { .value = 2 });

    // Load applies every user property in turn. The child's own font binding
    // matches and lights it back up; the container gate must not be touched,
    // because the graph builder derives subtree skipping from it.
    auto font = Prop(R"({"type":"combo","value":1})");
    f.scene.ApplyUserNodeVisibilityBindings("pbrfonttype", font);
    Check(Elidable(f.scene, 1), "container stays elidable while a child binding fires");

    auto language = Prop(R"({"type":"combo","value":1})");
    f.scene.ApplyUserNodeVisibilityBindings("language", language);
    Check(Elidable(f.scene, 1), "container stays elidable when its own gate is off");

    auto english = Prop(R"({"type":"combo","value":2})");
    f.scene.ApplyUserNodeVisibilityBindings("language", english);
    Check(! Elidable(f.scene, 1), "container is released when its gate turns on");
}

void TestDependencyCaptureAlpha() {
    sr::SceneNode source;
    source.SetBaseColor(Eigen::Vector3f::Ones(), 0.6f);
    source.SetVisible(false);

    Check(source.IsAlphaOverridden(), "hidden source overrides composite alpha");
    Check(std::abs(source.EffectiveAlpha()) < 0.0001f,
          "hidden source is transparent in the main composite");
    Check(! source.IsAlphaOverridden(sr::SceneRenderAlphaMode::DependencyCapture),
          "visibility alone does not override dependency alpha");
    Check(std::abs(source.EffectiveAlpha(sr::SceneRenderAlphaMode::DependencyCapture) - 0.6f) <
              0.0001f,
          "dependency capture preserves authored opacity");

    source.SetUserAlpha(0.25f);
    Check(source.IsAlphaOverridden(sr::SceneRenderAlphaMode::DependencyCapture),
          "dependency capture retains explicit opacity overrides");
    Check(std::abs(source.EffectiveAlpha(sr::SceneRenderAlphaMode::DependencyCapture) - 0.25f) <
              0.0001f,
          "dependency capture uses explicit opacity");

    sr::SceneNode final;
    final.SetBaseColor(Eigen::Vector3f::Ones(), 1.0f);
    final.SetAlphaSource(&source);
    Check(std::abs(final.EffectiveAlpha()) < 0.0001f,
          "final composite inherits hidden source visibility");
    Check(std::abs(final.EffectiveAlpha(sr::SceneRenderAlphaMode::DependencyCapture) - 0.25f) <
              0.0001f,
          "dependency final pass inherits source opacity without visibility");
}

void TestFinalOutputOverrideRestoresAuthoredTarget() {
    sr::SceneNode owner;
    sr::SceneImageEffectLayer layer(
        &owner, 100.0f, 100.0f, "_rt_effect_pingpong_a_test", "_rt_effect_pingpong_b_test");
    auto effect      = std::make_shared<sr::SceneImageEffect>();
    auto effect_node = rstd::sync::Arc<sr::SceneNode>::make();
    auto effect_mesh = std::make_shared<sr::SceneMesh>();
    effect_mesh->AddMaterial(sr::SceneMaterial {});
    effect_node->AddMesh(effect_mesh);
    effect->nodes.push_back(sr::SceneImageEffectNode {
        .output    = "_rt_effect_pingpong_b_test",
        .sceneNode = effect_node.clone(),
    });
    layer.AddEffect(effect);
    layer.SetFinalTarget("_rt_authored");

    sr::SceneMesh default_mesh;
    layer.SetFinalOutputOverride("_rt_link_25", true);
    layer.ResolveEffect(default_mesh, "effect");
    Check(effect->nodes.back().output == "_rt_link_25",
          "dependency graph overrides the final target");

    layer.ClearFinalOutputOverride();
    layer.ResolveEffect(default_mesh, "effect");
    Check(effect->nodes.back().output == "_rt_authored",
          "visible graph restores the authored final target");
}

} // namespace

int main() {
    TestResolution();
    TestElisionRepair();
    TestContainerSurvivesChildBinding();
    TestDependencyCaptureAlpha();
    TestFinalOutputOverrideRestoresAuthoredTarget();
    if (g_failures == 0) std::cout << "LayerVisibilityRegression: ok\n";
    return g_failures == 0 ? 0 : 1;
}

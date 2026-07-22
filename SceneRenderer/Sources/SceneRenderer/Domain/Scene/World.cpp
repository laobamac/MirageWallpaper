module sr.scene;
import eigen;
import rstd;
import rstd.cppstd;

import sr.fs;

namespace sr
{

namespace
{
void delete_vfs(void* p) noexcept { delete static_cast<fs::VFS*>(p); }

uint32_t next_scene_resource_generation() {
    static std::atomic_uint32_t next { 1 };
    return next.fetch_add(1, std::memory_order_relaxed);
}

uint64_t next_render_scene_version() {
    static std::atomic_uint64_t next { 1 };
    return next.fetch_add(1, std::memory_order_relaxed);
}

uint32_t index_from_size(std::size_t size) { return static_cast<uint32_t>(size); }

template<typename Id>
bool valid_index(Id id, uint32_t generation, std::size_t size) {
    return id.generation == generation && static_cast<std::size_t>(id.index) < size;
}

template<typename Id>
bool valid_render_index(Id id, uint64_t generation, std::size_t size) {
    return id.generation == generation && static_cast<std::size_t>(id.index) < size;
}

uint64_t scene_id_key(uint32_t index, uint32_t generation) {
    return (static_cast<uint64_t>(generation) << 32) | static_cast<uint64_t>(index);
}

uint64_t scene_id_key(SceneDrawItemId id) { return scene_id_key(id.index, id.generation); }

uint64_t scene_id_key(SceneMaterialId id) { return scene_id_key(id.index, id.generation); }

uint64_t scene_id_key(SceneMeshId id) { return scene_id_key(id.index, id.generation); }

float cubic(float p0, float p1, float p2, float p3, float t) {
    float omt = 1.0f - t;
    return omt * omt * omt * p0 + 3.0f * omt * omt * t * p1 + 3.0f * omt * t * t * p2 +
           t * t * t * p3;
}

float curve_end_frame(const SceneAnimationCurve& curve) {
    int  end         = curve.length;
    auto absorb_last = [&end](const std::vector<SceneAnimationKey>& keys) {
        if (! keys.empty()) end = std::max(end, keys.back().frame);
    };
    absorb_last(curve.c0);
    absorb_last(curve.c1);
    absorb_last(curve.c2);
    return static_cast<float>(end);
}

float animation_frame(const SceneAnimationCurve& curve, double runtime) {
    float fps   = curve.fps > 0.0f ? curve.fps : 30.0f;
    float frame = static_cast<float>(runtime) * fps;
    float end   = curve_end_frame(curve);
    if (end <= 0.0f) return frame;

    const float end_frame = end;
    bool        loop = curve.wraploop || curve.mode == "loop" || curve.mode == "repeat";
    if (loop) {
        frame = std::fmod(frame, end_frame);
        if (frame < 0.0f) frame += end_frame;
        return frame;
    }
    if (curve.mode == "mirror") {
        const float period = 2.0f * end_frame;
        float       folded = std::fmod(frame, period);
        if (folded < 0.0f) folded += period;
        return folded <= end_frame ? folded : (period - folded);
    }
    return std::clamp(frame, 0.0f, end_frame);
}

float eval_segment(const SceneAnimationKey& a, const SceneAnimationKey& b, float frame) {
    float dt = static_cast<float>(b.frame - a.frame);
    if (dt <= 0.0f) return b.value;
    float linear_t = std::clamp((frame - static_cast<float>(a.frame)) / dt, 0.0f, 1.0f);
    bool  has_tan  = a.front_enabled || b.back_enabled;
    if (! has_tan) return std::lerp(a.value, b.value, linear_t);

    float p0x = static_cast<float>(a.frame);
    float p3x = static_cast<float>(b.frame);
    float p1x = a.front_enabled ? p0x + a.front_x : p0x + dt / 3.0f;
    float p2x = b.back_enabled ? p3x + b.back_x : p3x - dt / 3.0f;
    float p0y = a.value;
    float p3y = b.value;
    float p1y = a.front_enabled ? p0y + a.front_y : std::lerp(p0y, p3y, 1.0f / 3.0f);
    float p2y = b.back_enabled ? p3y + b.back_y : std::lerp(p0y, p3y, 2.0f / 3.0f);

    if (! (p0x <= p1x && p1x <= p2x && p2x <= p3x)) return std::lerp(a.value, b.value, linear_t);

    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 16; ++i) {
        float mid = (lo + hi) * 0.5f;
        if (cubic(p0x, p1x, p2x, p3x, mid) < frame)
            lo = mid;
        else
            hi = mid;
    }
    return cubic(p0y, p1y, p2y, p3y, (lo + hi) * 0.5f);
}

float eval_axis(const std::vector<SceneAnimationKey>& keys, float frame, bool loop,
                float end_frame) {
    if (keys.empty()) return 0.0f;
    if (frame <= static_cast<float>(keys.front().frame)) return keys.front().value;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (frame <= static_cast<float>(keys[i].frame))
            return eval_segment(keys[i - 1], keys[i], frame);
    }
    // Past the last keyframe. For a looping curve whose length extends beyond
    // the last key (WE "wraploop"), interpolate the last key back to the first
    // across [last.frame, end_frame] so the wrap is continuous instead of
    // holding the last value and snapping at the loop boundary. Uses the last
    // key's out-tangent and the first key's in-tangent (with wrapped frame
    // coordinates) so cubic eases match a normal segment.
    const auto& last = keys.back();
    if (loop && end_frame > static_cast<float>(last.frame)) {
        SceneAnimationKey wrapped_first = keys.front();
        wrapped_first.frame             = static_cast<std::int32_t>(std::ceil(end_frame));
        if (wrapped_first.frame > last.frame) {
            return eval_segment(last, wrapped_first, frame);
        }
    }
    return last.value;
}

Eigen::Vector3f lerp_vec3(const Eigen::Vector3f& a, const Eigen::Vector3f& b, float t) {
    return a + (b - a) * t;
}

SceneCameraLookAtKey eval_lookat_track(const SceneCameraLookAtTrack& track, float frame) {
    if (track.keys.empty()) return {};
    if (frame <= track.keys.front().frame) return track.keys.front();
    for (std::size_t i = 1; i < track.keys.size(); ++i) {
        const auto& a = track.keys[i - 1];
        const auto& b = track.keys[i];
        if (frame > b.frame) continue;
        float dt = b.frame - a.frame;
        float t  = dt > 0.0f ? std::clamp((frame - a.frame) / dt, 0.0f, 1.0f) : 1.0f;
        return {
            .frame  = frame,
            .eye    = lerp_vec3(a.eye, b.eye, t),
            .center = lerp_vec3(a.center, b.center, t),
            .up     = lerp_vec3(a.up, b.up, t).normalized(),
        };
    }
    return track.keys.back();
}

std::optional<SceneCameraLookAtKey>
eval_lookat_tracks(std::span<const SceneCameraLookAtTrack> tracks, double runtime, float fps) {
    float total = 0.0f;
    for (const auto& track : tracks) total += std::max(track.duration, 0.0f);
    if (total <= 0.0f) return std::nullopt;

    float frame = static_cast<float>(runtime) * (fps > 0.0f ? fps : 1.0f);
    frame       = std::fmod(frame, total);
    if (frame < 0.0f) frame += total;

    float offset = 0.0f;
    for (const auto& track : tracks) {
        float duration = std::max(track.duration, 0.0f);
        if (duration <= 0.0f) continue;
        if (frame <= offset + duration) return eval_lookat_track(track, frame - offset);
        offset += duration;
    }
    return eval_lookat_track(tracks.back(), tracks.back().duration);
}

bool shader_values_equal(const ShaderValue& a, const ShaderValue& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

ShaderValue eval_shader_value_animation(const SceneShaderValueAnimation& animation,
                                        double                           runtime) {
    if (! animation.curve || animation.curve->Empty() || animation.base.size() == 0)
        return animation.base;

    std::vector<float> value(animation.base.size());
    for (std::size_t i = 0; i < animation.base.size(); ++i) value[i] = animation.base[i];

    if (value.size() == 1) {
        value[0] = animation.curve->EvaluateScalar(value[0], runtime);
        return ShaderValue(std::span<const float>(value));
    }

    Eigen::Vector3f base { value[0],
                           value.size() > 1 ? value[1] : 0.0f,
                           value.size() > 2 ? value[2] : 0.0f };
    auto            animated = animation.curve->EvaluateVec3(base, runtime);
    value[0]                 = animated.x();
    if (value.size() > 1) value[1] = animated.y();
    if (value.size() > 2) value[2] = animated.z();
    return ShaderValue(std::span<const float>(value));
}

void collect_linked_ids_from_material(const SceneMaterial& material, Set<i32>& out) {
    for (const auto& texture : material.textures) {
        if (IsSpecLinkTex(texture)) out.insert(static_cast<i32>(ParseLinkTex(texture)));
    }
}

void collect_linked_ids_from_node(SceneNode* node, Scene& scene, Set<i32>& out) {
    if (node == nullptr) return;
    if (node->HasMaterial()) collect_linked_ids_from_material(*node->Mesh()->Material(), out);
    if (! node->Camera().empty()) {
        auto it = scene.cameras.find(node->Camera());
        if (it != scene.cameras.end() && it->second->HasImgEffect()) {
            auto& effect_layer = it->second->GetImgEffect();
            for (usize i = 0; i < effect_layer->EffectCount(); i++) {
                auto& effect = effect_layer->GetEffect(i);
                for (auto& effect_node : effect->nodes) {
                    if (effect_node.sceneNode->HasMaterial()) {
                        collect_linked_ids_from_material(*effect_node.sceneNode->Mesh()->Material(),
                                                         out);
                    }
                }
            }
        }
    }
    for (auto& child : node->GetChildren())
        collect_linked_ids_from_node(child.as_ptr(), scene, out);
}

void collect_linked_ids_from_scene(Scene& scene, Set<i32>& out) {
    collect_linked_ids_from_node(scene.sceneGraph.as_ptr(), scene, out);
    for (auto& pp : scene.post_processes) {
        for (auto& step : pp->steps) {
            if (auto* pass = std::get_if<ScenePostProcessPass>(&step)) {
                collect_linked_ids_from_node(pass->node.as_ptr(), scene, out);
            }
        }
    }
}

SceneNode* find_layer_node(SceneNode* node, Scene& scene, i32 id) {
    if (node == nullptr) return nullptr;
    if (node->ID() == id) return node;

    if (! node->Camera().empty()) {
        auto it = scene.cameras.find(node->Camera());
        if (it != scene.cameras.end() && it->second->HasImgEffect()) {
            auto& effect_layer = it->second->GetImgEffect();
            for (usize i = 0; i < effect_layer->EffectCount(); i++) {
                auto& effect = effect_layer->GetEffect(i);
                for (auto& effect_node : effect->nodes) {
                    if (auto* found = find_layer_node(effect_node.sceneNode.as_ptr(), scene, id))
                        return found;
                }
            }
        }
    }

    for (auto& child : node->GetChildren()) {
        if (auto* found = find_layer_node(child.as_ptr(), scene, id)) return found;
    }
    return nullptr;
}

SceneNode* find_layer_node(Scene& scene, WallpaperLayerId id) {
    if (auto* found = find_layer_node(scene.sceneGraph.as_ptr(), scene, id.value)) return found;
    for (auto& pp : scene.post_processes) {
        if (! pp) continue;
        for (auto& step : pp->steps) {
            if (auto* pass = std::get_if<ScenePostProcessPass>(&step)) {
                if (auto* found = find_layer_node(pass->node.as_ptr(), scene, id.value))
                    return found;
            }
        }
    }
    return nullptr;
}

void ensure_snapshot_link_render_targets(Scene& scene, const Set<i32>& linked_ids) {
    for (auto id : linked_ids) {
        if (scene.elidable_layer_ids.count(id) == 0) continue;
        auto layer = WallpaperLayerId { .value = id };
        auto key   = GenLinkTex(static_cast<std::ptrdiff_t>(id));
        if (scene.renderTargets.contains(key)) continue;
        if (auto* source = find_layer_node(scene, layer))
            scene.EnsureLinkRenderTarget(layer, *source);
    }
}
} // namespace

void SceneResourceIndex::Rebuild(Scene& scene, uint32_t generation) {
    m_scene      = &scene;
    m_generation = generation;

    m_nodes.clear();
    m_meshes.clear();
    m_materials.clear();
    m_texture_keys.clear();
    m_render_target_keys.clear();
    m_camera_keys.clear();
    m_draw_items.clear();
    m_node_ids.clear();
    m_mesh_ids.clear();
    m_material_ids.clear();
    m_texture_ids.clear();
    m_render_target_ids.clear();
    m_camera_ids.clear();

    auto register_mesh = [this, generation](SceneMesh& mesh) {
        if (auto it = m_mesh_ids.find(&mesh); it != m_mesh_ids.end()) return it->second;
        SceneMeshId id { .index = index_from_size(m_meshes.size()), .generation = generation };
        m_meshes.push_back(&mesh);
        m_mesh_ids.emplace(&mesh, id);
        return id;
    };

    auto register_material = [this, generation](SceneMaterial& material) {
        if (auto it = m_material_ids.find(&material); it != m_material_ids.end()) return it->second;
        SceneMaterialId id { .index      = index_from_size(m_materials.size()),
                             .generation = generation };
        m_materials.push_back(&material);
        m_material_ids.emplace(&material, id);
        return id;
    };

    auto register_draw_items = [&](SceneNode& node, SceneNodeId node_id) {
        auto* mesh = node.Mesh();
        if (mesh == nullptr) return;

        SceneMeshId mesh_id = register_mesh(*mesh);
        const auto& slots   = mesh->MaterialSlots();
        const auto& parts   = mesh->Submeshes();
        for (std::size_t smi = 0; smi < parts.size(); ++smi) {
            const auto& submesh    = parts[smi];
            auto        slot_index = static_cast<std::size_t>(submesh.material_slot);
            if (slot_index >= slots.size() || ! slots[slot_index]) continue;

            SceneMaterialId material_id = register_material(*slots[slot_index]);
            SceneDrawItemId draw_id { .index      = index_from_size(m_draw_items.size()),
                                      .generation = generation };
            m_draw_items.push_back(
                SceneDrawItemRecord { .id            = draw_id,
                                      .node          = node_id,
                                      .mesh          = mesh_id,
                                      .material      = material_id,
                                      .submesh_index = static_cast<uint32_t>(smi) });
        }
    };

    auto register_node = [&](SceneNode& node) {
        if (auto it = m_node_ids.find(&node); it != m_node_ids.end()) return it->second;
        SceneNodeId id { .index = index_from_size(m_nodes.size()), .generation = generation };
        m_nodes.push_back(&node);
        m_node_ids.emplace(&node, id);
        register_draw_items(node, id);
        return id;
    };

    auto collect_node = [&](auto& self, SceneNode* node) -> void {
        if (node == nullptr) return;
        if (m_node_ids.count(node) != 0) return;

        register_node(*node);

        if (! node->Camera().empty()) {
            auto cam_it = scene.cameras.find(node->Camera());
            if (cam_it != scene.cameras.end() && cam_it->second->HasImgEffect()) {
                auto& eff_layer = cam_it->second->GetImgEffect();
                for (std::size_t ei = 0; ei < eff_layer->EffectCount(); ++ei) {
                    auto& eff = eff_layer->GetEffect(ei);
                    for (auto& eff_node : eff->nodes) self(self, eff_node.sceneNode.as_ptr());
                }
            }
        }

        for (auto& child : node->GetChildren()) self(self, child.as_ptr());
    };

    collect_node(collect_node, scene.sceneGraph.as_ptr());
    for (auto& pp : scene.post_processes) {
        if (! pp) continue;
        for (auto& step : pp->steps) {
            if (auto* pass = std::get_if<ScenePostProcessPass>(&step)) {
                collect_node(collect_node, pass->node.as_ptr());
            }
        }
    }

    auto collect_texture_ids = [generation, this]() {
        m_texture_keys.reserve(m_scene->textures.size());
        for (const auto& [key, _] : m_scene->textures) m_texture_keys.push_back(key);
        std::sort(m_texture_keys.begin(), m_texture_keys.end());
        for (std::size_t i = 0; i < m_texture_keys.size(); ++i) {
            m_texture_ids.emplace(
                m_texture_keys[i],
                SceneTextureId { .index = index_from_size(i), .generation = generation });
        }
    };

    auto collect_render_target_ids = [generation, this]() {
        m_render_target_keys.reserve(m_scene->renderTargets.size());
        for (const auto& [key, _] : m_scene->renderTargets) m_render_target_keys.push_back(key);
        std::sort(m_render_target_keys.begin(), m_render_target_keys.end());
        for (std::size_t i = 0; i < m_render_target_keys.size(); ++i) {
            m_render_target_ids.emplace(
                m_render_target_keys[i],
                SceneRenderTargetId { .index = index_from_size(i), .generation = generation });
        }
    };

    auto collect_camera_ids = [generation, this]() {
        m_camera_keys.reserve(m_scene->cameras.size());
        for (const auto& [key, _] : m_scene->cameras) m_camera_keys.push_back(key);
        std::sort(m_camera_keys.begin(), m_camera_keys.end());
        for (std::size_t i = 0; i < m_camera_keys.size(); ++i) {
            m_camera_ids.emplace(
                m_camera_keys[i],
                SceneCameraId { .index = index_from_size(i), .generation = generation });
        }
    };

    collect_texture_ids();
    collect_render_target_ids();
    collect_camera_ids();
}

std::optional<SceneNodeId> SceneResourceIndex::nodeId(const SceneNode& node) const {
    auto it = m_node_ids.find(&node);
    if (it == m_node_ids.end()) return std::nullopt;
    return it->second;
}

std::optional<SceneMeshId> SceneResourceIndex::meshId(const SceneMesh& mesh) const {
    auto it = m_mesh_ids.find(&mesh);
    if (it == m_mesh_ids.end()) return std::nullopt;
    return it->second;
}

std::optional<SceneMaterialId> SceneResourceIndex::materialId(const SceneMaterial& material) const {
    auto it = m_material_ids.find(&material);
    if (it == m_material_ids.end()) return std::nullopt;
    return it->second;
}

std::optional<SceneDrawItemId> SceneResourceIndex::drawItemFor(SceneNodeId node,
                                                               uint32_t    submesh_index) const {
    if (node.generation != m_generation) return std::nullopt;
    for (const auto& item : m_draw_items) {
        if (item.node.index == node.index && item.submesh_index == submesh_index) return item.id;
    }
    return std::nullopt;
}

std::optional<SceneTextureId> SceneResourceIndex::textureId(std::string_view url) const {
    auto it = m_texture_ids.find(std::string(url));
    if (it == m_texture_ids.end()) return std::nullopt;
    return it->second;
}

std::optional<SceneRenderTargetId> SceneResourceIndex::renderTargetId(std::string_view key) const {
    auto it = m_render_target_ids.find(std::string(key));
    if (it == m_render_target_ids.end()) return std::nullopt;
    return it->second;
}

std::optional<SceneCameraId> SceneResourceIndex::cameraId(std::string_view name) const {
    auto it = m_camera_ids.find(std::string(name));
    if (it == m_camera_ids.end()) return std::nullopt;
    return it->second;
}

std::optional<DrawItemView> SceneResourceIndex::resolve(SceneDrawItemId id) const {
    if (! valid_index(id, m_generation, m_draw_items.size())) return std::nullopt;
    const auto& item = m_draw_items[id.index];
    auto*       n    = node(item.node);
    auto*       me   = mesh(item.mesh);
    auto*       ma   = material(item.material);
    if (n == nullptr || me == nullptr || ma == nullptr) return std::nullopt;
    if (static_cast<std::size_t>(item.submesh_index) >= me->Submeshes().size()) return std::nullopt;
    return DrawItemView { .node          = n,
                          .mesh          = me,
                          .submesh       = &me->Submeshes()[item.submesh_index],
                          .material      = ma,
                          .submesh_index = item.submesh_index };
}

SceneNode* SceneResourceIndex::node(SceneNodeId id) const {
    if (! valid_index(id, m_generation, m_nodes.size())) return nullptr;
    return m_nodes[id.index];
}

SceneMesh* SceneResourceIndex::mesh(SceneMeshId id) const {
    if (! valid_index(id, m_generation, m_meshes.size())) return nullptr;
    return m_meshes[id.index];
}

SceneMaterial* SceneResourceIndex::material(SceneMaterialId id) const {
    if (! valid_index(id, m_generation, m_materials.size())) return nullptr;
    return m_materials[id.index];
}

const SceneTexture* SceneResourceIndex::texture(SceneTextureId id) const {
    if (m_scene == nullptr || ! valid_index(id, m_generation, m_texture_keys.size()))
        return nullptr;
    auto it = m_scene->textures.find(m_texture_keys[id.index]);
    if (it == m_scene->textures.end()) return nullptr;
    return &it->second;
}

const SceneRenderTarget* SceneResourceIndex::renderTarget(SceneRenderTargetId id) const {
    if (m_scene == nullptr || ! valid_index(id, m_generation, m_render_target_keys.size()))
        return nullptr;
    auto it = m_scene->renderTargets.find(m_render_target_keys[id.index]);
    if (it == m_scene->renderTargets.end()) return nullptr;
    return &it->second;
}

SceneRenderTarget* SceneResourceIndex::mutableRenderTarget(SceneRenderTargetId id) const {
    if (m_scene == nullptr || ! valid_index(id, m_generation, m_render_target_keys.size()))
        return nullptr;
    auto it = m_scene->renderTargets.find(m_render_target_keys[id.index]);
    if (it == m_scene->renderTargets.end()) return nullptr;
    return &it->second;
}

SceneCamera* SceneResourceIndex::camera(SceneCameraId id) const {
    if (m_scene == nullptr || ! valid_index(id, m_generation, m_camera_keys.size())) return nullptr;
    auto it = m_scene->cameras.find(m_camera_keys[id.index]);
    if (it == m_scene->cameras.end()) return nullptr;
    return it->second.get();
}

void RenderSceneSnapshot::Rebuild(Scene& scene, RenderSceneVersion version) {
    m_version = version;

    m_render_items.clear();
    m_texture_descs.clear();
    m_render_target_descs.clear();
    m_render_item_ids.clear();
    m_texture_desc_ids.clear();
    m_render_target_desc_ids.clear();
    m_source_layer_items.clear();
    m_material_render_items.clear();
    m_mesh_render_items.clear();
    m_link_sources.clear();
    m_link_source_ids.clear();
    m_linked_layer_ids.clear();

    collect_linked_ids_from_scene(scene, m_linked_layer_ids);
    ensure_snapshot_link_render_targets(scene, m_linked_layer_ids);
    scene.RebuildResourceIndex();

    const auto& index = scene.ResourceIndex();

    std::vector<std::string> texture_keys;
    texture_keys.reserve(scene.textures.size());
    for (const auto& [key, _] : scene.textures) texture_keys.push_back(key);
    std::sort(texture_keys.begin(), texture_keys.end());

    for (const auto& key : texture_keys) {
        auto id       = RenderTextureDescId { .index      = index_from_size(m_texture_descs.size()),
                                              .generation = version.value };
        auto scene_id = index.textureId(key).value_or(SceneTextureId {});
        auto desc_it  = scene.textures.find(key);
        m_texture_desc_ids.emplace(key, id);
        m_texture_descs.push_back(RenderTextureDescRecord {
            .id            = id,
            .scene_texture = scene_id,
            .key           = key,
            .desc          = desc_it != scene.textures.end() ? desc_it->second : SceneTexture {},
        });
    }

    std::vector<std::string> render_target_keys;
    render_target_keys.reserve(scene.renderTargets.size());
    for (const auto& [key, _] : scene.renderTargets) render_target_keys.push_back(key);
    std::sort(render_target_keys.begin(), render_target_keys.end());

    for (const auto& key : render_target_keys) {
        auto id       = RenderTargetDescId { .index      = index_from_size(m_render_target_descs.size()),
                                             .generation = version.value };
        auto scene_id = index.renderTargetId(key).value_or(SceneRenderTargetId {});
        auto desc_it  = scene.renderTargets.find(key);
        m_render_target_desc_ids.emplace(key, id);
        m_render_target_descs.push_back(RenderTargetDescRecord {
            .id                  = id,
            .scene_render_target = scene_id,
            .key                 = key,
            .desc = desc_it != scene.renderTargets.end() ? desc_it->second : SceneRenderTarget {},
        });
    }

    for (const auto& item : index.DrawItems()) {
        auto        id   = RenderItemId { .index      = index_from_size(m_render_items.size()),
                                          .generation = version.value };
        const auto* node = index.node(item.node);
        std::optional<RenderTargetDescId> output_override;
        if (auto view = index.resolve(item.id)) {
            if (view->submesh != nullptr && ! view->submesh->output_override.empty()) {
                output_override = renderTargetDescId(view->submesh->output_override);
            }
        }

        auto source_layer = WallpaperLayerId { .value = node != nullptr ? node->ID() : -1 };
        m_render_item_ids.emplace(scene_id_key(item.id), id);
        m_source_layer_items[source_layer.value].push_back(id);
        m_material_render_items[scene_id_key(item.material)].push_back(id);
        m_mesh_render_items[scene_id_key(item.mesh)].push_back(id);
        m_render_items.push_back(RenderItemRecord { .id              = id,
                                                    .scene_draw_item = item.id,
                                                    .scene_node      = item.node,
                                                    .scene_mesh      = item.mesh,
                                                    .scene_material  = item.material,
                                                    .source_layer    = source_layer,
                                                    .submesh_index   = item.submesh_index,
                                                    .output_override = output_override });
    }

    for (auto id : m_linked_layer_ids) {
        auto key     = GenLinkTex(static_cast<std::ptrdiff_t>(id));
        auto desc_id = renderTargetDescId(key);
        if (! desc_id) continue;

        auto record_index = index_from_size(m_link_sources.size());
        m_link_source_ids.emplace(id, record_index);
        m_link_sources.push_back(RenderLinkSourceRecord {
            .source_layer      = WallpaperLayerId { .value = id },
            .render_target_key = std::move(key),
            .render_target     = *desc_id,
        });
    }
}

const RenderItemRecord* RenderSceneSnapshot::renderItem(RenderItemId id) const {
    if (! valid_render_index(id, m_version.value, m_render_items.size())) return nullptr;
    return &m_render_items[id.index];
}

const RenderTextureDescRecord* RenderSceneSnapshot::textureDesc(RenderTextureDescId id) const {
    if (! valid_render_index(id, m_version.value, m_texture_descs.size())) return nullptr;
    return &m_texture_descs[id.index];
}

const RenderTargetDescRecord* RenderSceneSnapshot::renderTargetDesc(RenderTargetDescId id) const {
    if (! valid_render_index(id, m_version.value, m_render_target_descs.size())) return nullptr;
    return &m_render_target_descs[id.index];
}

std::optional<RenderItemId> RenderSceneSnapshot::renderItemFor(SceneDrawItemId id) const {
    auto it = m_render_item_ids.find(scene_id_key(id));
    if (it == m_render_item_ids.end()) return std::nullopt;
    return it->second;
}

std::optional<RenderTextureDescId> RenderSceneSnapshot::textureDescId(std::string_view key) const {
    auto it = m_texture_desc_ids.find(std::string(key));
    if (it == m_texture_desc_ids.end()) return std::nullopt;
    return it->second;
}

std::optional<RenderTargetDescId>
RenderSceneSnapshot::renderTargetDescId(std::string_view key) const {
    auto it = m_render_target_desc_ids.find(std::string(key));
    if (it == m_render_target_desc_ids.end()) return std::nullopt;
    return it->second;
}

std::span<const RenderItemId> RenderSceneSnapshot::renderItemsFor(WallpaperLayerId id) const {
    auto it = m_source_layer_items.find(id.value);
    if (it == m_source_layer_items.end()) return {};
    return { it->second.data(), it->second.size() };
}

std::span<const RenderItemId> RenderSceneSnapshot::renderItemsFor(SceneMaterialId id) const {
    auto it = m_material_render_items.find(scene_id_key(id));
    if (it == m_material_render_items.end()) return {};
    return { it->second.data(), it->second.size() };
}

std::span<const RenderItemId> RenderSceneSnapshot::renderItemsFor(SceneMeshId id) const {
    auto it = m_mesh_render_items.find(scene_id_key(id));
    if (it == m_mesh_render_items.end()) return {};
    return { it->second.data(), it->second.size() };
}

const RenderLinkSourceRecord* RenderSceneSnapshot::linkSource(WallpaperLayerId id) const {
    auto it = m_link_source_ids.find(id.value);
    if (it == m_link_source_ids.end()) return nullptr;
    return &m_link_sources[it->second];
}

bool RenderSceneSnapshot::HasLinkConsumer(WallpaperLayerId id) const {
    return m_linked_layer_ids.count(id.value) != 0;
}

RenderSceneSnapshot ExtractRenderSceneSnapshot(Scene& scene) {
    RenderSceneSnapshot snapshot;
    snapshot.Rebuild(scene, RenderSceneVersion { .value = next_render_scene_version() });
    return snapshot;
}

bool SceneAnimationCurve::Empty() const { return c0.empty() && c1.empty() && c2.empty(); }

float SceneAnimationCurve::EvaluateScalar(float base, double runtime) const {
    if (c0.empty()) return base;
    const float end   = curve_end_frame(*this);
    float       value = eval_axis(c0, animation_frame(*this, runtime), wraploop, end);
    return relative ? base + value : value;
}

Eigen::Vector3f SceneAnimationCurve::EvaluateVec3(const Eigen::Vector3f& base,
                                                  double                 runtime) const {
    if (Empty()) return base;
    const float     end   = curve_end_frame(*this);
    float           frame = animation_frame(*this, runtime);
    Eigen::Vector3f value = base;
    if (! c0.empty())
        value.x() = relative ? base.x() + eval_axis(c0, frame, wraploop, end)
                             : eval_axis(c0, frame, wraploop, end);
    if (! c1.empty())
        value.y() = relative ? base.y() + eval_axis(c1, frame, wraploop, end)
                             : eval_axis(c1, frame, wraploop, end);
    if (! c2.empty())
        value.z() = relative ? base.z() + eval_axis(c2, frame, wraploop, end)
                             : eval_axis(c2, frame, wraploop, end);
    return value;
}

void SceneNode::TickFieldAnimations(double runtime) {
    if (m_origin_curve) SetTranslate(m_origin_curve->EvaluateVec3(m_origin_base, runtime));
    if (m_scale_curve) SetScale(m_scale_curve->EvaluateVec3(m_scale_base, runtime));
    if (m_rotation_curve) SetRotation(m_rotation_curve->EvaluateVec3(m_rotation_base, runtime));
    if (m_alpha_curve) SetUserAlpha(m_alpha_curve->EvaluateScalar(m_base_alpha, runtime));
}

void SceneCameraPath::CaptureViewport() {
    if (! camera) return;
    default_width  = camera->Width();
    default_height = camera->Height();
    default_fov    = camera->Fov();
}

bool SceneCameraPath::ApplyDefault() {
    if (! camera) return false;
    if (default_lookat) {
        camera->SetLookAt(
            default_eye.cast<double>(), default_center.cast<double>(), default_up.cast<double>());
    }
    if (node) {
        node->SetTranslate(default_translate);
        node->SetRotation(default_rotation);
    }
    camera->SetWidth(default_width);
    camera->SetHeight(default_height);
    if (default_fov > 0.0) camera->SetFov(default_fov);
    camera->Update();
    return true;
}

bool SceneCameraPath::Tick(double runtime) {
    if (! camera) return false;
    if (! enabled) return ApplyDefault();

    if (! lookat_tracks.empty()) {
        auto key = eval_lookat_tracks(lookat_tracks, runtime, lookat_fps);
        if (! key) return false;
        camera->SetLookAt(
            key->eye.cast<double>(), key->center.cast<double>(), key->up.cast<double>());
        if (fov_base > 0.0f) camera->SetFov(fov_base);
        camera->Update();
        return true;
    }

    if (! node) return false;
    camera->AttatchNode(node);

    node->SetTranslate(path_translate_bias + origin_curve.EvaluateVec3(origin_base, runtime));
    node->SetRotation(path_rotation_bias + rotation_curve.EvaluateVec3(rotation_base, runtime));

    if (perspective) {
        float fov = fov_curve.EvaluateScalar(fov_base, runtime);
        if (fov > 0.0f) camera->SetFov(fov);
    } else {
        float zoom = zoom_curve.EvaluateScalar(zoom_base, runtime);
        zoom       = std::max(zoom, 0.001f);
        camera->SetWidth(default_width / static_cast<double>(zoom));
        camera->SetHeight(default_height / static_cast<double>(zoom));
    }
    camera->Update();
    return true;
}

Scene::Scene()
    : sceneGraph(rstd::sync::Arc<SceneNode>::make()),
      vfs(nullptr, &delete_vfs),
      paritileSys(std::make_unique<ParticleSystem>(*this)),
      m_resource_generation(next_scene_resource_generation()) {}
Scene::~Scene() = default;

bool SceneMaterial::SetShaderValueAnimation(std::string                          uniform_name,
                                            std::shared_ptr<SceneAnimationCurve> curve) {
    if (uniform_name.empty() || ! curve || curve->Empty()) return false;

    ShaderValue base;
    if (auto it = customShader.constValues.find(uniform_name);
        it != customShader.constValues.end()) {
        base = it->second;
    } else if (customShader.shader) {
        if (auto it = customShader.shader->default_uniforms.find(uniform_name);
            it != customShader.shader->default_uniforms.end()) {
            base = ShapeShaderValue(uniform_name, it->second);
        }
    }
    if (base.size() == 0) return false;

    customShader.valueAnimations[std::move(uniform_name)] =
        SceneShaderValueAnimation { .base = base, .curve = std::move(curve) };
    return true;
}

bool SceneMaterial::TickShaderValueAnimations(double runtime) {
    bool changed = false;
    for (auto& [uniform_name, animation] : customShader.valueAnimations) {
        ShaderValue value = eval_shader_value_animation(animation, runtime);
        if (auto it = customShader.constValues.find(uniform_name);
            it != customShader.constValues.end() && shader_values_equal(it->second, value)) {
            continue;
        }
        customShader.constValues[uniform_name] = std::move(value);
        changed                                = true;
    }
    if (changed) customShader.dirty = true;
    return changed;
}

void Scene::RebuildResourceIndex() { m_resource_index.Rebuild(*this, m_resource_generation); }

bool Scene::EnsureTextureDescriptor(std::string_view key) {
    if (key.empty() || IsSpecTex(key)) return true;
    std::string name(key);
    if (textures.contains(name)) return true;
    if (! imageParser) return false;

    const auto   header = imageParser->ParseHeader(name);
    SceneTexture texture;
    texture.url    = name;
    texture.sample = header.sample;
    if (header.isSprite) {
        texture.isSprite   = true;
        texture.spriteAnim = header.spriteAnim;
    }
    textures.emplace(std::move(name), std::move(texture));
    return true;
}

bool Scene::SetMaterialShaderValue(SceneMaterial& material, std::string_view uniform_name,
                                   const ShaderValue& value) {
    return material.SetShaderValue(std::string(uniform_name), value);
}

SceneMaterialTextureSlotMutation
Scene::SetMaterialTextureSlot(SceneMaterial& material, uint32_t slot, std::string_view texture) {
    if (! EnsureTextureDescriptor(texture)) return {};

    auto slot_index = static_cast<std::size_t>(slot);
    if (material.textures.size() <= slot_index) material.textures.resize(slot_index + 1);
    auto& current = material.textures[slot_index];
    if (current == texture) return {};

    current = std::string(texture);
    if (m_resource_index.Empty()) RebuildResourceIndex();
    return SceneMaterialTextureSlotMutation {
        .changed  = true,
        .material = m_resource_index.materialId(material),
    };
}

SceneMaterialShaderVariantMutation
Scene::SetMaterialShaderVariant(SceneMaterial& material, SceneShaderVariantMutation mutation) {
    if (! material.SetShaderVariant(std::move(mutation.shader), std::move(mutation.variant)))
        return {};

    if (m_resource_index.Empty()) RebuildResourceIndex();
    return SceneMaterialShaderVariantMutation {
        .changed  = true,
        .material = m_resource_index.materialId(material),
    };
}

void Scene::ClearUserPropertyDiagnostics(std::string_view key) {
    if (key.empty()) {
        m_user_property_diagnostics.clear();
        return;
    }
    for (auto it = m_user_property_diagnostics.begin(); it != m_user_property_diagnostics.end();) {
        if (it->key == key) {
            it = m_user_property_diagnostics.erase(it);
        } else {
            ++it;
        }
    }
}

void Scene::AddUserPropertyDiagnostic(SceneUserPropertyDiagnostic diagnostic) {
    m_user_property_diagnostics.push_back(std::move(diagnostic));
}

void Scene::RebuildElidableLayerIds() {
    elidable_layer_ids.clear();
    elidable_layer_ids.insert(static_elidable_layer_ids.begin(), static_elidable_layer_ids.end());
    elidable_layer_ids.insert(visibility_elidable_layer_ids.begin(),
                              visibility_elidable_layer_ids.end());
}

void Scene::MarkLayerStaticElidable(WallpaperLayerId id) {
    static_elidable_layer_ids.insert(id.value);
    elidable_layer_ids.insert(id.value);
}

void Scene::MarkLayerVisibilityElidable(WallpaperLayerId id) {
    visibility_elidable_layer_ids.insert(id.value);
    elidable_layer_ids.insert(id.value);
}

bool Scene::SetNodeVisible(SceneNode& node, bool visible) {
    if (node.Visible() == visible) return false;

    const i32 id = node.ID();
    node.SetVisible(visible);
    if (id < 0) return false;

    // Do not mutate visibility_elidable_layer_ids here. A script may set the
    // same layer false and true before the frame is drawn; only its final
    // state determines whether the compiled render graph needs to change.
    m_pending_node_visibility_changes[id] = &node;
    return true;
}

bool Scene::CommitNodeVisibilityChanges() {
    bool requires_graph_rebuild = false;
    for (const auto& [id, node] : m_pending_node_visibility_changes) {
        if (node == nullptr) continue;

        const bool was_elidable = elidable_layer_ids.count(id) != 0;
        if (! node->Visible()) {
            MarkLayerVisibilityElidable(WallpaperLayerId { .value = id });
        } else if (visibility_elidable_layer_ids.erase(id) != 0) {
            RebuildElidableLayerIds();
        }

        const bool is_elidable = elidable_layer_ids.count(id) != 0;
        requires_graph_rebuild |= was_elidable != is_elidable;
    }
    m_pending_node_visibility_changes.clear();

    if (requires_graph_rebuild) m_render_graph_dirty = true;
    return requires_graph_rebuild;
}

bool Scene::ApplyUserNodeVisibilityBindings(std::string_view key, const Json& property) {
    if (m_resource_index.Empty()) RebuildResourceIndex();
    bool matched_binding = false;
    for (auto* node : m_resource_index.Nodes()) {
        if (node == nullptr) continue;
        if (auto visible =
                ResolveSceneUserVisibilityBinding(node->VisibleUserBinding(), key, property)) {
            SetNodeVisible(*node, *visible);
            matched_binding = true;
        }
    }
    bool requires_graph_rebuild = CommitNodeVisibilityChanges();
    // A user toggle of a layer's visibility must always recompile the graph:
    // at load the node's Visible() bool can already match the toggled value
    // while the compiled graph still elided it, so the elision-delta check
    // above misses it and the first toggle would no-op until the next edit.
    // This path is user-driven only (never the per-frame script tick), so an
    // unconditional rebuild on a real binding match is cheap and correct.
    if (matched_binding) requires_graph_rebuild = true;
    return requires_graph_rebuild;
}

std::optional<SceneImageEffectRef> Scene::FindNodeImageEffect(const SceneNode& node,
                                                              std::string_view name) {
    if (node.Camera().empty()) return std::nullopt;
    auto camera_it = cameras.find(node.Camera());
    if (camera_it == cameras.end() || ! camera_it->second->HasImgEffect()) return std::nullopt;

    auto& effect_layer = camera_it->second->GetImgEffect();
    if (! effect_layer) return std::nullopt;
    auto effect = effect_layer->FindEffect(name);
    if (! effect) return std::nullopt;
    return SceneImageEffectRef { .layer = effect_layer.get(), .effect = std::move(effect) };
}

bool Scene::SetImageEffectRuntimeVisible(const SceneImageEffectRef& ref, bool visible) {
    if (! ref.layer || ! ref.effect) return false;
    if (! ref.layer->SetEffectRuntimeVisible(*ref.effect, visible)) return false;
    m_render_graph_dirty = true;
    return true;
}

bool Scene::ApplyUserImageEffectVisibilityBindings(std::string_view key, const Json& property) {
    if (m_resource_index.Empty()) RebuildResourceIndex();

    bool                                  requires_graph_rebuild = false;
    std::unordered_set<SceneImageEffect*> visited;
    for (auto* node : m_resource_index.Nodes()) {
        if (node == nullptr || node->Camera().empty()) continue;
        auto camera_it = cameras.find(node->Camera());
        if (camera_it == cameras.end() || ! camera_it->second->HasImgEffect()) continue;
        auto& effect_layer = camera_it->second->GetImgEffect();
        for (usize i = 0; i < effect_layer->EffectCount(); ++i) {
            auto& effect = effect_layer->GetEffect(i);
            if (! effect || ! visited.insert(effect.get()).second) continue;
            auto visible =
                ResolveSceneUserVisibilityBinding(effect->visible_user_binding, key, property);
            if (! visible) continue;
            if (SetImageEffectRuntimeVisible({ .layer = effect_layer.get(), .effect = effect },
                                             *visible)) {
                requires_graph_rebuild = true;
            }
        }
    }
    return requires_graph_rebuild;
}

bool Scene::ApplyUserLightVisibilityBindings(std::string_view key, const Json& property) {
    bool changed = false;
    for (auto& light : lights) {
        if (! light) continue;
        auto visible =
            ResolveSceneUserVisibilityBinding(light->visibleUserBinding(), key, property);
        if (! visible) continue;
        changed |= light->runtimeVisible() != *visible;
        light->setRuntimeVisible(*visible);
    }
    return changed;
}

bool Scene::ApplyUserCameraPathVisibilityBindings(std::string_view key, const Json& property) {
    auto it = camera_path_user_index.find(std::string(key));
    if (it == camera_path_user_index.end()) return false;

    bool changed = false;
    for (auto& path : it->second) {
        if (! path) continue;
        auto enabled = ResolveSceneUserVisibilityBinding(path->visible_user_binding, key, property);
        if (! enabled) continue;
        changed |= path->enabled != *enabled;
        path->SetEnabled(*enabled);
    }
    return changed;
}

std::vector<SceneMeshDirtyEvent> Scene::ConsumePreparedMeshDirtyEvents() {
    if (m_resource_index.Empty()) RebuildResourceIndex();

    std::vector<SceneMeshDirtyEvent> events;
    for (auto* mesh : m_resource_index.Meshes()) {
        if (mesh == nullptr) continue;
        auto flags = mesh->DirtyFlags();
        if (flags == SceneMeshDirtyNone) continue;

        SceneMeshDirtyFlags consume     = SceneMeshDirtyNone;
        SceneMeshDirtyFlags event_flags = SceneMeshDirtyNone;
        if ((flags & SceneMeshDirtyLayout) != 0) {
            consume     = SceneMeshDirtyAll;
            event_flags = SceneMeshDirtyLayout;
        } else if (! mesh->Dynamic() && (flags & SceneMeshDirtyData) != 0) {
            consume     = SceneMeshDirtyData;
            event_flags = SceneMeshDirtyData;
        }
        if (consume == SceneMeshDirtyNone) continue;

        auto consumed = mesh->ConsumeDirtyFlags(consume);
        if ((consumed & SceneMeshDirtyLayout) != 0) {
            event_flags = SceneMeshDirtyLayout;
        } else if ((consumed & SceneMeshDirtyData) == 0) {
            continue;
        }

        if (auto id = m_resource_index.meshId(*mesh)) {
            events.push_back(SceneMeshDirtyEvent { .mesh = *id, .flags = event_flags });
        }
    }
    return events;
}

std::vector<SceneMaterialDirtyEvent> Scene::ConsumePreparedMaterialDirtyEvents() {
    if (m_resource_index.Empty()) RebuildResourceIndex();

    std::vector<SceneMaterialDirtyEvent> events;
    for (auto* material : m_resource_index.Materials()) {
        if (material == nullptr) continue;
        auto flags = material->DirtyFlags();
        if (flags == SceneMaterialDirtyNone) continue;

        SceneMaterialDirtyFlags consume     = SceneMaterialDirtyNone;
        SceneMaterialDirtyFlags event_flags = SceneMaterialDirtyNone;
        if ((flags & SceneMaterialDirtyGraph) != 0) {
            consume     = SceneMaterialDirtyAll;
            event_flags = SceneMaterialDirtyGraph;
        } else {
            consume     = flags & (SceneMaterialDirtyResources | SceneMaterialDirtyPipeline);
            event_flags = consume;
        }
        if (consume == SceneMaterialDirtyNone) continue;

        auto consumed = material->ConsumeDirtyFlags(consume);
        if ((consumed & SceneMaterialDirtyGraph) != 0) {
            event_flags = SceneMaterialDirtyGraph;
        } else {
            event_flags = consumed & (SceneMaterialDirtyResources | SceneMaterialDirtyPipeline);
        }
        if (event_flags == SceneMaterialDirtyNone) continue;

        if (auto id = m_resource_index.materialId(*material)) {
            events.push_back(SceneMaterialDirtyEvent { .material = *id, .flags = event_flags });
        }
    }
    return events;
}

void Scene::TickCameraPaths() {
    if (camera_paths.empty()) return;

    // Recycle the scratch containers' capacity across frames; clear() keeps the
    // allocated buckets so a steady-state scene does no per-frame heap churn.
    auto& has_enabled = m_camera_path_has_enabled;
    auto& touched     = m_camera_path_touched;
    auto& reset       = m_camera_path_reset;
    has_enabled.clear();
    touched.clear();
    reset.clear();

    for (const auto& path : camera_paths) {
        if (path && path->enabled) has_enabled[path->camera_name] = true;
    }

    for (const auto& path : camera_paths) {
        if (! path || ! path->enabled) continue;
        if (path->Tick(elapsingTime)) touched.insert(path->camera_name);
    }
    for (const auto& path : camera_paths) {
        if (! path || has_enabled[path->camera_name] || reset.contains(path->camera_name)) continue;
        if (path->ApplyDefault()) touched.insert(path->camera_name);
        reset.insert(path->camera_name);
    }

    for (const auto& name : touched) UpdateLinkedCamera(name);
}

void Scene::TickMaterialShaderAnimations() {
    if (m_resource_index.Empty()) RebuildResourceIndex();

    for (auto* material : m_resource_index.Materials()) {
        if (material == nullptr) continue;
        material->TickShaderValueAnimations(elapsingTime);
    }
}

void Scene::TickNodeFieldAnimations() {
    auto tick_node = [runtime = elapsingTime](auto&                             self,
                                              const rstd::sync::Arc<SceneNode>& node) -> void {
        if (! node) return;
        node->TickFieldAnimations(runtime);
        for (const auto& child : node->GetChildren()) self(self, child);
    };
    tick_node(tick_node, sceneGraph);
}

void Scene::TickTransformUpdaters() {
    for (auto& update : transform_updaters) update(elapsingTime);
}

void Scene::CaptureCameraPathViewports() {
    for (auto& path : camera_paths) {
        if (path) path->CaptureViewport();
    }
}

std::string Scene::EnsureLinkRenderTarget(WallpaperLayerId source_layer,
                                          const SceneNode& source_node) {
    auto link_key = GenLinkTex(static_cast<std::ptrdiff_t>(source_layer.value));
    if (renderTargets.count(link_key) == 0) {
        auto sz                 = source_node.Size();
        renderTargets[link_key] = {
            .width      = sz.x() > 0 ? static_cast<i32>(sz.x()) : ortho[0],
            .height     = sz.y() > 0 ? static_cast<i32>(sz.y()) : ortho[1],
            .allowReuse = false,
        };
    }
    return link_key;
}

} // namespace sr

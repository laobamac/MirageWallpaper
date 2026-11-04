module;
#include <utility>

module sr.vulkan_render;
import sr.core;
import sr.types;
import rstd.cppstd;
import sr.vulkan;
import sr.scene;

namespace sr::vulkan
{

namespace
{

ShaderReflectionKey MakeShaderReflectionKey(const SceneShader& shader) {
    return ShaderReflectionKey {
        .shader    = &shader,
        .shader_id = shader.id,
        .code_hash = SceneShaderCodeHash(shader),
    };
}

CachedShaderReflection MakeCachedReflection(std::vector<Uni_ShaderSpv> spvs,
                                            ShaderReflected            reflected) {
    CachedShaderReflection out;
    out.stages.reserve(spvs.size());
    for (auto& spv : spvs) {
        if (! spv) continue;
        out.stages.push_back(CachedShaderStage {
            .entry_point = spv->entry_point,
            .stage       = spv->stage,
            .spirv       = std::move(spv->spirv),
        });
    }
    out.reflected = std::move(reflected);
    return out;
}

} // namespace

std::size_t ShaderReflectionKeyHash::operator()(const ShaderReflectionKey& key) const {
    std::size_t seed { 0 };
    utils::hash_combine(seed, key.shader);
    utils::hash_combine(seed, key.shader_id);
    utils::hash_combine(seed, key.code_hash);
    return seed;
}

const CachedShaderReflection* ShaderReflectionCache::Query(const SceneShader& shader) {
    auto key = MakeShaderReflectionKey(shader);
    if (auto it = m_entries.find(key); it != m_entries.end()) {
        return &it->second;
    }

    std::vector<Uni_ShaderSpv> spvs;
    ShaderReflected            reflected;
    if (! GenReflect(shader.codes, spvs, reflected)) return nullptr;

    auto [it, inserted] = m_entries.emplace(
        std::move(key), MakeCachedReflection(std::move(spvs), std::move(reflected)));
    (void)inserted;
    return &it->second;
}

void ShaderReflectionCache::Clear() { m_entries.clear(); }

std::vector<Uni_ShaderSpv> CloneShaderSpvs(const CachedShaderReflection& cached) {
    std::vector<Uni_ShaderSpv> out;
    out.reserve(cached.stages.size());
    for (const auto& stage : cached.stages) {
        auto spv         = std::make_unique<ShaderSpv>();
        spv->entry_point = stage.entry_point;
        spv->stage       = stage.stage;
        spv->spirv       = stage.spirv;
        out.push_back(std::move(spv));
    }
    return out;
}

} // namespace sr::vulkan

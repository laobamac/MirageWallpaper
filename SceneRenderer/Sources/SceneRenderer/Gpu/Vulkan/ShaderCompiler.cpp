module;

#include <rstd/macro.hpp>
#include <spirv_reflect.h>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include "Utils/Sha.hpp"
module sr.shader_compile;
import sr.core;
import sr.types;
import rstd.log;
import rstd.cppstd;

using namespace sr;
using namespace sr::vulkan;

namespace
{
// Spill a payload to /tmp/<sha1> for post-mortem inspection. Returns the
// written path so callers can mention it in the error message.
std::string logToTmpfileWithSha1(std::span<const char> in) {
    std::string           name   = utils::genSha1(in);
    std::filesystem::path fspath = std::filesystem::temp_directory_path() / name;
    std::string           path   = fspath.native();
    auto*                 file   = std::fopen(path.c_str(), "wb");
    if (! file) return path;
    std::fwrite(in.data(), 1, in.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
    return path;
}

inline VkShaderStageFlagBits ToVkType(sr::ShaderType s) {
    switch (s) {
    case ShaderType::VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderType::FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderType::GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
    }
    rstd_assert(false);
    return VK_SHADER_STAGE_VERTEX_BIT;
}

inline VkFormat ToVkType(SpvReflectFormat type) { return static_cast<VkFormat>(type); }

inline VkShaderStageFlagBits ToVkType(SpvReflectShaderStageFlagBits s) {
    switch (s) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT: return VK_SHADER_STAGE_VERTEX_BIT;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: return VK_SHADER_STAGE_GEOMETRY_BIT;
    default: rstd_assert(false); return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

inline sr::ShaderType FromSpvStage(SpvReflectShaderStageFlagBits s) {
    switch (s) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT: return ShaderType::VERTEX;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return ShaderType::FRAGMENT;
    case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: return ShaderType::GEOMETRY;
    default: rstd_assert(false); return ShaderType::VERTEX;
    }
}

template<typename VEC, typename FUNC>
bool EnumAllRef(VEC& vec, FUNC&& func) {
    unsigned count { 0 };
    auto     result = func(&count, nullptr);
    rstd_assert(result == SPV_REFLECT_RESULT_SUCCESS);
    vec.resize(count);
    result = func(&count, vec.data());
    rstd_assert(result == SPV_REFLECT_RESULT_SUCCESS);
    return result == SPV_REFLECT_RESULT_SUCCESS;
}

inline EShLanguage ToEShLanguage(sr::ShaderType s) {
    switch (s) {
    case ShaderType::VERTEX: return EShLangVertex;
    case ShaderType::FRAGMENT: return EShLangFragment;
    case ShaderType::GEOMETRY: return EShLangGeometry;
    }
    rstd_assert(false);
    return EShLangVertex;
}

inline glslang::EShTargetClientVersion ToClientVersion(VulkanTarget t) {
    switch (t) {
    case VulkanTarget::Vulkan_1_0: return glslang::EShTargetVulkan_1_0;
    case VulkanTarget::Vulkan_1_1: return glslang::EShTargetVulkan_1_1;
    case VulkanTarget::Vulkan_1_2: return glslang::EShTargetVulkan_1_2;
    case VulkanTarget::Vulkan_1_3: return glslang::EShTargetVulkan_1_3;
    }
    return glslang::EShTargetVulkan_1_1;
}

inline glslang::EShTargetLanguageVersion ToSpvVersion(VulkanTarget t) {
    // Pair Vulkan target with the matching SPIR-V version. See
    // https://github.com/KhronosGroup/glslang/blob/main/StandAlone/StandAlone.cpp
    switch (t) {
    case VulkanTarget::Vulkan_1_0: return glslang::EShTargetSpv_1_0;
    case VulkanTarget::Vulkan_1_1: return glslang::EShTargetSpv_1_3;
    case VulkanTarget::Vulkan_1_2: return glslang::EShTargetSpv_1_5;
    case VulkanTarget::Vulkan_1_3: return glslang::EShTargetSpv_1_6;
    }
    return glslang::EShTargetSpv_1_3;
}

inline const char* DefaultEntryName(SourceLang lang, sr::ShaderType s) {
    if (lang == SourceLang::Glsl) return "main";
    switch (s) {
    case ShaderType::VERTEX: return "main_vs";
    case ShaderType::FRAGMENT: return "main_ps";
    case ShaderType::GEOMETRY: return "main_gs";
    }
    return "main";
}

} // namespace

bool sr::vulkan::GenReflect(std::span<const std::vector<unsigned>> codes,
                             std::vector<Uni_ShaderSpv>& spvs, ShaderReflected& ref) {
    spvs.clear();
    for (const auto& code : codes) {
        spv_reflect::ShaderModule spv_ref(code, SPV_REFLECT_MODULE_FLAG_NO_COPY);
        VkShaderStageFlagBits     stage = ::ToVkType(spv_ref.GetShaderStage());
        {
            Uni_ShaderSpv spv = std::make_unique<ShaderSpv>();
            spv->stage        = ::FromSpvStage(spv_ref.GetShaderStage());
            spv->spirv        = code;
            if (const char* ep = spv_ref.GetEntryPointName(); ep && ep[0] != '\0') {
                spv->entry_point = ep;
            }
            spvs.emplace_back(std::move(spv));
        }
        std::vector<SpvReflectInterfaceVariable*> inputs;
        std::vector<SpvReflectDescriptorBinding*> bindings;

        bool ok = EnumAllRef(bindings, [&](auto&&... args) {
            return spv_ref.EnumerateDescriptorBindings(args...);
        });
        if (! ok) return false;

        VkDescriptorSetLayoutBinding vkbinding {};
        vkbinding.stageFlags = stage;

        for (auto pb : bindings) {
            auto& b = *pb;
            if (! b.accessed) continue;

            auto bind_name = std::string(b.name).empty() && b.type_description->type_name != nullptr
                                 ? b.type_description->type_name
                                 : b.name;

            if (exists(ref.binding_map, bind_name)) {
                auto& bind = ref.binding_map[bind_name];
                bind.stageFlags |= stage;
                continue;
            }
            if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                auto& block      = b.block;
                auto  block_name = std::string(block.name).empty() ? bind_name : block.name;
                ref.blocks.push_back(ShaderReflected::Block {
                    .size = block.size, .name = block.name, .member_map = {} });
                auto& ref_block = ref.blocks.front();

                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

                for (u32 i = 0; i < block.member_count; i++) {
                    auto&                           unif = block.members[i];
                    ShaderReflected::BlockedUniform bunif {};
                    bunif.size                      = unif.size;
                    bunif.offset                    = unif.offset;
                    ref_block.member_map[unif.name] = bunif;
                }
            } else if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                       b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                // Our HLSL synth emits `Texture2D + SamplerState` at the same
                // binding with `[[vk::combinedImageSampler]]`. glslang doesn't
                // expand that attribute into an OpTypeSampledImage; SPIRV-Reflect
                // sees SAMPLED_IMAGE (the Texture2D half) accessed and the
                // SamplerState as unaccessed. Bind it as a VK combined image
                // sampler — per Vulkan 1.0 §14.5.2 a COMBINED_IMAGE_SAMPLER
                // descriptor is legal to access via either OpTypeSampledImage
                // or separate OpTypeImage/OpTypeSampler.
                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            } else if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER) {
                // The paired SamplerState half of the combined image sampler
                // pair — already covered by the SAMPLED_IMAGE entry at the
                // same binding.
                continue;
            } else {
                rstd_error("unknown DescriptorBinding {}", (int)b.descriptor_type);
                return false;
            }

            ref.binding_map[bind_name] = vkbinding;
        }

        if (stage == VK_SHADER_STAGE_VERTEX_BIT) {
            EnumAllRef(inputs, [&](auto&&... args) {
                return spv_ref.EnumerateInputVariables(args...);
            });

            for (auto pinput : inputs) {
                auto& input = *pinput;
                if (sr::sstart_with(input.name, "gl_")) continue;

                if (input.location == std::numeric_limits<decltype(input.location)>::max()) {
                    rstd_error("shader input {} no location", input.name);
                    return false;
                }
                ShaderReflected::Input rinput;
                rinput.location = input.location;
                rinput.format   = ::ToVkType(input.format);

                // Strip HLSL struct-prefixed names. glslang HLSL emits VS
                // inputs as `<entry_param_name>.<field>` (e.g. our wrapper's
                // `_ww_in.a_Position`). DXC used `in.var.<SEMANTIC>`. C++
                // vertex-buffer layout matches by bare attribute name
                // (`a_Position`), so strip everything up to the final `.`.
                std::string_view name = input.name;
                if (auto dot = name.rfind('.'); dot != std::string_view::npos) {
                    name.remove_prefix(dot + 1);
                }
                ref.input_location_map[std::string(name)] = rinput;
            }
        }
    }
    return true;
}

void sr::vulkan::InitProcess() { glslang::InitializeProcess(); }
void sr::vulkan::FinalizeProcess() { glslang::FinalizeProcess(); }

namespace
{

// Configure a TShader for our Vulkan target. The user shader has already
// gone through PreShaderHeader (combo `#define`s + GLSL prologue + the
// __SHADER_PLACEHOLD__ slot for synthesized layouts) so glslang sees
// fully resolved source.
void ConfigureShader(glslang::TShader& shader, SourceLang lang, VulkanTarget target,
                     const char* entry) {
    glslang::EShSource src_lang =
        (lang == SourceLang::Hlsl) ? glslang::EShSourceHlsl : glslang::EShSourceGlsl;
    shader.setEnvInput(src_lang, shader.getStage(), glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, ToClientVersion(target));
    shader.setEnvTarget(glslang::EShTargetSpv, ToSpvVersion(target));
    shader.setEntryPoint(entry);
    shader.setSourceEntryPoint(entry);
    // We emit explicit `layout(location=N)` and `layout(set=B, binding=K)`
    // ourselves from Finalprocessor, so glslang's auto-binding stays off.
    shader.setAutoMapLocations(false);
    shader.setAutoMapBindings(false);
    // Be lenient about combinations of #version / unset clip ranges in WE
    // shaders — they mostly look like GLSL 110 desktop with Vulkan semantics
    // grafted on top.
    shader.setEnvInputVulkanRulesRelaxed();
}

constexpr EShMessages kCompileMessages =
    static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgRelaxedErrors |
                             EShMsgSuppressWarnings | EShMsgKeepUncalled);

} // namespace

bool sr::vulkan::Preprocess(std::string_view src, ShaderType stage, SourceLang lang,
                             std::string& out) {
    glslang::TShader shader(ToEShLanguage(stage));
    std::string      src_copy(src);
    const char*      data = src_copy.c_str();
    const int        len  = (int)src_copy.size();
    const char*      name = "ww";
    shader.setStringsWithLengthsAndNames(&data, &len, &name, 1);
    ConfigureShader(shader, lang, VulkanTarget::Vulkan_1_1, DefaultEntryName(lang, stage));

    const int                        default_version = 110;
    const EProfile                   profile         = ECoreProfile;
    const bool                       forward_compat  = false;
    glslang::TShader::ForbidIncluder includer;

    std::string preprocessed;
    bool        ok = shader.preprocess(GetDefaultResources(),
                                       default_version,
                                       profile,
                                       false,
                                       forward_compat,
                                       kCompileMessages,
                                       &preprocessed,
                                       includer);
    if (! ok) {
        std::string tmp = logToTmpfileWithSha1(src);
        rstd_error("glslang(preprocess): {}", shader.getInfoLog());
        rstd_error("shader source is at {}", tmp);
        return false;
    }
    out = std::move(preprocessed);
    return true;
}

bool sr::vulkan::CompileAndLinkShaderUnits(std::span<const ShaderCompUnit> compUnits,
                                            const ShaderCompOpt&            opt,
                                            std::vector<Uni_ShaderSpv>&     spvs) {
    spvs.clear();
    spvs.reserve(compUnits.size());

    for (const auto& unit : compUnits) {
        const std::string entry_str =
            unit.entry_point.empty() ? DefaultEntryName(unit.lang, unit.stage) : unit.entry_point;
        const char* entry = entry_str.c_str();

        glslang::TShader shader(ToEShLanguage(unit.stage));
        const char*      data = unit.src.c_str();
        const int        len  = (int)unit.src.size();
        const char*      name = "ww";
        shader.setStringsWithLengthsAndNames(&data, &len, &name, 1);
        ConfigureShader(shader, unit.lang, opt.target, entry);

        const int                        default_version = 110;
        const EProfile                   profile         = ECoreProfile;
        const bool                       forward_compat  = false;
        glslang::TShader::ForbidIncluder includer;

        if (! shader.parse(GetDefaultResources(),
                           default_version,
                           profile,
                           false,
                           forward_compat,
                           kCompileMessages,
                           includer)) {
            std::string tmp = logToTmpfileWithSha1(unit.src);
            // Strip WARNING lines; EShMsgSuppressWarnings doesn't actually
            // omit them from the info log on this glslang build.
            std::string log = shader.getInfoLog();
            std::string filtered;
            for (std::size_t i = 0, e = log.size(); i < e;) {
                std::size_t nl = log.find('\n', i);
                if (nl == std::string::npos) nl = e;
                std::string_view line(log.data() + i, nl - i);
                if (line.find("WARNING") == std::string_view::npos) {
                    filtered.append(line);
                    filtered.push_back('\n');
                }
                i = nl + 1;
            }
            rstd_error("glslang(parse): {}", filtered);
            if (const char* d = shader.getInfoDebugLog(); d && d[0])
                rstd_error("glslang(parse debug): {}", d);
            rstd_error("shader source is at {}", tmp);
            return false;
        }

        glslang::TProgram program;
        program.addShader(&shader);
        if (! program.link(kCompileMessages)) {
            std::string tmp = logToTmpfileWithSha1(unit.src);
            rstd_error("glslang(link): {}", program.getInfoLog());
            rstd_error("shader source is at {}", tmp);
            return false;
        }

        glslang::TIntermediate* intermediate = program.getIntermediate(ToEShLanguage(unit.stage));
        if (! intermediate) {
            rstd_error("glslang(intermediate): no intermediate for stage");
            return false;
        }

        glslang::SpvOptions spv_opts;
        spv_opts.validate          = true;
        spv_opts.generateDebugInfo = false;
        spv_opts.disableOptimizer  = ! opt.optimize;
        spv::SpvBuildLogger logger;

        Uni_ShaderSpv spv = std::make_unique<ShaderSpv>();
        spv->stage        = unit.stage;
        spv->entry_point  = entry_str;
        glslang::GlslangToSpv(*intermediate, spv->spirv, &logger, &spv_opts);

        if (auto msgs = logger.getAllMessages(); ! msgs.empty()) {
            rstd_warn("glslang(spv): {}", msgs);
        }
        if (spv->spirv.empty()) {
            rstd_error("glslang(spv): no SPIR-V output produced");
            return false;
        }

        const char* dump_spirv = std::getenv("SCENERENDERER_DUMP_SPIRV");
        if (dump_spirv == nullptr || dump_spirv[0] == '\0') {
            dump_spirv = std::getenv("SCENERENDERER_DUMP_SPIRV");
        }
        if (dump_spirv != nullptr && dump_spirv[0] != '\0' && dump_spirv[0] != '0') {
            static int            dump_idx = 0;
            std::filesystem::path base_path =
                std::filesystem::temp_directory_path() /
                ("SceneRenderer_spirv_" + std::to_string(dump_idx++) + "_" + entry_str);
            std::string base     = base_path.native();
            std::string spv_path = base + ".spv";
            std::string src_path = base + ".glsl";
            if (auto* f = std::fopen(spv_path.c_str(), "wb")) {
                std::fwrite(spv->spirv.data(), sizeof(u32), spv->spirv.size(), f);
                std::fclose(f);
            }
            if (auto* f = std::fopen(src_path.c_str(), "wb")) {
                std::fwrite(unit.src.data(), 1, unit.src.size(), f);
                std::fclose(f);
            }
            rstd_info("dumped SPIR-V + source: {}.{{spv,glsl}}", base);
        }

        spvs.emplace_back(std::move(spv));
    }

    return true;
}

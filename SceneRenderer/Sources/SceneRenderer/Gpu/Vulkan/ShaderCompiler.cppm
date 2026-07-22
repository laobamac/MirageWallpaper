module;

// Vulkan headers only — no dispatch / loader. ShaderReflected exposes
// VkDescriptorSetLayoutBinding and VkFormat, which SPIRV-Reflect produces
// natively, so consumers can hand the reflection straight to a pipeline
// builder without translating types. We do NOT link Vulkan_LIBRARIES from
// this module's target; downstream binaries that only want shader
// compilation get the headers but no libvulkan dependency.
export module sr.shader_compile;
import sr.core;
export import vulkan;
import sr.types;
import rstd.cppstd;

export namespace sr::vulkan
{

// ---------- Spv.hpp ----------

struct ShaderSpv {
    std::string entry_point { "main" };
    ShaderType  stage;

    std::vector<unsigned int> spirv;
};

using Uni_ShaderSpv = std::unique_ptr<ShaderSpv>;

// ---------- ShaderReflect.hpp ----------

struct ShaderReflected {
    struct BlockedUniform {
        int         block_index;
        unsigned    offset;
        std::size_t size { 0 };
        std::size_t num { 1 };
    };
    struct Block {
        int         index;
        unsigned    size;
        std::string name;

        // Hashed (not tree) map: UpdateUniform probes this once per uniform per
        // frame with a string_view; iteration order is never relied upon (the
        // two walk sites sort their own output / only check overlaps).
        HashMap<BlockedUniform> member_map;
    };
    std::vector<Block> blocks;

    Map<std::string, VkDescriptorSetLayoutBinding> binding_map;

    struct Input {
        unsigned location;
        VkFormat format;
    };
    Map<std::string, Input> input_location_map;
};

bool GenReflect(std::span<const std::vector<unsigned int>> codes, std::vector<Uni_ShaderSpv>& spvs,
                ShaderReflected& ref);

// ---------- ShaderComp.hpp ----------

enum class VulkanTarget : unsigned
{
    Vulkan_1_0,
    Vulkan_1_1,
    Vulkan_1_2,
    Vulkan_1_3,
};

enum class SourceLang : unsigned
{
    Glsl,
    Hlsl,
};

struct ShaderCompUnit {
    ShaderType  stage;
    std::string src;
    std::string entry_point; // if empty, "main" (GLSL) or "main_<stage>" (HLSL).
    SourceLang  lang { SourceLang::Glsl };
};

struct ShaderCompOpt {
    VulkanTarget target { VulkanTarget::Vulkan_1_1 };
    bool         optimize { false };
};

bool CompileAndLinkShaderUnits(std::span<const ShaderCompUnit> compUnits, const ShaderCompOpt& opt,
                               std::vector<Uni_ShaderSpv>& spvs);

// Expand every `#if`, `#include` and `#define` in `src` so downstream
// regex passes see only live declarations with macros already resolved
// (e.g. `g_Bones[BONECOUNT]` becomes `g_Bones[4]`, `#if SKINNING=0`
// blocks vanish entirely). `lang` selects glslang's GLSL vs HLSL
// preprocessor. On failure returns false and leaves `out` untouched.
bool Preprocess(std::string_view src, ShaderType stage, SourceLang lang, std::string& out);

// glslang::InitializeProcess / FinalizeProcess wrappers. Init must be
// called once before any compile; Finalize at shutdown.
void InitProcess();
void FinalizeProcess();

} // namespace sr::vulkan

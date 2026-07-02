module;

export module sr.pkg.parse:wp_mdl_parser;
import eigen;
import sr.core;
import rstd.cppstd;
import sr.fs;
import sr.scene;
import sr.pkg.scene_obj;

export import sr.pkg.puppet;
import :wp_shader_parser; // WPShaderInfo

export namespace sr

{

// File header preceding the per-mesh body. Per hexpat: 4-byte version tag +
// u32 vertex layout flag + u32 always-one + u32 mesh_count.
struct WPMdlHeader {
    i32 mdlv { 13 };
    u32 mdl_flag { 0 }; // vertex layout bitmask; mdlv<=14 meshes inherit this
    u32 unk_a { 1 };    // always_one in hexpat
    u32 mesh_count { 1 };
};

struct WPMdl {
    WPMdlHeader header;

    // One element per header.mesh_count. mesh_count > 1 only seen on static
    // (non-puppet) meshes; renderer currently consumes meshes[0] only.
    struct Mesh {
        std::string mat_json_file;
        u32         flag_a { 0 }; // hexpat Mesh.flag_a (usually 0; 2 has trailing 1)
        bool        has_flag_a2_one { false };
        u32         flag { 0 }; // per-mesh vertex layout flag (mdlv>14); 0 = inherit header
        std::array<float, 3> aabb_min {};
        std::array<float, 3> aabb_max {};
        bool                 has_aabb { false }; // mdlv>=17

        // SoA attributes; empty means the bit was not set in `flag`.
        std::vector<std::array<float, 3>>    positions;
        std::vector<std::array<float, 3>>    normals;
        std::vector<std::array<float, 4>>    tangents; // tangent[3] + tangent_sign
        std::vector<std::array<uint8_t, 4>>  extra4;
        std::vector<std::array<uint32_t, 4>> blend_indices;
        std::vector<std::array<float, 4>>    blend_weights;
        std::vector<std::array<float, 2>>    texcoords;
        std::vector<std::array<float, 2>>    texcoord2;

        std::vector<std::array<uint32_t, 3>> indices;

        // V21+ Parts sub-block — uv2 region per vertex + part draw ranges.
        struct Part {
            uint32_t id;
            uint32_t start;
            uint32_t size;
        };
        std::vector<std::array<float, 2>> part_uv2;
        std::vector<uint32_t>             part_uv2_pad;
        std::vector<Part>                 parts;

        // V23+ Mask blocks attached to a single-puppet mesh.
        struct MaskBlock {
            uint32_t              leading_a;
            std::string           mat_json;
            std::vector<uint32_t> part_ids_a;
            std::vector<uint32_t> part_ids_b;
        };
        std::vector<MaskBlock> masks;
    };
    std::vector<Mesh> meshes;

    i32 mdls { 1 };
    i32 mdla { 1 };
    i32 mdle { 0 }; // 0 = section not present
    i32 mdmp { 0 };

    // MDMP morph sections — present when an animation drives shape blends.
    // Each section keyed by event_time matching a v4 AnimV4Event.time.
    struct MorphSectionData {
        uint32_t                             shape_id;
        std::string                          tag;
        uint32_t                             hash;
        std::vector<std::array<uint16_t, 3>> vertices;
        std::vector<uint16_t>                vertex_trailers; // shape_id != 0
        std::vector<uint8_t>                 trailer;         // shape_id == 0
    };
    struct MorphSection {
        float                         event_time;
        uint16_t                      event_id;
        std::vector<MorphSectionData> sections;
    };
    std::vector<MorphSection> morph_sections;

    std::shared_ptr<WPPuppet> puppet;
    // combo
    // SKINNING = 1
    // BONECOUNT

    // input
    // uvec4 a_BlendIndices
    // vec4 a_BlendWeights
    // uniform mat4x3 g_Bones[BONECOUNT]
};

class ModelAssetCompiler {
public:
    // Reads only the bytes preceding mat_json_file. Cheap; safe to call
    // over the whole corpus even on mdls that would hang full Parse.
    static bool ParseHeader(std::string_view path, fs::VFS&, WPMdlHeader&);

    static bool                             Parse(std::string_view path, fs::VFS&, WPMdl&);
    static std::optional<wpscene::Material> ParseMaterial(std::string_view ref, fs::VFS&);

    static void AddPuppetShaderInfo(WPShaderInfo& info, const WPMdl& mdl);
    static void AddPuppetMatInfo(wpscene::Material& mat, const WPMdl& mdl);

    // Emit vertex/index arrays for any WPMdl::Mesh, sending only the vertex
    // attribute streams whose SoA vectors are populated. Skinning combos must
    // be wired separately via AddPuppetShaderInfo / AddPuppetMatInfo when the
    // mesh has bone weights.
    static void GenMeshFromMdl(SceneMesh::Submesh& submesh, const WPMdl::Mesh& src,
                               std::array<float, 2> texcoord_scale = { 1.0f, 1.0f });

    // Like GenMeshFromMdl, but the submesh draws only the parts whose `id` is
    // in `clip_part_ids` — used for clipping-mask submeshes that only cover the
    // affected (e.g. iris) parts. Material slot is the caller's responsibility.
    static void GenMaskSubmeshFromMdl(SceneMesh::Submesh& submesh, const WPMdl::Mesh& src,
                                      std::span<const uint32_t> clip_part_ids,
                                      std::array<float, 2>      texcoord_scale = { 1.0f, 1.0f });
};

} // namespace sr

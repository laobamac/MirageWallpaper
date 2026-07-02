module;

export module sr.pkg.puppet;
import eigen;
import sr.core;
import rstd.cppstd;

export namespace sr

{

class WPPuppetLayer;

class WPPuppet {
public:
    enum class PlayMode
    {
        Loop,
        Mirror,
        Single
    };
    static constexpr uint32_t NO_PARENT = 0xFFFFFFFFu;

    // Bind hierarchy and animation hierarchy are tracked independently.
    //
    // - bind_parent drives prepared(): world_bind = parent.world_bind * local_bind.
    // - anim_parent drives genFrame(): the per-frame pose is composed along the
    //   anim chain, so children inherit their parent's animated motion.
    //
    // MDLV<21 stores the same parent for both. MDLV21 stores a flat bind
    // silhouette (bones are already world-anchored) but keeps the file's parent
    // chain for animation, so eye/lash bones still inherit their parent's blink.
    struct Bone {
        std::string name;
        // hexpat MDLS Bone.sim_type: 0=static, 1=physics target, 3=IK chain.
        int32_t         sim_type { 0 };
        Eigen::Affine3f local_bind { Eigen::Affine3f::Identity() };
        uint32_t        bind_parent { NO_PARENT };
        uint32_t        anim_parent { NO_PARENT };
        // Original on-file parent index. MDLV21 flattens bind_parent for
        // skinning while keeping anim_parent on this chain.
        uint32_t file_parent { NO_PARENT };

        // Per-bone WE bone_simulation JSON (spring/damping/gravity for
        // hair/cloth). Captured raw; evaluation hook is TBD.
        std::string simulation_json;

        // prepared
        Eigen::Affine3f world_bind { Eigen::Affine3f::Identity() };
        Eigen::Affine3f inv_bind { Eigen::Affine3f::Identity() };

        // Mean position of vertices weighted to this bone, expressed as an
        // offset from local_bind.translation(). Used to bake frame.scale into
        // a pure-translation g_Bones[i] (matches WE DXBC convention) — the
        // sprite anchored to this bone is approximated as a rigid body
        // centered at this offset, so scale compresses the sprite toward
        // bone.t without producing LBS triangle stretching.
        Eigen::Vector3f vertex_centroid_offset { 0.0f, 0.0f, 0.0f };

        // Per-bone "offset transform" from the MDLS section's `has_offset_trans`
        // block (3-float pos + 4x4 mat). The 3-float pos is the bone's true
        // skinning pivot in puppet-local coords — WE scales around this point,
        // not the computed vertex centroid. Captured even if mdls < 3 so the
        // existing parser path stays valid.
        Eigen::Vector3f file_skin_pivot { 0.0f, 0.0f, 0.0f };
        Eigen::Matrix4f file_skin_mat { Eigen::Matrix4f::Identity() };
        bool            has_file_skin_pivot { false };

        // Per-bone 4x4 from the MDLE section. Format is known; semantics
        // unconfirmed — linear() bit-matches local_bind.linear() but
        // translation() doesn't match world_bind / inv_bind / file_skin_pivot
        // / (local + centroid). Captured raw; no consumer yet.
        Eigen::Affine3f file_world_bind { Eigen::Affine3f::Identity() };
        bool            has_file_world_bind { false };

        bool noBindParent() const { return bind_parent == NO_PARENT; }
        bool noAnimParent() const { return anim_parent == NO_PARENT; }
    };

    // Named locator/anchor parsed from the MDAT section between MDLS and MDLA.
    // 64-byte payload is a column-major 4x4 affine transform in the anchored
    // bone's local space; `unk` is the bone index this attachment is wired to.
    // Consumed by ImageObject `attachment = "<name>"` to position child
    // images at named bone offsets (e.g. bangs under a head bone).
    struct Attachment {
        uint16_t        bone_index { 0 }; // hexpat MDAT Attachment.unk
        std::string     name;
        Eigen::Affine3f local_xform { Eigen::Affine3f::Identity() };
    };
    struct BoneFrame {
        Eigen::Vector3f position;
        Eigen::Vector3f angle;
        Eigen::Vector3f scale;

        // prepared
        Eigen::Quaterniond quaternion;
    };
    // One animated channel. Today only BoneTrack exists; the parser leaves
    // a vec slot per bone (dense, in bone order). MorphTrack / SlotTrack
    // are deliberately not declared yet — drop them in as sibling vectors
    // on Animation when V22+ shows up, not as variants over this one.
    struct BoneTrack {
        uint32_t               bone_index { 0 };
        int32_t                unk { 0 }; // hexpat BoneTrack.unk
        std::vector<BoneFrame> frames;
    };

    // mdla>=3 per-anim translation/root-motion payload. trans_flag==1 uses
    // extra_track + main_track; trans_flag==0 can store main_track followed by
    // zero-delimited same-sized tail tracks.
    struct AnimTrans {
        std::vector<float>              extra_track;
        std::vector<float>              main_track;
        std::vector<std::vector<float>> tail_tracks;
    };

    // Per-bone, per-frame curve (anim.length + 1 samples). Reused by both the
    // mdla>=3 blend_curves block (0..1 weights) and mdla==6 scalar_curves
    // (typically constant per curve).
    struct BoneFrameCurve {
        std::vector<float> values;
    };

    // mdla>=4 timed curve event. `values` is a per-frame ease/blend curve.
    struct AnimV4Event {
        float              time;
        uint32_t           flags;
        std::vector<float> values;
    };

    // Trailing event list — present on every animation regardless of mdla
    // version. `event_json` is the WE editor's keyframe payload.
    struct AnimEvent {
        uint32_t    time_value;
        std::string event_json;
    };

    struct Animation {
        i32         id;
        u32         unk_after_id { 0 };
        double      fps;
        i32         length;
        PlayMode    mode;
        std::string name;

        std::vector<BoneTrack> bone_tracks;

        // mdla>=3 trans block (presence gated by trans_flag).
        std::optional<AnimTrans> trans;
        // mdla>=3 per-bone blend curves (size == bone_tracks.size() when present).
        std::vector<BoneFrameCurve> blend_curves;
        // mdla>=4 timed events.
        std::vector<AnimV4Event> v4_events;
        // mdla>=5 anim AABB.
        std::array<float, 3> aabb_min {};
        std::array<float, 3> aabb_max {};
        bool                 has_aabb { false };
        // mdla==6 per-bone scalar curves (same shape as blend_curves).
        std::vector<BoneFrameCurve> scalar_curves;
        // Trailing event list (all mdla versions).
        std::vector<AnimEvent> events;

        // prepared
        double max_time;
        double frame_time;
        struct InterpolationInfo {
            idx    frame_a;
            idx    frame_b;
            double t;
        };
        InterpolationInfo getInterpolationInfo(double* cur_time) const;
    };

    // MDLS v3 IK chain configuration. Schema derived from a single corpus
    // sample (hexpat MDLSBlock extras_flag==2 path); fields kept raw.
    struct BoneDir {
        uint32_t             bone_id;
        std::array<float, 3> dir;
    };
    struct ChainBoneDir {
        uint16_t             chain_id;
        uint32_t             bone_id;
        std::array<float, 3> dir;
    };
    struct BoneCond {
        uint16_t cnt;
        uint32_t id;
        uint32_t child;
        uint32_t val;
    };
    struct IkConfig {
        Eigen::Matrix4f                      chain_a_target { Eigen::Matrix4f::Identity() };
        uint8_t                              ik_version { 0 };
        std::array<uint32_t, 2>              ik_header {};
        Eigen::Matrix4f                      chain_b_target { Eigen::Matrix4f::Identity() };
        std::array<uint8_t, 7>               ik_flags {};
        std::array<Eigen::Vector3f, 6>       pole_targets {};
        std::vector<BoneDir>                 rest_rotations;
        std::vector<ChainBoneDir>            ik_targets;
        std::optional<BoneDir>               ik_target_root;
        BoneCond                             ik_constraint {};
        std::array<std::vector<uint32_t>, 2> ik_bone_lists;
        uint32_t                             ik_chain_count { 0 };
        std::array<float, 2>                 ik_chain_length {};
        std::vector<uint32_t>                ik_chain_bones;
    };

public:
    std::vector<Bone>       bones;
    std::vector<Animation>  anims;
    std::vector<Attachment> attachments;
    std::optional<IkConfig> ik_config;

    // MDLV21 puppets store bones as world-anchored (each `local_bind.t` is
    // a puppet-local position, not parent-relative) and sprite vertices live
    // at `bind.t + vertex_centroid_offset`. Skinning needs to flatten the
    // parent chain and pivot scale/rotation around the centroid; MDLV22+
    // uses standard chain LBS. Set at parse time from `header.mdlv`.
    bool world_anchored_bones { false };

    std::span<const Eigen::Affine3f> genFrame(WPPuppetLayer&, double time) noexcept;
    void                             prepared();

private:
    std::vector<Eigen::Affine3f> m_final_affines;
};

class WPPuppetLayer {
    friend class WPPuppet;

public:
    WPPuppetLayer();
    WPPuppetLayer(std::shared_ptr<WPPuppet>);
    ~WPPuppetLayer();

    bool hasPuppet() const { return (bool)m_puppet; };

    struct AnimationLayer {
        i32    id { 0 }; // animation file id (PKGV0001+)
        double rate { 1.0f };
        double blend { 1.0f };
        bool   visible { true };
        double cur_time { 0.0f };

        // Schema-only absorption (renderer reads only id/rate/blend/visible).
        i32         layer_id { 0 };     // animationlayers[].id (was unread)
        std::string name;               // animationlayers[].name (was unread)
        bool        additive { false }; // PKGV0019+; blend operator
        bool        blendin { false };  // PKGV0021+
        bool        blendout { false }; // PKGV0021+
        double      blendtime { 0.0 };  // PKGV0021+
    };

    void prepared(std::span<AnimationLayer>);

    std::span<const Eigen::Affine3f> genFrame(double time) noexcept;
    uint32_t                         boneIndex(std::string_view name) const noexcept;
    std::optional<Eigen::Affine3f>   boneTransform(uint32_t index, double time) noexcept;

    void updateInterpolation(double time) noexcept;

private:
    struct Layer {
        AnimationLayer                         anim_layer;
        const WPPuppet::Animation*             anim { nullptr };
        WPPuppet::Animation::InterpolationInfo interp_info {};

        operator bool() const noexcept { return anim != nullptr; };
    };

    // Absolute scene elapsed time of the last advance. Guards against
    // multi-pass nodes (e.g. puppet with mask pre-pass + clipped-main)
    // advancing animation N× per frame.
    double m_last_elapsed { -1.0 };

    std::vector<Layer>        m_layers;
    std::shared_ptr<WPPuppet> m_puppet;
};

} // namespace sr

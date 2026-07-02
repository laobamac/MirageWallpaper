module;

export module sr.scene:lighting;
import eigen;
import sr.core;
import rstd.cppstd;

// SceneNode lives in the primary interface unit of sr.scene. SceneLight
// only needs a raw observation pointer here; ownership is carried by the
// scene tree.
export namespace sr
{

class SceneNode;

enum class SceneLightType : u8
{
    Point       = 0,
    Spot        = 1,
    Directional = 2
};

class SceneLight {
public:
    struct Desc {
        SceneLightType  type { SceneLightType::Point };
        Eigen::Vector3f color { 1.0f, 1.0f, 1.0f };
        float           radius { 0.0f };
        float           intensity { 1.0f };
        float           exponent { 1.0f };
        float           attenuation { 0.0f };
        float           mindistance { 0.0f };
        // Cone angle cosines (cos(half-angle)). Identity (1.0) = no falloff.
        float inner_cone_cos { 1.0f };
        float outer_cone_cos { 1.0f };
        float light_source_size { 0.0f };
        float cascade_distances[3] { 0.0f, 0.0f, 0.0f };
        bool  cast_shadow { false };
        bool  cast_volumetrics { false };
    };

    explicit SceneLight(const Desc& d)
        : m_desc(d), m_premultiplied_color(d.color * d.intensity * d.radius * d.radius) {}
    ~SceneLight() = default;

    const Desc&     desc() const { return m_desc; }
    SceneLightType  type() const { return m_desc.type; }
    Eigen::Vector3f color() const { return m_desc.color * m_desc.intensity; }
    float           radius() const { return m_desc.radius; }
    SceneNode*      node() const { return m_node; }
    // Legacy uniform G_LCP layout (color * intensity * radius²) consumed by
    // shaders that bind g_LightsColorPremultiplied.
    Eigen::Vector3f premultipliedColor() const { return m_premultiplied_color; }

    void setNode(SceneNode* node) { m_node = node; }

    // WE field-binding: `visible: {user: "key", value: <bool>}` ties this light's
    // runtime visibility to engine.userProperties[<key>]. Empty key = unbound.
    const std::string& visibleUserKey() const { return m_visible_user_key; }
    void               setVisibleUserKey(std::string k) { m_visible_user_key = std::move(k); }
    bool               runtimeVisible() const { return m_runtime_visible; }
    void               setRuntimeVisible(bool v) { m_runtime_visible = v; }

private:
    Desc            m_desc;
    Eigen::Vector3f m_premultiplied_color { Eigen::Vector3f::Zero() };
    SceneNode*      m_node { nullptr };

    std::string m_visible_user_key {};
    bool        m_runtime_visible { true };
};

} // namespace sr

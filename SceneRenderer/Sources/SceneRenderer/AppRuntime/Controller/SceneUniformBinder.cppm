module;

export module sr.scene_uniform_updater;
import eigen;
import sr.core;
import rstd.cppstd;
import sr.scene;

import sr.pkg.puppet; // WPPuppetLayer

export namespace sr
{

struct SceneUniformInfo {
    bool has_MI { false };
    bool has_M { false };
    bool has_AM { false };
    bool has_MVP { false };
    bool has_MVPI { false };
    bool has_EYEPOSITION { false };
    bool has_EFFECTMODELMATRIX { false };
    bool has_EMVP { false };
    bool has_EMVPI { false };
    bool has_LAYERMODELMATRIX { false };
    bool has_ETVP { false };
    bool has_ETVPI { false };
    bool has_VP { false };

    bool has_BONES { false };
    bool has_TIME { false };
    bool has_FRAMETIME { false };
    bool has_DAYTIME { false };
    bool has_DAYTIME_LEGACY { false };
    bool has_POINTERPOSITION { false };
    bool has_POINTERPOSITIONLAST { false };
    bool has_PARALLAXPOSITION { false };
    bool has_TEXELSIZE { false };
    bool has_TEXELSIZEHALF { false };
    bool has_SCREEN { false };
    bool has_LP { false };
    bool has_LCR { false };
    bool has_USERALPHA { false };
    bool has_COLOR4 { false };
    bool has_COLOR { false };
    bool has_ALPHA { false };
    bool has_BRIGHTNESS { false };

    // WE audio-bar shaders. Each pair is a (Left, Right) float[N] array
    // selected by the shader's RESOLUTION combo at compile time.
    bool has_audio_16_l { false }, has_audio_16_r { false };
    bool has_audio_32_l { false }, has_audio_32_r { false };
    bool has_audio_64_l { false }, has_audio_64_r { false };

    struct Tex {
        bool has_resolution { false };
        bool has_mipmap { false };
    };
    std::array<Tex, 12> texs;
};

struct SceneUniformNodeData {
    std::array<float, 2>                       parallaxDepth { 0.0f, 0.0f };
    std::array<float, 2>                       propagatedParallaxDepth { 0.0f, 0.0f };
    bool                                       propagate_parallax_to_children { true };
    std::vector<std::pair<usize, std::string>> renderTargets;
    std::shared_ptr<WPPuppetLayer>             puppet_layer;
    bool                                       use_camera_eye_position { false };
    SceneNode*                                 effect_projection_node { nullptr };
    std::array<float, 2>                       effect_projection_size { 0.0f, 0.0f };
};

struct SceneCameraParallax {
    bool  enable { false };
    float amount;
    float delay;
    float mouseinfluence;
};

struct SceneCameraShake {
    bool  enable { false };
    float amplitude { 0.0f };
    float speed { 0.0f };
    float roughness { 1.0f };
};

class SceneUniformUpdater : public IShaderValueUpdater {
public:
    SceneUniformUpdater(Scene* scene): m_scene(scene) {}
    virtual ~SceneUniformUpdater() {}

    void FrameBegin() override;

    void InitUniforms(SceneNode*, const ExistsUniformOp&) override;
    void UpdateUniforms(SceneNode*, sprite_map_t&, const UpdateUniformOp&) override;
    void FrameEnd() override;
    void MouseInput(double, double) override;
    void SetTexelSize(float x, float y) override;

    void SetNodeData(void*, const SceneUniformNodeData&);
    // Replicate the uniform record from src to dst. Used when scripts clone
    // a SceneNode at parse time so the clones pick up the template data.
    void CopyNodeData(void* src, void* dst);
    void SetCameraParallax(const SceneCameraParallax& value) { m_parallax = value; }
    void SetCameraShake(const SceneCameraShake& value) { m_cameraShake = value; }
    void SetCameraParallaxMouseInfluence(float value) override {
        m_parallax.mouseinfluence = value;
    }
    void SetCameraShakeEnabled(bool value) override { m_cameraShake.enable = value; }
    void SetCameraShakeAmplitude(float value) override { m_cameraShake.amplitude = value; }
    void SetCameraShakeSpeed(float value) override { m_cameraShake.speed = value; }
    void SetCameraShakeRoughness(float value) override { m_cameraShake.roughness = value; }

    // Push the current 64-bin spectrum snapshot. Renderer calls this once
    // per frame before drawFrame. Used to fill `g_AudioSpectrum{16,32,64}{Left,Right}`
    // shader uniforms in UpdateUniforms.
    void SetAudioSpectrum(std::span<const float, 64> left,
                          std::span<const float, 64> right) override;

    void SetScreenSize(i32 w, i32 h) override { m_screen_size = { (float)w, (float)h }; }

private:
    Scene*               m_scene;
    SceneCameraParallax  m_parallax;
    SceneCameraShake     m_cameraShake;
    double               m_dayTime { 0.0f };
    std::array<float, 2> m_texelSize { 1.0f / 1920.0f, 1.0f / 1080.0f };

    std::array<float, 2> m_mousePos { 0.5f, 0.5f };
    std::array<float, 2> m_mousePosLast { 0.5f, 0.5f };
    std::array<float, 2> m_mousePosInput { 0.5f, 0.5f };
    double               m_mouseDelayedTime { 0.0f };
    unsigned             m_mouseInputCount { 0 };

    std::chrono::time_point<std::chrono::steady_clock> m_last_mouse_input_time;

    std::array<float, 2> m_screen_size { 1920, 1080 };

    // Per-frame visual bands ready for std140 packing in UpdateUniforms.
    // AudioCapture already applies smoothing and amplitude mapping.
    std::array<float, 16> m_audio_16_l {};
    std::array<float, 16> m_audio_16_r {};
    std::array<float, 32> m_audio_32_l {};
    std::array<float, 32> m_audio_32_r {};
    std::array<float, 64> m_audio_64_l {};
    std::array<float, 64> m_audio_64_r {};

    // Reused std140 packing buffer for the audio-bar uniforms. Each of the up
    // to six per-frame audio uploads packs one amplitude into .x of a vec4, so
    // the widest case is 64 bands * 4 = 256 floats. Kept as a member to avoid
    // heap-allocating a fresh std::vector on every band, every frame.
    std::array<float, 64 * 4> m_audio_pack_scratch {};

    Map<void*, SceneUniformNodeData> m_nodeDataMap;
    Map<void*, SceneUniformInfo>     m_nodeUniformInfoMap;
};

} // namespace sr

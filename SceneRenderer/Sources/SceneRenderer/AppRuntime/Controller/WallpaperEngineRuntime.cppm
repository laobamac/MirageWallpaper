module;
#include <vulkan/vulkan.h>

export module sr.scene_wallpaper;
import sr.core;
import sr.json;
import rstd.cppstd;

export import sr.scene;
export import sr.vulkan_render;
export import sr.vulkan;
export import sr.types;
export import sr.pkg.parse;

export namespace sr
{

using FirstFrameCallback = std::function<void()>;
using UserPropertyDiagnosticCallback =
    std::function<void(std::vector<SceneUserPropertyDiagnostic>)>;
using RenderPassDiagnosticCallback =
    std::function<void(std::vector<vulkan::PreparedPassDiagnostic>)>;

// Fired once per loaded scene with the parsed `general.clearcolor` so the
// host can keep letterbox bars aligned with the scene's intended background.
// Components are 0..=1 sRGB. Alpha is fixed at 1.0 by the host because the
// rendered frame is always opaque.
using ClearColorCallback = std::function<void(float r, float g, float b)>;
using AudioDemandCallback = std::function<void(bool needed)>;

struct MediaStatus {
    uint32_t    state { 0 };
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string art_url;
    std::string previous_art_url;
};

struct SceneWallpaperConfig {
    std::string                             source_pkg_path;
    std::string                             assets_dir;
    std::string                             cache_dir;
    std::shared_ptr<wpscene::SceneDocument> scene_document;
    rstd::json::Map                         user_properties;
    uint32_t                                fps { 30 };
    float                                   volume { 1.0f };
    bool                                    muted { false };
    FillMode                                fill_mode { FillMode::ASPECTCROP };
    float                                   speed { 1.0f };
    bool                                    graphviz { false };
    bool                                    spectrum_enabled { true };
    bool                                    external_spectrum { false };
    bool                                    load_from_memory { false };
};

class SceneRuntimeController;

class SceneWallpaper : NoCopy {
public:
    SceneWallpaper();
    ~SceneWallpaper();
    bool init();
    bool inited() const;

    void initVulkan(RenderInitInfo);

    void play();
    void play(uint32_t fade_ms);
    void pause();
    void pause(uint32_t fade_ms);
    void requestFrame();
    void mouseInput(double x, double y);
    // button: 0=left, 1=right, 2=middle (GLFW numbering). down=true on
    // press, false on release.
    void mouseButton(int button, bool down);
    void mouseEnter(bool in_window);

    void configure(SceneWallpaperConfig);
    void setFps(uint32_t);
    void setVolume(float);
    void setVolumeScale(float);
    void setVolumeScale(float, uint32_t fade_ms);
    void setMuted(bool);
    void setFillMode(FillMode);
    void setSpeed(float);
    void setMediaStatus(MediaStatus);
    void setAudioSpectrum(std::array<float, 64>, std::array<float, 64>);
    void setUserPropertyRaw(std::string_view, std::string);
    void setUserPropertyJson(std::string_view, Json);
    void setOnFirstFrame(FirstFrameCallback);
    void setOnUserPropertyDiagnostics(UserPropertyDiagnosticCallback);
    void requestPreparedPassDiagnostics(RenderPassDiagnosticCallback);

    // Install (or clear, with `nullptr`) a callback invoked on the
    // main thread after each scene is parsed, carrying the scene's
    // `general.clearcolor`. Set once before initVulkan.
    void setOnClearColor(ClearColorCallback);
    void setOnAudioDemand(AudioDemandCallback);

    ExSwapchain* exSwapchain() const;

    bool waitVulkanInited(uint32_t timeout_ms);

    // Returns true once the render controller has received and stored a scene
    // (i.e. loadScene → RenderSetScene round-trip is complete).
    bool sceneReady() const;

    VkInstance       vkInstance() const;
    VkPhysicalDevice vkPhysicalDevice() const;
    VkDevice         vkDevice() const;
    VkQueue          vkGraphicsQueue() const;
    uint32_t         vkGraphicsQueueFamily() const;

    void deviceUuid(uint8_t out[16]) const;
    void driverUuid(uint8_t out[16]) const;

private:
    bool m_inited { false };

private:
    friend class SceneRuntimeController;

    bool                                    m_offscreen { false };
    std::unique_ptr<SceneRuntimeController> m_runtime;
};

} // namespace sr

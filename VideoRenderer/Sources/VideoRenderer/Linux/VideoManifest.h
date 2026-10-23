#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <exception>
#include <filesystem>

struct VideoRendererManifestError: std::exception {
    int code;
    std::string userInfo;
    VideoRendererManifestError(int code, std::string description);
    [[nodiscard]] const char * what() const noexcept override;
};

class VRVideoManifest {
public:
    [[nodiscard]] static std::shared_ptr<VRVideoManifest> loadFromDirectory(const std::string& dir);

    [[nodiscard]] const std::filesystem::path& wallpaperDirectory() const noexcept {
        return m_wallpaperDirectory;
    }
    [[nodiscard]] const std::string& title() const noexcept { return m_title; }
    [[nodiscard]] const std::string& preview() const noexcept { return m_preview; }
    [[nodiscard]] const std::string& videoFile() const noexcept { return m_videoFile; }
    [[nodiscard]] const std::filesystem::path& videoPath() const noexcept { return m_videoPath; }
    [[nodiscard]] const nlohmann::json& userProperties() const noexcept {
        return m_userProperties;
    }

private:
    std::shared_ptr<VRVideoManifest> initWithDirectory(const std::filesystem::path&directory,
                                                       const std::string &title, const std::string &preview,
                                                       const std::string &videoFile,
                                                       const std::filesystem::path &videoPath, const nlohmann::json &userProperties);

    std::filesystem::path m_wallpaperDirectory;
    std::string m_title;
    std::string m_preview;
    std::string m_videoFile;
    std::filesystem::path m_videoPath;
    nlohmann::json m_userProperties;
};

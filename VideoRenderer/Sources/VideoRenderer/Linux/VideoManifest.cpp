#include "VideoManifest.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

enum {
    VRManifestErrorOpenFailed = 1,
    VRManifestErrorInvalidJSON,
    VRManifestErrorWrongType,
    VRManifestErrorMissingVideo,
    VRManifestErrorUnsafePath,
};

const std::unordered_set<std::string> VRVideoExtensions = {
    ".mp4", ".mov", ".m4v", ".avi", ".mkv", ".webm"
};

// VideoRendererManifestError Begin
VideoRendererManifestError::VideoRendererManifestError(int code, std::string description) : code(code),
    userInfo(std::move(description)) {
}

const char *VideoRendererManifestError::what() const noexcept {
    return userInfo.c_str();
}

// VideoRendererManifestError End

// Resolve a manifest-supplied relative path inside `directory`, or nil if it
// escapes. project.json is untrusted Workshop content, so a "file" entry of
// "../../../../Users/you/Movies/private.mov" must not end up playing on the
// desktop. Same containment pattern as WRURLSchemeHandler
// -safePathForRelative:inDirectory:: canonicalize BOTH sides (standardize and
// resolve symlinks, since a symlinked entry inside the directory can point
// anywhere) and require equality with the root or a "<root>/" prefix.
static fs::path VRContainedPath(std::string relative, const fs::path &directory) {
    if (relative.empty()) return {};
    while (relative.starts_with('/')) relative = relative.substr(1);
    if (relative.empty()) return {};
    const auto base = fs::weakly_canonical(fs::is_symlink(directory) ? fs::read_symlink(directory) : directory);
    const auto combined = fs::weakly_canonical(base / fs::path(relative));
    auto standardised = fs::is_symlink(combined) ? fs::read_symlink(combined) : combined;
    if (standardised != base && !standardised.string().starts_with(base.string() + "/")) return {};
    return standardised;
}

static std::string VRFindFirstVideoFile(const fs::path &directory) {
    std::string videoFileName;
    std::ranges::for_each(fs::directory_iterator{directory}, [&videoFileName](const auto &dir_entry) {
        if (const auto &fullPath = dir_entry.path(); fs::exists(fullPath) && fs::is_regular_file(fullPath)) {
            if (VRVideoExtensions.contains(fullPath.extension().string())) {
                videoFileName = fullPath.filename().string();
            }
        }
    });
    return videoFileName;
}

static json VRManifestUserProperties(const json &json_data) {
    if (!json_data["general"].is_object()) return {};
    auto properties = json_data["general"]["properties"];
    return properties.is_object() ? properties : json{};
}

std::shared_ptr<VRVideoManifest> VRVideoManifest::loadFromDirectory(const std::string &dir) {
    fs::path directory;
    try {
        directory = fs::canonical(fs::path(dir)); // This means standardised path.
    } catch ([[maybe_unused]] const fs::filesystem_error &e) {
        throw VideoRendererManifestError(VRManifestErrorOpenFailed,
                                         std::format("wallpaper directory not found: {}", directory.c_str()));
    }

    auto const projectPath = directory / "project.json";
    auto parsed = json();
    {
        std::ifstream jsonFile(projectPath);
        if (!jsonFile.is_open())
            throw VideoRendererManifestError(VRManifestErrorOpenFailed,
                                             std::format("cannot open {}", projectPath.c_str()));

        try {
            parsed = json::parse(jsonFile);
        } catch (const json::parse_error &e) {
            throw VideoRendererManifestError(VRManifestErrorInvalidJSON,
                                             std::format("invalid project.json: {} with error {}", projectPath.c_str(),
                                                         e.what()));
        }

        try {
            std::string type = parsed["type"].get<std::string>();
            std::ranges::transform(type, type.begin(), ::tolower);
            if (type != "video")
                throw VideoRendererManifestError(VRManifestErrorWrongType,
                                                 std::format("project.json type is '{}', expected 'video'",
                                                             type.empty() ? type.c_str() : "<missing>"));
            std::string videoFile = parsed["file"].get<std::string>();
            if (videoFile.empty()) videoFile = VRFindFirstVideoFile(directory);
            if (videoFile.empty())
                throw VideoRendererManifestError(VRManifestErrorMissingVideo,
                                                 "video wallpaper has no playable file entry");

            const auto videoPath = VRContainedPath(videoFile, directory);
            if (videoPath.empty())
                throw VideoRendererManifestError(VRManifestErrorUnsafePath,
                                                 "project.json 'file' entry escapes the wallpaper directory");
            if (!fs::exists(videoPath) || fs::is_directory(videoPath))
                throw VideoRendererManifestError(VRManifestErrorMissingVideo,
                                                 std::format("video file not found in wallpaper directory: {}",
                                                             videoFile));

            auto title = parsed["title"].get<std::string>();
            if (title.empty()) title = directory.stem();

            auto preview = parsed["preview"].get<std::string>();
            auto ptr = new VRVideoManifest();
            return ptr->initWithDirectory(directory, title, preview, videoFile, videoPath, VRManifestUserProperties(parsed));
        } catch (const json::out_of_range &e) {
            throw VideoRendererManifestError(VRManifestErrorInvalidJSON,
                                             std::format("invalid project.json: {} with error {}", projectPath.c_str(),
                                                         e.what()));
        }
    }
}

std::shared_ptr<VRVideoManifest> VRVideoManifest::initWithDirectory(const fs::path &directory,
    const std::string &title, const std::string &preview, const std::string &videoFile,
    const std::filesystem::path &videoPath, const json &userProperties) {
    this->m_wallpaperDirectory = directory;
    this->m_title = title;
    this->m_preview = preview;
    this->m_videoFile = videoFile;
    this->m_videoPath = videoPath;
    this->m_userProperties = userProperties;
    return std::shared_ptr<VRVideoManifest>(this);
}

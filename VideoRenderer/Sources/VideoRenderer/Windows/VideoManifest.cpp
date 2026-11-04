#include "VideoManifest.h"

#include <QString>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
namespace {
std::string utf8(const fs::path& path) {
    return QString::fromStdWString(path.native()).toStdString();
}
fs::path nativePath(const std::string& path) {
    return fs::path(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())).toStdWString());
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}
fs::path containedPath(const fs::path& root, const std::string& file) {
    const fs::path relative = nativePath(file);
    if (relative.empty() || relative.has_root_path() || file.find(':') != std::string::npos) return {};
    const fs::path resolved = fs::weakly_canonical(root / relative);
    const QString base = QString::fromStdWString(root.native());
    const QString full = QString::fromStdWString(resolved.native());
    if (!full.startsWith(base + QLatin1Char('\\'), Qt::CaseInsensitive)) return {};
    return resolved;
}
std::string firstVideo(const fs::path& directory) {
    static const std::unordered_set<std::string> extensions = {".mp4", ".mov", ".m4v", ".avi", ".mkv", ".webm"};
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && extensions.contains(lower(utf8(entry.path().extension()))))
            files.push_back(entry.path().filename());
    }
    std::sort(files.begin(), files.end());
    return files.empty() ? std::string{} : utf8(files.front());
}
}

VideoRendererManifestError::VideoRendererManifestError(int value, std::string description)
    : code(value), userInfo(std::move(description)) {}
const char* VideoRendererManifestError::what() const noexcept { return userInfo.c_str(); }

std::shared_ptr<VRVideoManifest> VRVideoManifest::loadFromDirectory(const std::string& dir) {
    try {
        const fs::path directory = fs::canonical(nativePath(dir));
        std::ifstream input(directory / L"project.json");
        if (!input) throw VideoRendererManifestError(1, "cannot open project.json in " + dir);
        const auto project = nlohmann::json::parse(input);
        if (!project.is_object()) throw VideoRendererManifestError(2, "project.json must be an object");
        const std::string type = lower(project.value("type", std::string{}));
        if (type != "video") throw VideoRendererManifestError(3, "project.json type must be 'video'");
        std::string file = project.value("file", std::string{});
        if (file.empty()) file = firstVideo(directory);
        if (file.empty()) throw VideoRendererManifestError(4, "video wallpaper has no playable file entry");
        const fs::path path = containedPath(directory, file);
        if (path.empty()) throw VideoRendererManifestError(5, "project.json 'file' entry escapes the wallpaper directory");
        if (!fs::is_regular_file(path)) throw VideoRendererManifestError(4, "video file not found: " + file);
        auto manifest = std::make_shared<VRVideoManifest>();
        manifest->m_wallpaperDirectory = directory;
        manifest->m_videoPath = path;
        manifest->m_videoFile = file;
        manifest->m_title = project.value("title", utf8(directory.filename()));
        if (manifest->m_title.empty()) manifest->m_title = utf8(directory.filename());
        manifest->m_preview = project.value("preview", std::string{});
        const auto general = project.find("general");
        if (general != project.end() && general->is_object()) {
            const auto properties = general->find("properties");
            if (properties != general->end() && properties->is_object()) manifest->m_userProperties = *properties;
        }
        return manifest;
    } catch (const nlohmann::json::exception& error) {
        throw VideoRendererManifestError(2, std::string("invalid project.json: ") + error.what());
    } catch (const fs::filesystem_error& error) {
        throw VideoRendererManifestError(1, std::string("cannot resolve video wallpaper: ") + error.what());
    }
}

std::shared_ptr<VRVideoManifest> VRVideoManifest::loadFromPath(const std::string& path) {
    std::error_code error;
    const fs::path file = fs::canonical(nativePath(path), error);
    if (error || !fs::is_regular_file(file, error)) return loadFromDirectory(path);
    auto manifest = std::make_shared<VRVideoManifest>();
    manifest->m_videoPath = file;
    manifest->m_wallpaperDirectory = file.parent_path();
    manifest->m_videoFile = utf8(file.filename());
    manifest->m_title = utf8(file.stem());
    return manifest;
}

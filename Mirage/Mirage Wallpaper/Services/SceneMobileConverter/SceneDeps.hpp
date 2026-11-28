//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_SCENE_DEPS_HPP
#define MIRAGE_SCENE_MOBILE_SCENE_DEPS_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mirage::mobile {

struct CollectedFile {
    std::string rel_path;
    std::filesystem::path full_path;
};

// Collects loose scene project files using Wallpaper Engine's dependency-graph crawler
// (`scene.json` -> models/materials/shaders/#include/particles/effects/sounds/fonts +
// `engine.registerAsset(...)`), enforcing strict realpath containment inside `project_dir`.
// Falls back to all non-editor runtime files when `include_all_non_editor` is true or
// the scene file cannot be crawled.
std::vector<CollectedFile> CollectLooseProjectFiles(
    const std::filesystem::path& project_dir,
    std::string_view scene_file_rel,
    bool include_all_non_editor = false);

std::vector<CollectedFile> CrawlSceneDependencies(
    const std::filesystem::path& project_dir,
    std::string_view scene_rel);

} // namespace mirage::mobile

#endif /* MIRAGE_SCENE_MOBILE_SCENE_DEPS_HPP */

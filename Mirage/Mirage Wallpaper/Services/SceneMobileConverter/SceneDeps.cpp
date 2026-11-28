//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "SceneDeps.hpp"
#include "JsonCompat.hpp"
#include "MobileInternal.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

namespace mirage::mobile {

namespace {

constexpr size_t kMaxWalkFiles = 50000;
constexpr size_t kMaxCrawlDepth = 64;

std::vector<CollectedFile> WalkDirSafe(const std::filesystem::path& root) {
    auto canon_root = CanonicalPath(root);
    if (!canon_root) return {};
    std::string root_prefix = canon_root->string();
    if (!root_prefix.empty() && root_prefix.back() != '/') root_prefix.push_back('/');

    std::vector<CollectedFile> out;
    std::error_code ec;
    auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(*canon_root, options, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (out.size() >= kMaxWalkFiles) break;
        if (it.depth() > static_cast<int>(kMaxCrawlDepth)) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code st_ec;
        auto status = it->symlink_status(st_ec);
        if (st_ec || std::filesystem::is_symlink(status)) {
            // Never follow symlinks inside untrusted Workshop packages.
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) continue;

        auto canon_file = CanonicalPath(it->path());
        if (!canon_file) continue;
        std::string fstr = canon_file->string();
        if (fstr.size() <= root_prefix.size() ||
            fstr.compare(0, root_prefix.size(), root_prefix) != 0) {
            continue;
        }
        std::string rel = fstr.substr(root_prefix.size());
        auto safe_rel = NormalizeSafeArchivePath(rel);
        if (!safe_rel) continue;
        out.push_back(CollectedFile{std::move(*safe_rel), std::move(*canon_file)});
    }
    std::sort(out.begin(), out.end(), [](const CollectedFile& a, const CollectedFile& b) {
        return a.rel_path < b.rel_path;
    });
    return out;
}

const std::regex& ReInclude() {
    static const std::regex re(R"((?:^|\n)[ \t]*#include[ \t]+["<]([^">]+)[">])");
    return re;
}

const std::regex& ReRegisterAsset() {
    static const std::regex re(R"(engine\.registerAsset[\s]*\([\s]*\\?["']([\s\w\d\.\\/]+)\\?["'])");
    return re;
}

const std::regex& ReMdlMaterial() {
    static const std::regex re(R"(materials/[A-Za-z0-9_\-./ ]+\.json)");
    return re;
}

bool IsExcludedEditorFile(std::string_view lower) {
    return EndsWithIgnoreCase(lower, ".pkg") ||
           EndsWithIgnoreCase(lower, ".mpkg") ||
           EndsWithIgnoreCase(lower, ".tex-json") ||
           EndsWithIgnoreCase(lower, ".dxs") ||
           StartsWithIgnoreCase(lower, "shaders/blobssm");
}

class SceneDepCrawler {
public:
    explicit SceneDepCrawler(const std::filesystem::path& root) {
        auto all = WalkDirSafe(root);
        for (auto& f : all) {
            index_[AsciiLower(f.rel_path)] = std::move(f);
        }
    }

    void CrawlScene(std::string_view scene_rel) {
        auto norm = NormalizeRelativeAssetRef(scene_rel);
        if (!norm) return;
        if (!visited_scenes_.insert(AsciiLower(*norm)).second) return;
        auto added = AddFile(*norm);
        if (!added) return;
        auto doc = ReadJsonFile(added->full_path);
        if (!doc || !doc->is_object()) return;

        if (auto cam = doc->find("camera"); cam != doc->end() && cam->is_object()) {
            if (auto paths = cam->find("paths"); paths != cam->end() && paths->is_array()) {
                for (const auto& p : *paths) {
                    if (p.is_string()) {
                        AddFile(p.get_ref<const std::string&>());
                    } else if (p.is_object()) {
                        if (auto f = p.find("file"); f != p.end() && f->is_string()) {
                            AddFile(f->get_ref<const std::string&>());
                        }
                    }
                }
            }
        }

        if (auto objects = doc->find("objects"); objects != doc->end() && objects->is_array()) {
            for (const auto& obj : *objects) {
                if (obj.is_object()) CrawlSceneObject(obj);
            }
        }
    }

    void CrawlRegisteredAssets() {
        std::set<std::string> scanned_jsons;
        for (size_t pass = 0; pass < 16; ++pass) {
            std::vector<CollectedFile> pending;
            for (const auto& [rel, full] : collected_) {
                std::string lower = AsciiLower(rel);
                if (EndsWithIgnoreCase(lower, ".json") && scanned_jsons.count(lower) == 0) {
                    pending.push_back(CollectedFile{rel, full});
                }
            }
            if (pending.empty()) break;

            std::vector<std::string> discovered;
            for (const auto& item : pending) {
                scanned_jsons.insert(AsciiLower(item.rel_path));
                std::error_code ec;
                auto sz = std::filesystem::file_size(item.full_path, ec);
                if (ec || sz > 32ULL * 1024ULL * 1024ULL) continue;
                std::ifstream ifs(item.full_path, std::ios::binary);
                if (!ifs) continue;
                std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                for (std::sregex_iterator it(text.begin(), text.end(), ReRegisterAsset()), end; it != end; ++it) {
                    if (it->size() > 1) {
                        if (auto ref = NormalizeRelativeAssetRef((*it)[1].str())) {
                            discovered.push_back(std::move(*ref));
                        }
                    }
                }
            }
            if (discovered.empty()) break;
            for (const auto& asset : discovered) {
                CrawlGenericAsset(asset, 0);
            }
        }
    }

    std::vector<CollectedFile> Finish() {
        std::vector<CollectedFile> out;
        out.reserve(collected_.size());
        for (auto& [rel, full] : collected_) {
            out.push_back(CollectedFile{rel, full});
        }
        return out;
    }

private:
    std::optional<CollectedFile> ResolveExisting(std::string_view rel) const {
        auto norm = NormalizeRelativeAssetRef(rel);
        if (!norm) return std::nullopt;
        auto it = index_.find(AsciiLower(*norm));
        if (it == index_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<CollectedFile> AddFile(std::string_view rel) {
        auto found = ResolveExisting(rel);
        if (!found) return std::nullopt;
        std::string lower = AsciiLower(found->rel_path);
        if (IsExcludedEditorFile(lower)) return std::nullopt;
        collected_[found->rel_path] = found->full_path;
        return found;
    }

    std::optional<Json> ReadJsonFile(const std::filesystem::path& p) const {
        std::error_code ec;
        auto sz = std::filesystem::file_size(p, ec);
        if (ec || sz == 0 || sz > 32ULL * 1024ULL * 1024ULL) return std::nullopt;
        try {
            auto bytes = ReadFileBytes(p, 32ULL * 1024ULL * 1024ULL);
            return ParseJsonLenient(bytes);
        } catch (...) {
            return std::nullopt;
        }
    }

    void CrawlSceneObject(const Json& obj) {
        auto get_str = [&](const char* key) -> std::optional<std::string_view> {
            auto it = obj.find(key);
            if (it != obj.end() && it->is_string()) {
                return std::string_view(it->get_ref<const std::string&>());
            }
            return std::nullopt;
        };

        if (auto s = get_str("model")) CrawlModel(*s, 0);
        if (auto s = get_str("skin")) { CrawlMaterial(*s, 0); AddFile(*s); }
        if (auto s = get_str("particle")) CrawlParticle(*s, 0);

        if (auto it = obj.find("dependencies"); it != obj.end() && it->is_array()) {
            for (const auto& d : *it) {
                if (d.is_string()) CrawlGenericAsset(d.get_ref<const std::string&>(), 0);
            }
        }
        if (auto s = get_str("mask")) CrawlTexture(*s);
        if (auto s = get_str("sprite")) CrawlMaterial(*s, 0);
        if (auto s = get_str("image")) CrawlImageLayer(*s, 0);

        if (auto it = obj.find("instance"); it != obj.end()) {
            if (it->is_string()) {
                CrawlGenericAsset(it->get_ref<const std::string&>(), 0);
            } else if (it->is_object()) {
                CrawlPassLike(*it, 0);
            }
        }

        if (auto it = obj.find("sound"); it != obj.end()) {
            if (it->is_array()) {
                for (const auto& s : *it) {
                    if (s.is_string()) CrawlPrefixedFile("sounds", s.get_ref<const std::string&>());
                }
            } else if (it->is_string()) {
                CrawlPrefixedFile("sounds", it->get_ref<const std::string&>());
            }
        }

        if (auto s = get_str("font")) CrawlPrefixedFile("fonts", *s);
        if (auto it = obj.find("text"); it != obj.end() && it->is_object()) {
            if (auto f = it->find("font"); f != it->end() && f->is_string()) {
                CrawlPrefixedFile("fonts", f->get_ref<const std::string&>());
            }
        }

        if (auto s = get_str("cookie")) CrawlTexture(*s);
        if (auto it = obj.find("light"); it != obj.end() && it->is_object()) {
            if (auto c = it->find("cookie"); c != it->end() && c->is_string()) {
                CrawlTexture(c->get_ref<const std::string&>());
            }
        }

        if (auto s = get_str("path")) AddFile(*s);

        if (auto it = obj.find("effects"); it != obj.end() && it->is_array()) {
            for (const auto& eff : *it) {
                if (!eff.is_object()) continue;
                if (auto ef = eff.find("file"); ef != eff.end() && ef->is_string()) {
                    CrawlEffect(ef->get_ref<const std::string&>(), 0);
                }
                if (auto passes = eff.find("passes"); passes != eff.end() && passes->is_array()) {
                    for (const auto& pass : *passes) {
                        if (pass.is_object()) CrawlPassLike(pass, 0);
                    }
                }
            }
        }

        if (auto s = get_str("material")) CrawlMaterial(*s, 0);
    }

    void CrawlImageLayer(std::string_view rel, size_t depth) {
        if (depth > kMaxCrawlDepth) return;
        auto norm = NormalizeRelativeAssetRef(rel);
        if (!norm || !visited_images_.insert(AsciiLower(*norm)).second) return;
        auto added = AddFile(*norm);
        if (!added) return;
        auto doc = ReadJsonFile(added->full_path);
        if (!doc || !doc->is_object()) return;
        if (auto it = doc->find("material"); it != doc->end() && it->is_string()) {
            CrawlMaterial(it->get_ref<const std::string&>(), depth + 1);
        }
        if (auto it = doc->find("puppet"); it != doc->end() && it->is_string()) {
            CrawlModel(it->get_ref<const std::string&>(), depth + 1);
        }
        if (auto it = doc->find("passes"); it != doc->end() && it->is_array()) {
            for (const auto& p : *it) {
                if (p.is_object()) CrawlPassLike(p, depth + 1);
            }
        }
    }

    void CrawlModel(std::string_view rel, size_t depth) {
        if (depth > kMaxCrawlDepth) return;
        auto norm = NormalizeRelativeAssetRef(rel);
        if (!norm || !visited_models_.insert(AsciiLower(*norm)).second) return;
        auto added = AddFile(*norm);
        if (!added) return;

        if (EndsWithIgnoreCase(added->rel_path, ".json")) {
            if (auto doc = ReadJsonFile(added->full_path); doc && doc->is_object()) {
                if (auto it = doc->find("material"); it != doc->end() && it->is_string()) {
                    CrawlMaterial(it->get_ref<const std::string&>(), depth + 1);
                }
                if (auto it = doc->find("puppet"); it != doc->end() && it->is_string()) {
                    CrawlModel(it->get_ref<const std::string&>(), depth + 1);
                }
                if (auto it = doc->find("mdl"); it != doc->end() && it->is_string()) {
                    CrawlModel(it->get_ref<const std::string&>(), depth + 1);
                }
                if (auto it = doc->find("file"); it != doc->end() && it->is_string()) {
                    CrawlModel(it->get_ref<const std::string&>(), depth + 1);
                }
            }
            if (added->rel_path.size() > 5) {
                std::string stem = added->rel_path.substr(0, added->rel_path.size() - 5);
                AddFile(stem + ".mdl");
            }
        } else if (EndsWithIgnoreCase(added->rel_path, ".mdl")) {
            std::error_code ec;
            auto sz = std::filesystem::file_size(added->full_path, ec);
            if (!ec && sz > 0 && sz <= 32ULL * 1024ULL * 1024ULL) {
                std::ifstream ifs(added->full_path, std::ios::binary);
                if (ifs) {
                    std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                    for (std::sregex_iterator it(raw.begin(), raw.end(), ReMdlMaterial()), end; it != end; ++it) {
                        CrawlMaterial(it->str(), depth + 1);
                    }
                }
            }
        }
    }

    void CrawlParticle(std::string_view rel, size_t depth) {
        if (depth > kMaxCrawlDepth) return;
        auto norm = NormalizeRelativeAssetRef(rel);
        if (!norm || !visited_particles_.insert(AsciiLower(*norm)).second) return;
        auto added = AddFile(*norm);
        if (!added) return;
        auto doc = ReadJsonFile(added->full_path);
        if (!doc || !doc->is_object()) return;
        if (auto it = doc->find("material"); it != doc->end() && it->is_string()) {
            CrawlMaterial(it->get_ref<const std::string&>(), depth + 1);
        }
        if (auto it = doc->find("children"); it != doc->end() && it->is_array()) {
            for (const auto& ch : *it) {
                if (ch.is_object()) {
                    if (auto n = ch.find("name"); n != ch.end() && n->is_string()) {
                        CrawlParticle(n->get_ref<const std::string&>(), depth + 1);
                    }
                }
            }
        }
    }

    void CrawlEffect(std::string_view rel, size_t depth) {
        if (depth > kMaxCrawlDepth) return;
        auto norm = NormalizeRelativeAssetRef(rel);
        if (!norm || !visited_effects_.insert(AsciiLower(*norm)).second) return;
        auto added = AddFile(*norm);
        if (!added) return;
        auto doc = ReadJsonFile(added->full_path);
        if (!doc || !doc->is_object()) return;
        if (auto it = doc->find("passes"); it != doc->end() && it->is_array()) {
            for (const auto& p : *it) {
                if (p.is_object()) CrawlPassLike(p, depth + 1);
            }
        }
    }

    void CrawlMaterial(std::string_view rel, size_t depth) {
        if (depth > kMaxCrawlDepth) return;
        auto norm = NormalizeRelativeAssetRef(rel);
        if (!norm) return;
        std::vector<std::string> candidates;
        candidates.push_back(*norm);
        if (EndsWithIgnoreCase(*norm, ".json")) {
            candidates.push_back("materials/" + *norm);
        } else {
            candidates.push_back(*norm + ".json");
        }
        candidates.push_back("materials/" + *norm + ".json");

        std::optional<std::string> resolved;
        for (const auto& c : candidates) {
            if (ResolveExisting(c)) {
                resolved = c;
                break;
            }
        }
        if (!resolved || !visited_materials_.insert(AsciiLower(*resolved)).second) return;
        auto added = AddFile(*resolved);
        if (!added) return;
        auto doc = ReadJsonFile(added->full_path);
        if (!doc || !doc->is_object()) return;
        if (auto it = doc->find("passes"); it != doc->end() && it->is_array()) {
            for (const auto& p : *it) {
                if (p.is_object()) CrawlPassLike(p, depth + 1);
            }
        }
        if (auto it = doc->find("assets"); it != doc->end() && it->is_array()) {
            for (const auto& a : *it) {
                if (a.is_string()) CrawlGenericAsset(a.get_ref<const std::string&>(), depth + 1);
            }
        }
    }

    void CrawlPassLike(const Json& pass, size_t depth) {
        if (depth > kMaxCrawlDepth) return;
        if (auto it = pass.find("material"); it != pass.end() && it->is_string()) {
            CrawlMaterial(it->get_ref<const std::string&>(), depth + 1);
        }
        if (auto it = pass.find("shader"); it != pass.end() && it->is_string()) {
            CrawlShader(it->get_ref<const std::string&>(), depth + 1);
        }
        if (auto it = pass.find("textures"); it != pass.end() && it->is_array()) {
            for (const auto& t : *it) {
                if (t.is_string()) CrawlTexture(t.get_ref<const std::string&>());
            }
        }
        if (auto it = pass.find("assets"); it != pass.end() && it->is_array()) {
            for (const auto& a : *it) {
                if (a.is_string()) CrawlGenericAsset(a.get_ref<const std::string&>(), depth + 1);
            }
        }
    }

    void CrawlTexture(std::string_view tex_ref) {
        auto norm = NormalizeRelativeAssetRef(tex_ref);
        if (!norm || StartsWithIgnoreCase(*norm, "_rt_")) return;
        std::vector<std::string> candidates;
        if (EndsWithIgnoreCase(*norm, ".tex")) {
            if (StartsWithIgnoreCase(*norm, "materials/")) {
                candidates.push_back(*norm);
            } else {
                candidates.push_back(*norm);
                candidates.push_back("materials/" + *norm);
            }
        } else if (StartsWithIgnoreCase(*norm, "materials/")) {
            candidates.push_back(*norm + ".tex");
            candidates.push_back(*norm);
        } else {
            candidates.push_back("materials/" + *norm + ".tex");
            candidates.push_back(*norm + ".tex");
            candidates.push_back("materials/" + *norm);
            candidates.push_back(*norm);
        }
        for (const auto& c : candidates) {
            if (AddFile(c)) break;
        }
    }

    void CrawlShader(std::string_view shader_ref, size_t depth) {
        if (depth > kMaxCrawlDepth) return;
        auto norm = NormalizeRelativeAssetRef(shader_ref);
        if (!norm) return;
        std::string_view base = *norm;
        if (StartsWithIgnoreCase(base, "shaders/")) base.remove_prefix(8);
        std::string stem(base);
        for (std::string_view ext : {".vert", ".frag", ".geom"}) {
            if (EndsWithIgnoreCase(stem, ext)) {
                stem.resize(stem.size() - ext.size());
                break;
            }
        }
        for (const char* ext : {"vert", "frag", "geom"}) {
            CrawlShaderFile("shaders/" + stem + "." + ext, depth + 1);
        }
        CrawlShaderFile(*norm, depth + 1);
    }

    void CrawlShaderFile(std::string_view rel, size_t depth) {
        if (depth > kMaxCrawlDepth) return;
        auto norm = NormalizeRelativeAssetRef(rel);
        if (!norm || !visited_shaders_.insert(AsciiLower(*norm)).second) return;
        auto added = AddFile(*norm);
        if (!added) return;
        std::error_code ec;
        auto sz = std::filesystem::file_size(added->full_path, ec);
        if (ec || sz == 0 || sz > 8ULL * 1024ULL * 1024ULL) return;
        std::ifstream ifs(added->full_path, std::ios::binary);
        if (!ifs) return;
        std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        std::string parent_dir;
        if (size_t slash = added->rel_path.rfind('/'); slash != std::string::npos) {
            parent_dir = added->rel_path.substr(0, slash);
        }

        for (std::sregex_iterator it(text.begin(), text.end(), ReInclude()), end; it != end; ++it) {
            if (it->size() <= 1) continue;
            auto inc_norm = NormalizeRelativeAssetRef((*it)[1].str());
            if (!inc_norm) continue;
            std::vector<std::string> candidates;
            if (!parent_dir.empty()) candidates.push_back(parent_dir + "/" + *inc_norm);
            candidates.push_back("shaders/" + *inc_norm);
            candidates.push_back(*inc_norm);
            for (const auto& c : candidates) {
                if (ResolveExisting(c)) {
                    CrawlShaderFile(c, depth + 1);
                    break;
                }
            }
        }
    }

    void CrawlPrefixedFile(std::string_view folder, std::string_view asset_ref) {
        auto norm = NormalizeRelativeAssetRef(asset_ref);
        if (!norm) return;
        std::string prefix = std::string(folder) + "/";
        if (!AddFile(*norm) && !StartsWithIgnoreCase(*norm, prefix)) {
            AddFile(prefix + *norm);
        }
    }

    void CrawlGenericAsset(std::string_view raw, size_t depth) {
        if (depth > kMaxCrawlDepth) return;
        auto norm = NormalizeRelativeAssetRef(raw);
        if (!norm) return;
        if (StartsWithIgnoreCase(*norm, "models/")) {
            if (EndsWithIgnoreCase(*norm, ".mdl")) {
                CrawlModel(*norm, depth + 1);
            } else {
                CrawlImageLayer(*norm, depth + 1);
                CrawlModel(*norm, depth + 1);
            }
        } else if (StartsWithIgnoreCase(*norm, "materials/")) {
            if (EndsWithIgnoreCase(*norm, ".json")) {
                CrawlMaterial(*norm, depth + 1);
            } else {
                CrawlTexture(*norm);
            }
        } else if (StartsWithIgnoreCase(*norm, "particles/")) {
            CrawlParticle(*norm, depth + 1);
        } else if (StartsWithIgnoreCase(*norm, "effects/")) {
            CrawlEffect(*norm, depth + 1);
        } else if (StartsWithIgnoreCase(*norm, "shaders/")) {
            CrawlShader(*norm, depth + 1);
        } else if (StartsWithIgnoreCase(*norm, "sounds/")) {
            CrawlPrefixedFile("sounds", *norm);
        } else if (StartsWithIgnoreCase(*norm, "fonts/")) {
            CrawlPrefixedFile("fonts", *norm);
        } else {
            AddFile(*norm);
        }
    }

    std::map<std::string, CollectedFile> index_;
    std::map<std::string, std::filesystem::path> collected_;
    std::set<std::string> visited_materials_;
    std::set<std::string> visited_models_;
    std::set<std::string> visited_particles_;
    std::set<std::string> visited_images_;
    std::set<std::string> visited_effects_;
    std::set<std::string> visited_shaders_;
    std::set<std::string> visited_scenes_;
};

std::vector<CollectedFile> CollectAllNonEditorFiles(const std::filesystem::path& project_dir) {
    auto all = WalkDirSafe(project_dir);
    std::set<std::string> compiled_tex_stems;
    for (const auto& f : all) {
        std::string lower = AsciiLower(f.rel_path);
        if (EndsWithIgnoreCase(lower, ".tex") && lower.size() > 4) {
            compiled_tex_stems.insert(lower.substr(0, lower.size() - 4));
        }
    }

    std::map<std::string, std::filesystem::path> map;
    for (const auto& f : all) {
        // Skip hidden files/directories.
        bool hidden = false;
        size_t pos = 0;
        while (pos < f.rel_path.size()) {
            if (f.rel_path[pos] == '.') { hidden = true; break; }
            size_t slash = f.rel_path.find('/', pos);
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        if (hidden) continue;

        std::string lower = AsciiLower(f.rel_path);
        if (IsExcludedEditorFile(lower) ||
            EndsWithIgnoreCase(lower, ".psd") ||
            EndsWithIgnoreCase(lower, ".xcf") ||
            EndsWithIgnoreCase(lower, ".blend") ||
            EndsWithIgnoreCase(lower, ".fbx") ||
            EndsWithIgnoreCase(lower, ".3ds") ||
            EndsWithIgnoreCase(lower, ".dae") ||
            EndsWithIgnoreCase(lower, ".obj")) {
            continue;
        }
        if (lower.find('/') == std::string::npos && StartsWithIgnoreCase(lower, "preview.")) {
            continue;
        }
        if (StartsWithIgnoreCase(lower, "materials/")) {
            if (size_t dot = lower.rfind('.'); dot != std::string::npos) {
                std::string_view ext = std::string_view(lower).substr(dot + 1);
                bool raw_img = (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tga" ||
                                ext == "bmp" || ext == "gif" || ext == "webp" || ext == "tif" || ext == "tiff");
                if (raw_img && compiled_tex_stems.count(lower.substr(0, dot)) > 0) {
                    continue;
                }
            }
        }
        map[f.rel_path] = f.full_path;
    }

    std::vector<CollectedFile> out;
    out.reserve(map.size());
    for (auto& [rel, full] : map) {
        out.push_back(CollectedFile{rel, full});
    }
    return out;
}

} // namespace

std::vector<CollectedFile> CrawlSceneDependencies(
    const std::filesystem::path& project_dir,
    std::string_view scene_rel) {
    SceneDepCrawler crawler(project_dir);
    crawler.CrawlScene(scene_rel);
    crawler.CrawlRegisteredAssets();
    return crawler.Finish();
}

std::vector<CollectedFile> CollectLooseProjectFiles(
    const std::filesystem::path& project_dir,
    std::string_view scene_file_rel,
    bool include_all_non_editor) {
    if (!include_all_non_editor) {
        std::string rel = scene_file_rel.empty() ? std::string("scene.json") : std::string(scene_file_rel);
        if (ResolveContainedFile(project_dir, rel).has_value()) {
            auto crawled = CrawlSceneDependencies(project_dir, rel);
            if (!crawled.empty()) return crawled;
        }
    }
    return CollectAllNonEditorFiles(project_dir);
}

} // namespace mirage::mobile

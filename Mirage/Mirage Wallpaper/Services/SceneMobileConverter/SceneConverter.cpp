//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "SceneMobileConverter.h"
#include "JsonCompat.hpp"
#include "MobileInternal.hpp"
#include "PkgArchive.hpp"
#include "SceneDeps.hpp"
#include "ShaderRewriter.hpp"
#include "TexTranscoder.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace mirage::mobile {

namespace {

constexpr uint64_t kUhdPixelThreshold = 2075520ULL;
constexpr uint64_t kPixelArtPixelThreshold = 307200ULL;
constexpr size_t kStageToDiskThresholdBytes = 4ULL * 1024ULL * 1024ULL;
constexpr const char* kDefaultMpkgMagic = "PKGM0020";

bool IsStrippedPcPropertyKey(std::string_view key) {
    static const std::unordered_set<std::string_view> kStripped = {
        "alignment",
        "alignmentx",
        "alignmenty",
        "alignmentz",
        "alignmentposition",
        "alignmentfliph",
        "pluginledextensionsenableleds",
        "wec_e",
        "wec_brs",
        "wec_con",
        "wec_sat",
        "wec_hue",
        "rate",
    };
    return kStripped.count(AsciiLower(key)) > 0;
}

bool IsAudioEntry(std::string_view rel_path) {
    std::string lower = AsciiLower(rel_path);
    if (lower == "sounds" || StartsWithIgnoreCase(lower, "sounds/")) {
        return true;
    }
    if (size_t dot = lower.rfind('.'); dot != std::string::npos) {
        std::string_view ext = std::string_view(lower).substr(dot + 1);
        if (ext == "flac" || ext == "m4a" || ext == "mp3" || ext == "ogg" || ext == "wav") {
            return true;
        }
    }
    return false;
}

std::string ParentDirLower(std::string_view rel_path) {
    size_t slash = rel_path.rfind('/');
    if (slash == std::string_view::npos) return "";
    return AsciiLower(rel_path.substr(0, slash));
}

uint64_t ExtractWorkshopId(const Json& project_json, const std::filesystem::path& dir) {
    if (auto it = project_json.find("workshopid"); it != project_json.end()) {
        if (it->is_string()) {
            std::string trimmed = TrimAscii(it->get_ref<const std::string&>());
            if (!trimmed.empty() && std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
                try { return std::stoull(trimmed); } catch (...) {}
            }
        } else if (it->is_number_unsigned()) {
            return it->get<uint64_t>();
        }
    }
    std::string folder = dir.filename().string();
    if (!folder.empty() && std::all_of(folder.begin(), folder.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    })) {
        try { return std::stoull(folder); } catch (...) {}
    }
    return 0;
}

std::optional<uint64_t> ParseResolutionTagPixels(std::string_view tag) {
    std::string cleaned(tag);
    for (const char* prefix : {"Ultrawide ", "ultrawide ", "Dual ", "dual ",
                               "Triple ", "triple ", "Portrait ", "portrait "}) {
        size_t pos = 0;
        size_t plen = std::strlen(prefix);
        while ((pos = cleaned.find(prefix, pos)) != std::string::npos) {
            cleaned.erase(pos, plen);
        }
    }
    size_t x_pos = cleaned.find(" x ");
    if (x_pos == std::string::npos) return std::nullopt;
    std::string w_str = TrimAscii(std::string_view(cleaned).substr(0, x_pos));
    std::string h_str = TrimAscii(std::string_view(cleaned).substr(x_pos + 3));
    if (w_str.empty() || h_str.empty()) return std::nullopt;
    try {
        uint64_t w = std::stoull(w_str);
        uint64_t h = std::stoull(h_str);
        return w * h;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<uint8_t> ReadFilePrefix(const std::filesystem::path& path, size_t max_bytes) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::vector<uint8_t> buf(max_bytes);
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(max_bytes));
    buf.resize(static_cast<size_t>(std::max<std::streamsize>(0, ifs.gcount())));
    return buf;
}

std::vector<uint8_t> GenerateFallbackPreviewJpeg() {
    constexpr size_t kSize = 360;
    std::vector<uint8_t> rgba(kSize * kSize * 4);
    for (size_t y = 0; y < kSize; ++y) {
        for (size_t x = 0; x < kSize; ++x) {
            size_t idx = (y * kSize + x) * 4;
            uint8_t v = static_cast<uint8_t>(24 + ((x + y) * 20 / 720));
            rgba[idx] = v;
            rgba[idx + 1] = static_cast<uint8_t>(v + 4);
            rgba[idx + 2] = static_cast<uint8_t>(v + 12);
            rgba[idx + 3] = 255;
        }
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, rgba.data(), rgba.size(), nullptr);
    CGImageRef img = CGImageCreate(
        kSize, kSize, 8, 32, kSize * 4, cs,
        static_cast<CGBitmapInfo>(
            static_cast<uint32_t>(kCGBitmapByteOrderDefault) |
            static_cast<uint32_t>(kCGImageAlphaNoneSkipLast)),
        provider, nullptr, false, kCGRenderingIntentDefault);

    CFMutableDataRef jpeg_data = CFDataCreateMutable(kCFAllocatorDefault, 0);
    CFStringRef jpeg_type = CFSTR("public.jpeg");
    CGImageDestinationRef dest = CGImageDestinationCreateWithData(jpeg_data, jpeg_type, 1, nullptr);
    std::vector<uint8_t> out;
    if (img && dest) {
        float quality = 0.75f;
        CFNumberRef qnum = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &quality);
        const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
        const void* vals[] = {qnum};
        CFDictionaryRef props = CFDictionaryCreate(
            kCFAllocatorDefault, keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CGImageDestinationAddImage(dest, img, props);
        if (CGImageDestinationFinalize(dest)) {
            const UInt8* ptr = CFDataGetBytePtr(jpeg_data);
            CFIndex len = CFDataGetLength(jpeg_data);
            if (ptr && len > 0) out.assign(ptr, ptr + len);
        }
        CFRelease(props);
        CFRelease(qnum);
    }
    if (dest) CFRelease(dest);
    if (jpeg_data) CFRelease(jpeg_data);
    if (img) CGImageRelease(img);
    if (provider) CGDataProviderRelease(provider);
    if (cs) CGColorSpaceRelease(cs);
    if (out.empty()) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PROJECT, "failed to encode fallback preview");
    }
    return out;
}

// Memory budget semaphore so large 4K/8K textures never trigger OOM during parallel transcoding,
// while still allowing at least one huge texture to proceed without deadlocking.
class MemoryBudget {
public:
    explicit MemoryBudget(uint64_t limit_bytes) : limit_(limit_bytes) {}

    void Acquire(uint64_t bytes) {
        std::unique_lock<std::mutex> lock(mu_);
        uint64_t effective = std::min(bytes, limit_);
        cv_.wait(lock, [&]() {
            return used_ == 0 || used_ + effective <= limit_;
        });
        used_ += effective;
    }

    void Release(uint64_t bytes) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            uint64_t effective = std::min(bytes, limit_);
            used_ = (used_ >= effective) ? (used_ - effective) : 0;
        }
        cv_.notify_all();
    }

private:
    uint64_t limit_;
    uint64_t used_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
};

struct SourceEntry {
    std::string rel_path;
    bool from_pkg = false;
    PkgIndexEntry pkg_entry;
    std::filesystem::path disk_path;
    uint64_t size = 0;
};

struct TexJob {
    size_t original_index = 0;
    SourceEntry source;
    bool force_etc2 = false;
    uint64_t estimated_mem_bytes = 0;
};

struct ExportStats {
    std::string mpkg_magic;
    uint64_t total_entries = 0;
    uint64_t tex_transcoded = 0;
    uint64_t tex_skipped = 0;
    uint64_t shaders_rewritten = 0;
    uint64_t shaders_total = 0;
    uint64_t sounds_removed = 0;
    uint64_t output_size_bytes = 0;
};

ExportStats RunSceneExport(const MirageSceneMobileExportOptions& opts) {
    if (!opts.wallpaper_dir || !opts.output_path) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PROJECT, "missing wallpaper_dir or output_path");
    }
    std::filesystem::path wallpaper_dir(opts.wallpaper_dir);
    std::filesystem::path render_dir = (opts.render_dir && opts.render_dir[0] != '\0')
        ? std::filesystem::path(opts.render_dir)
        : wallpaper_dir;
    std::filesystem::path output_path(opts.output_path);

    std::vector<std::filesystem::path> overlay_dirs;
    if (opts.overlay_dirs && opts.overlay_dir_count > 0) {
        overlay_dirs.reserve(opts.overlay_dir_count);
        for (size_t i = 0; i < opts.overlay_dir_count; ++i) {
            if (opts.overlay_dirs[i] && opts.overlay_dirs[i][0] != '\0') {
                overlay_dirs.emplace_back(opts.overlay_dirs[i]);
            }
        }
    }

    std::optional<std::filesystem::path> zcompat_dir;
    if (opts.zcompat_dir && opts.zcompat_dir[0] != '\0') {
        zcompat_dir = std::filesystem::path(opts.zcompat_dir);
    }

    std::mutex progress_mu;
    double highest_progress = -1.0;
    auto report_progress = [&](double fraction) {
        if (!opts.progress_cb) return;
        double clamped = std::clamp(fraction, 0.0, 1.0);
        std::lock_guard<std::mutex> lk(progress_mu);
        if (clamped < highest_progress) clamped = highest_progress;
        highest_progress = clamped;
        opts.progress_cb(clamped, opts.progress_context);
    };
    report_progress(0.0);
    CheckCancelled(opts.cancel_flag);

    // 1. Load project.json (for presets where wallpaper_dir != render_dir, base project comes from
    //    render_dir while title/preview/workshopid can come from wallpaper_dir).
    auto canon_wp_dir = CanonicalPath(wallpaper_dir);
    auto canon_render_dir = CanonicalPath(render_dir);
    if (!canon_wp_dir || !canon_render_dir) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PROJECT, "invalid wallpaper directory");
    }
    bool is_preset = (*canon_wp_dir != *canon_render_dir);

    auto base_pj_path = ResolveContainedFile(*canon_render_dir, "project.json");
    if (!base_pj_path) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PROJECT, "missing project.json");
    }
    auto base_pj_opt = ParseJsonLenient(ReadFileBytes(*base_pj_path, 16 * 1024 * 1024));
    if (!base_pj_opt || !base_pj_opt->is_object()) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PROJECT, "malformed project.json");
    }
    Json project_doc = std::move(*base_pj_opt);

    // Extract the base scene's workshop ID for zcompat shader rules BEFORE preset metadata overrides it.
    uint64_t workshop_id = opts.workshop_id_override != 0
        ? opts.workshop_id_override
        : ExtractWorkshopId(project_doc, *canon_render_dir);

    Json preset_property_overrides = Json::object();
    auto merge_preset_file = [&](const std::filesystem::path& dir, bool apply_metadata) {
        auto canon_ov = CanonicalPath(dir);
        if (!canon_ov || *canon_ov == *canon_render_dir) return;
        auto preset_pj_path = ResolveContainedFile(*canon_ov, "project.json");
        if (!preset_pj_path) return;
        auto preset_doc = ParseJsonLenient(ReadFileBytes(*preset_pj_path, 16 * 1024 * 1024));
        if (!preset_doc || !preset_doc->is_object()) return;
        if (apply_metadata) {
            if (auto it = preset_doc->find("title"); it != preset_doc->end() && it->is_string()) {
                project_doc["title"] = *it;
            }
            if (auto it = preset_doc->find("preview"); it != preset_doc->end() && it->is_string()) {
                project_doc["preview"] = *it;
            }
            if (auto it = preset_doc->find("workshopid"); it != preset_doc->end()) {
                project_doc["workshopid"] = *it;
            }
        }
        if (auto preset_map = preset_doc->find("preset");
            preset_map != preset_doc->end() && preset_map->is_object()) {
            for (const auto& [k, v] : preset_map->items()) {
                preset_property_overrides[k] = v;
            }
        }
    };
    if (is_preset) {
        // Apply nested overlay presets from innermost to outermost, then wallpaper_dir itself.
        for (auto it = overlay_dirs.rbegin(); it != overlay_dirs.rend(); ++it) {
            merge_preset_file(*it, false);
        }
        merge_preset_file(*canon_wp_dir, true);
    }

    // Verify wallpaper type is "scene".
    std::string wp_type;
    if (auto it = project_doc.find("type"); it != project_doc.end() && it->is_string()) {
        wp_type = AsciiLower(TrimAscii(it->get_ref<const std::string&>()));
    }
    if (wp_type != "scene") {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PROJECT, "project type is not scene");
    }

    std::string scene_name = "scene.json";
    if (auto it = project_doc.find("file"); it != project_doc.end() && it->is_string()) {
        auto safe_scene = NormalizeSafeArchivePath(it->get_ref<const std::string&>());
        if (!safe_scene) {
            throw ConvertError(MIRAGE_MOBILE_UNSAFE_PROJECT_PATH, it->get_ref<const std::string&>());
        }
        scene_name = *safe_scene;
    }

    // 2. Discover source entries: prefer `<file_stem>.pkg` or `scene.pkg`; else crawl loose project files.
    std::optional<PkgIndex> pkg_index;
    std::string mpkg_magic = kDefaultMpkgMagic;
    std::map<std::string, SourceEntry> source_map; // keyed by lowercase rel_path

    auto try_open_pkg = [&](std::string_view pkg_rel) -> bool {
        auto pkg_path = ResolveContainedFile(*canon_render_dir, pkg_rel);
        if (!pkg_path) return false;
        pkg_index = PkgIndex::Open(*pkg_path);
        if (pkg_index->is_mobile()) {
            mpkg_magic = pkg_index->magic();
        } else if (auto ver = pkg_index->version_number()) {
            mpkg_magic = FormatMpkgMagic(MapPkgToMpkgVersion(*ver, opts.force_legacy_pkg != 0));
        }
        for (const auto& e : pkg_index->entries()) {
            if (AsciiEqualsIgnoreCase(e.name, "project.json")) continue;
            SourceEntry se;
            se.rel_path = e.name;
            se.from_pkg = true;
            se.pkg_entry = e;
            se.size = e.size;
            source_map[AsciiLower(e.name)] = std::move(se);
        }
        return true;
    };

    std::string pkg_from_scene;
    if (size_t dot = scene_name.rfind('.'); dot != std::string::npos) {
        pkg_from_scene = scene_name.substr(0, dot) + ".pkg";
    }
    bool loaded_pkg = false;
    if (!pkg_from_scene.empty()) {
        loaded_pkg = try_open_pkg(pkg_from_scene);
    }
    if (!loaded_pkg && !AsciiEqualsIgnoreCase(pkg_from_scene, "scene.pkg")) {
        loaded_pkg = try_open_pkg("scene.pkg");
    }

    if (!loaded_pkg) {
        auto loose = CollectLooseProjectFiles(*canon_render_dir, scene_name, false);
        for (auto& f : loose) {
            if (AsciiEqualsIgnoreCase(f.rel_path, "project.json")) continue;
            std::error_code ec;
            auto sz = std::filesystem::file_size(f.full_path, ec);
            if (ec) continue;
            SourceEntry se;
            se.rel_path = std::move(f.rel_path);
            se.from_pkg = false;
            se.disk_path = std::move(f.full_path);
            se.size = sz;
            source_map[AsciiLower(se.rel_path)] = std::move(se);
        }
    }

    // Verify scene entry exists.
    auto scene_it = source_map.find(AsciiLower(scene_name));
    if (scene_it == source_map.end()) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PROJECT, "missing scene file: " + scene_name);
    }

    auto read_source_bytes = [&](const SourceEntry& se) -> std::vector<uint8_t> {
        if (se.from_pkg) return pkg_index->ReadEntry(se.pkg_entry);
        return ReadFileBytes(se.disk_path);
    };

    // 3. Parse scene.json & inspect texture headers for auto UHD / Pixel Art detection (if needed).
    std::vector<uint8_t> scene_bytes = read_source_bytes(scene_it->second);
    auto scene_json_opt = ParseJsonLenient(scene_bytes);

    auto detect_pixel_art = [&]() -> bool {
        if (scene_json_opt && scene_json_opt->is_object()) {
            if (auto g = scene_json_opt->find("general"); g != scene_json_opt->end() && g->is_object()) {
                if (auto ortho = g->find("orthogonalprojection"); ortho != g->end() && ortho->is_object()) {
                    uint64_t w = ortho->value("width", 0ULL);
                    uint64_t h = ortho->value("height", 0ULL);
                    if (w > 0 && h > 0 && w * h < kPixelArtPixelThreshold) return true;
                }
            }
        }
        if (auto tags = project_doc.find("tags"); tags != project_doc.end() && tags->is_array()) {
            for (const auto& t : *tags) {
                if (t.is_string() && AsciiEqualsIgnoreCase(t.get_ref<const std::string&>(), "pixel art")) {
                    return true;
                }
            }
        }
        size_t total_tex = 0, nearest_tex = 0;
        for (const auto& [lower, se] : source_map) {
            if (StartsWithIgnoreCase(lower, "materials/") && EndsWithIgnoreCase(lower, ".tex")) {
                try {
                    auto hdr_bytes = se.from_pkg
                        ? pkg_index->ReadEntryPrefix(se.pkg_entry, 64)
                        : ReadFilePrefix(se.disk_path, 64);
                    auto hdr = TexFile::ParseHeader(hdr_bytes.data(), hdr_bytes.size(), se.rel_path);
                    ++total_tex;
                    if (hdr.flags & kTexFlagNoInterpolation) ++nearest_tex;
                } catch (...) {}
            }
        }
        return total_tex > 0 && nearest_tex * 2 >= total_tex;
    };

    auto detect_uhd = [&]() -> bool {
        if (auto tags = project_doc.find("tags"); tags != project_doc.end() && tags->is_array()) {
            for (const auto& t : *tags) {
                if (t.is_string()) {
                    if (auto px = ParseResolutionTagPixels(t.get_ref<const std::string&>());
                        px && *px > kUhdPixelThreshold) {
                        return true;
                    }
                }
            }
        }
        if (scene_json_opt && scene_json_opt->is_object()) {
            if (auto g = scene_json_opt->find("general"); g != scene_json_opt->end() && g->is_object()) {
                if (auto ortho = g->find("orthogonalprojection"); ortho != g->end() && ortho->is_object()) {
                    uint64_t w = ortho->value("width", 0ULL);
                    uint64_t h = ortho->value("height", 0ULL);
                    if (w * h > kUhdPixelThreshold) return true;
                }
            }
        }
        for (const auto& [lower, se] : source_map) {
            if (StartsWithIgnoreCase(lower, "materials/") && EndsWithIgnoreCase(lower, ".tex")) {
                try {
                    auto hdr_bytes = se.from_pkg
                        ? pkg_index->ReadEntryPrefix(se.pkg_entry, 64)
                        : ReadFilePrefix(se.disk_path, 64);
                    auto hdr = TexFile::ParseHeader(hdr_bytes.data(), hdr_bytes.size(), se.rel_path);
                    if (static_cast<uint64_t>(hdr.image_width) * hdr.image_height > kUhdPixelThreshold) {
                        return true;
                    }
                } catch (...) {}
            }
        }
        return false;
    };

    bool is_pixel_art = (opts.pixel_art >= 0) ? (opts.pixel_art != 0) : detect_pixel_art();
    uint32_t reduction = 1;
    MirageMobileCompressionMode compression = MIRAGE_MOBILE_COMPRESSION_HIGH_PERFORMANCE;
    if (opts.texture_reduction > 0) {
        reduction = static_cast<uint32_t>(std::clamp(opts.texture_reduction, 1, 8));
        if (opts.compression_mode >= 0) {
            compression = static_cast<MirageMobileCompressionMode>(opts.compression_mode);
        } else {
            compression = is_pixel_art
                ? MIRAGE_MOBILE_COMPRESSION_HIGH_QUALITY
                : MIRAGE_MOBILE_COMPRESSION_HIGH_PERFORMANCE;
        }
    } else {
        bool is_uhd = detect_uhd();
        if (is_uhd) {
            compression = MIRAGE_MOBILE_COMPRESSION_HIGH_PERFORMANCE;
            reduction = 2;
        } else if (is_pixel_art) {
            compression = MIRAGE_MOBILE_COMPRESSION_HIGH_QUALITY;
            reduction = 1;
        } else {
            compression = MIRAGE_MOBILE_COMPRESSION_HIGH_PERFORMANCE;
            reduction = 1;
        }
        if (opts.compression_mode >= 0) {
            compression = static_cast<MirageMobileCompressionMode>(opts.compression_mode);
        }
    }

    // 4. Partition entries into sounds, textures, shaders, scene.json, and zero-copy passthroughs.
    ScopedTempDir staging("Mirage-SceneMobile");
    std::map<std::string, MpkgWriteEntry> output_entries; // keyed by lowercase rel_path
    std::vector<TexJob> tex_jobs;
    ExportStats stats;
    stats.mpkg_magic = mpkg_magic;

    size_t total_steps = std::max<size_t>(source_map.size() + 2, 1);
    std::atomic<size_t> completed_steps{0};
    auto bump_progress = [&]() {
        size_t done = completed_steps.fetch_add(1, std::memory_order_relaxed) + 1;
        report_progress(std::min(0.9, (static_cast<double>(done) / static_cast<double>(total_steps)) * 0.9));
    };

    size_t entry_idx = 0;
    for (const auto& [lower, se] : source_map) {
        CheckCancelled(opts.cancel_flag);
        ++entry_idx;

        if (!opts.keep_sounds && IsAudioEntry(se.rel_path)) {
            ++stats.sounds_removed;
            bump_progress();
            continue;
        }

        if (EndsWithIgnoreCase(lower, ".tex")) {
            std::string parent = ParentDirLower(se.rel_path);
            bool is_forced_dir = (parent == "materials" || parent == "materials/masks");
            bool force_etc2 = is_forced_dir && (compression == MIRAGE_MOBILE_COMPRESSION_HIGH_PERFORMANCE);
            uint64_t est_mem = std::max<uint64_t>(se.size * 6ULL, 8ULL * 1024ULL * 1024ULL);
            try {
                auto hdr_bytes = se.from_pkg
                    ? pkg_index->ReadEntryPrefix(se.pkg_entry, 64)
                    : ReadFilePrefix(se.disk_path, 64);
                auto hdr = TexFile::ParseHeader(hdr_bytes.data(), hdr_bytes.size(), se.rel_path);
                uint64_t rgba_bytes = static_cast<uint64_t>(hdr.texture_width) * hdr.texture_height * 4ULL;
                est_mem = std::max<uint64_t>(rgba_bytes * 3ULL, 4ULL * 1024ULL * 1024ULL);
            } catch (...) {}

            tex_jobs.push_back(TexJob{entry_idx, se, force_etc2, est_mem});
            continue;
        }

        if (AsciiEqualsIgnoreCase(se.rel_path, scene_name)) {
            bool modified = false;
            if (scene_json_opt && scene_json_opt->is_object()) {
                Json scene_doc = *scene_json_opt;
                auto& gen = scene_doc["general"];
                if (!gen.is_object()) gen = Json::object();
                if (reduction == 1) {
                    if (gen.erase("texturereduction") > 0) modified = true;
                } else {
                    gen["texturereduction"] = reduction;
                    modified = true;
                }
                if (modified) {
                    output_entries[lower] = MpkgWriteEntry::FromMemory(
                        se.rel_path, ToJsonCppStyledBytes(scene_doc));
                }
            }
            if (!modified) {
                if (se.from_pkg) {
                    output_entries[lower] = MpkgWriteEntry::FromFileSlice(
                        se.rel_path, pkg_index->path(), se.pkg_entry.absolute_offset, se.pkg_entry.size);
                } else {
                    output_entries[lower] = MpkgWriteEntry::FromFile(se.rel_path, se.disk_path);
                }
            }
            bump_progress();
            continue;
        }

        if (IsShaderPath(se.rel_path)) {
            ++stats.shaders_total;
            auto raw = read_source_bytes(se);
            std::string_view src_sv(reinterpret_cast<const char*>(raw.data()), raw.size());
            auto [patched, changed] = RewriteShaderForMobile(
                se.rel_path, src_sv, workshop_id, zcompat_dir);
            if (changed) {
                ++stats.shaders_rewritten;
                output_entries[lower] = MpkgWriteEntry::FromMemory(
                    se.rel_path, std::vector<uint8_t>(patched.begin(), patched.end()));
            } else if (se.from_pkg) {
                output_entries[lower] = MpkgWriteEntry::FromFileSlice(
                    se.rel_path, pkg_index->path(), se.pkg_entry.absolute_offset, se.pkg_entry.size);
            } else {
                output_entries[lower] = MpkgWriteEntry::FromFile(se.rel_path, se.disk_path);
            }
            bump_progress();
            continue;
        }

        // Unchanged non-texture, non-shader entry: zero-copy file slice.
        if (se.from_pkg) {
            output_entries[lower] = MpkgWriteEntry::FromFileSlice(
                se.rel_path, pkg_index->path(), se.pkg_entry.absolute_offset, se.pkg_entry.size);
        } else {
            output_entries[lower] = MpkgWriteEntry::FromFile(se.rel_path, se.disk_path);
        }
        bump_progress();
    }

    // 5. Execute LPT parallel texture transcoding with memory budget guard.
    if (!tex_jobs.empty()) {
        std::sort(tex_jobs.begin(), tex_jobs.end(), [](const TexJob& a, const TexJob& b) {
            if (a.source.size != b.source.size) return a.source.size > b.source.size;
            return a.original_index < b.original_index;
        });

        unsigned hw = std::thread::hardware_concurrency();
        size_t default_workers = (hw > 2) ? std::min<size_t>(hw - 1, 16) : 1;
        size_t max_workers = (opts.jobs > 0) ? static_cast<size_t>(std::clamp(opts.jobs, 1, 64)) : default_workers;
        size_t worker_count = std::min(max_workers, tex_jobs.size());

        // When only a few textures exist (e.g., 1 giant 4K/8K texture), let ETC2 parallelize rows inside each job.
        size_t inner_threads = (worker_count < default_workers)
            ? std::max<size_t>(1, default_workers / std::max<size_t>(worker_count, 1))
            : 1;

        MemoryBudget budget(512ULL * 1024ULL * 1024ULL); // 512 MiB working-set budget
        std::atomic<size_t> next_job{0};
        std::atomic<uint64_t> staged_seq{0};
        std::mutex result_mu;
        std::exception_ptr first_error;
        std::atomic<bool> failed{false};
        std::vector<MpkgWriteEntry> tex_results;
        tex_results.reserve(tex_jobs.size());
        std::atomic<uint64_t> transcoded_count{0};
        std::atomic<uint64_t> skipped_count{0};

        auto worker_fn = [&]() {
            while (true) {
                if (failed.load(std::memory_order_relaxed) || IsCancelled(opts.cancel_flag)) {
                    return;
                }
                size_t idx = next_job.fetch_add(1, std::memory_order_relaxed);
                if (idx >= tex_jobs.size()) return;
                const TexJob& job = tex_jobs[idx];

                budget.Acquire(job.estimated_mem_bytes);
                struct BudgetGuard {
                    MemoryBudget& b;
                    uint64_t sz;
                    ~BudgetGuard() { b.Release(sz); }
                } bguard{budget, job.estimated_mem_bytes};

                try {
                    TexTranscodeResult res;
                    {
                        auto raw = read_source_bytes(job.source);
                        TexTranscodeOptions topts;
                        topts.force_etc2 = job.force_etc2;
                        topts.shrink = reduction;
                        topts.pixel_art = is_pixel_art;
                        topts.prefer_rgb8_when_opaque = (opts.prefer_rgb8_when_opaque != 0);
                        topts.inner_threads = inner_threads;
                        topts.cancel_flag = opts.cancel_flag;

                        res = TranscodeTexBytesForMobile(raw.data(), raw.size(), job.source.rel_path, topts);
                    }
                    MpkgWriteEntry out_entry;
                    if (!res.transcoded) {
                        skipped_count.fetch_add(1, std::memory_order_relaxed);
                        if (job.source.from_pkg) {
                            out_entry = MpkgWriteEntry::FromFileSlice(
                                job.source.rel_path,
                                pkg_index->path(),
                                job.source.pkg_entry.absolute_offset,
                                job.source.pkg_entry.size);
                        } else {
                            out_entry = MpkgWriteEntry::FromFile(job.source.rel_path, job.source.disk_path);
                        }
                    } else {
                        transcoded_count.fetch_add(1, std::memory_order_relaxed);
                        if (res.bytes.size() >= kStageToDiskThresholdBytes) {
                            uint64_t seq = staged_seq.fetch_add(1, std::memory_order_relaxed);
                            auto staged_file = staging.path() / ("tex-" + std::to_string(seq) + ".tex");
                            std::ofstream ofs(staged_file, std::ios::binary);
                            if (!ofs) {
                                throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "cannot write staged texture");
                            }
                            ofs.write(reinterpret_cast<const char*>(res.bytes.data()),
                                      static_cast<std::streamsize>(res.bytes.size()));
                            if (!ofs) {
                                throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "failed writing staged texture");
                            }
                            ofs.close();
                            out_entry = MpkgWriteEntry::FromFileSlice(
                                job.source.rel_path, std::move(staged_file), 0, res.bytes.size());
                        } else {
                            out_entry = MpkgWriteEntry::FromMemory(job.source.rel_path, std::move(res.bytes));
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lk(result_mu);
                        tex_results.push_back(std::move(out_entry));
                    }
                    bump_progress();
                } catch (...) {
                    std::lock_guard<std::mutex> lk(result_mu);
                    if (!first_error) first_error = std::current_exception();
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        };

        if (worker_count == 1) {
            worker_fn();
        } else {
            std::vector<std::thread> threads;
            threads.reserve(worker_count);
            for (size_t t = 0; t < worker_count; ++t) {
                threads.emplace_back(worker_fn);
            }
            for (auto& th : threads) th.join();
        }

        if (first_error) std::rethrow_exception(first_error);
        CheckCancelled(opts.cancel_flag);

        stats.tex_transcoded = transcoded_count.load(std::memory_order_relaxed);
        stats.tex_skipped = skipped_count.load(std::memory_order_relaxed);
        for (auto& e : tex_results) {
            output_entries[AsciiLower(e.name)] = std::move(e);
        }
    }

    // 6. Resolve preview image (preserve verbatim from wallpaper_dir / render_dir / archive;
    //    reject path traversal; synthesize 360px JPEG fallback only if absent).
    std::string preview_name;
    bool preview_generated = false;
    std::optional<std::filesystem::path> preview_disk_path;
    if (auto it = project_doc.find("preview"); it != project_doc.end() && it->is_string()) {
        std::string raw_preview = TrimAscii(it->get_ref<const std::string&>());
        if (!raw_preview.empty()) {
            auto safe_prev = NormalizeSafeArchivePath(raw_preview);
            if (!safe_prev) {
                throw ConvertError(MIRAGE_MOBILE_UNSAFE_PROJECT_PATH, raw_preview);
            }
            preview_name = *safe_prev;
            preview_disk_path = ResolveContainedFile(*canon_wp_dir, preview_name);
            if (!preview_disk_path && is_preset) {
                preview_disk_path = ResolveContainedFile(*canon_render_dir, preview_name);
            }
        }
    }

    if (preview_disk_path) {
        std::string pkey = AsciiLower(preview_name);
        if (output_entries.count(pkey) > 0) {
            // Preview was already inside scene.pkg; keep the directory's copy cleanly.
            output_entries[pkey] = MpkgWriteEntry::FromFile(preview_name, *preview_disk_path);
        } else {
            output_entries[pkey] = MpkgWriteEntry::FromFile(preview_name, *preview_disk_path);
        }
    } else if (!preview_name.empty() && output_entries.count(AsciiLower(preview_name)) > 0) {
        // Preview is already present inside the archive.
    } else {
        preview_name = "nopreview.jpg";
        preview_generated = true;
        output_entries[AsciiLower(preview_name)] = MpkgWriteEntry::FromMemory(
            preview_name, GenerateFallbackPreviewJpeg());
    }
    bump_progress();

    // 7. Patch project.json (preview, workshopurl, user property overrides, scenetexture VFS).
    if (preview_generated || !project_doc.contains("preview") ||
        !project_doc["preview"].is_string() || TrimAscii(project_doc["preview"].get<std::string>()).empty()) {
        project_doc["preview"] = preview_name;
    }
    if (opts.workshop_url_override && opts.workshop_url_override[0] != '\0') {
        project_doc["workshopurl"] = std::string(opts.workshop_url_override);
    }

    Json clean_overrides = Json::object();
    auto merge_overrides = [&](const Json& obj) {
        if (!obj.is_object()) return;
        for (const auto& [k, v] : obj.items()) {
            if (IsStrippedPcPropertyKey(k)) continue;
            Json unwrapped = (v.is_object() && v.contains("value")) ? v["value"] : v;
            if (unwrapped.is_null()) continue;
            clean_overrides[k] = std::move(unwrapped);
        }
    };
    merge_overrides(preset_property_overrides);
    if (opts.property_overrides_json && opts.property_overrides_json[0] != '\0') {
        if (auto overrides_opt = ParseJsonLenient(std::string_view(opts.property_overrides_json));
            overrides_opt && overrides_opt->is_object()) {
            merge_overrides(*overrides_opt);
        }
    }

    if (!clean_overrides.empty()) {
        if (!project_doc.contains("general") || !project_doc["general"].is_object()) {
            project_doc["general"] = Json::object();
        }
        auto& gen = project_doc["general"];
        if (!gen.contains("properties") || !gen["properties"].is_object()) {
            gen["properties"] = Json::object();
        }
        auto& props = gen["properties"];

        for (const auto& [prop_key, override_val] : clean_overrides.items()) {
            auto pit = props.find(prop_key);
            if (pit != props.end() && pit->is_object()) {
                bool is_scene_tex = pit->contains("type") && (*pit)["type"].is_string() &&
                    AsciiEqualsIgnoreCase((*pit)["type"].get_ref<const std::string&>(), "scenetexture");
                if (is_scene_tex) {
                    if (override_val.is_string()) {
                        std::string raw_p = override_val.get<std::string>();
                        if (raw_p.empty()) {
                            (*pit)["value"] = "";
                            continue;
                        }
                        if (StartsWithIgnoreCase(raw_p, "mpkg_vfs://")) {
                            (*pit)["value"] = raw_p;
                            continue;
                        }
                    }
                    std::optional<std::filesystem::path> resolved_file;
                    if (override_val.is_string()) {
                        std::string raw_p = override_val.get<std::string>();
                        std::filesystem::path cand(raw_p);
                        if (cand.is_absolute()) {
                            std::error_code ec;
                            if (std::filesystem::is_regular_file(cand, ec) && !ec) {
                                resolved_file = CanonicalPath(cand);
                            }
                        } else {
                            for (const auto& ov : overlay_dirs) {
                                if (auto r = ResolveContainedFile(ov, raw_p)) {
                                    resolved_file = r;
                                    break;
                                }
                            }
                            if (!resolved_file) {
                                resolved_file = ResolveContainedFile(*canon_wp_dir, raw_p);
                            }
                            if (!resolved_file && is_preset) {
                                resolved_file = ResolveContainedFile(*canon_render_dir, raw_p);
                            }
                        }
                    }
                    if (resolved_file) {
                        std::string fname = resolved_file->filename().string();
                        std::string vfs_rel = "files/" + fname;
                        auto existing = output_entries.find(AsciiLower(vfs_rel));
                        if (existing != output_entries.end() &&
                            (!existing->second.is_file || existing->second.file_path != *resolved_file)) {
                            vfs_rel = "files/" + prop_key + "_" + fname;
                        }
                        if (IsSafeEntryName(vfs_rel)) {
                            output_entries[AsciiLower(vfs_rel)] = MpkgWriteEntry::FromFile(
                                vfs_rel, *resolved_file);
                            (*pit)["value"] = "MPKG_VFS://" + vfs_rel;
                            continue;
                        }
                    }
                    props.erase(pit);
                    continue;
                }
                (*pit)["value"] = override_val;
            } else {
                props[prop_key] = Json{{"value", override_val}};
            }
        }
    }

    output_entries["project.json"] = MpkgWriteEntry::FromMemory(
        "project.json", ToJsonCppStyledBytes(project_doc));
    bump_progress();

    // 8. Sort entries deterministically (case-insensitive order, tie-break lex) and stream atomically.
    std::vector<MpkgWriteEntry> final_entries;
    final_entries.reserve(output_entries.size());
    for (auto& [_, e] : output_entries) {
        final_entries.push_back(std::move(e));
    }
    std::sort(final_entries.begin(), final_entries.end(),
              [](const MpkgWriteEntry& a, const MpkgWriteEntry& b) {
                  std::string la = AsciiLower(a.name);
                  std::string lb = AsciiLower(b.name);
                  return (la == lb) ? (a.name < b.name) : (la < lb);
              });

    stats.total_entries = final_entries.size();
    stats.output_size_bytes = WriteMpkgArchive(
        final_entries,
        mpkg_magic,
        output_path,
        [&](uint64_t written, uint64_t total) {
            double frac = (total == 0) ? 1.0 : (static_cast<double>(written) / static_cast<double>(total));
            report_progress(0.9 + frac * 0.1);
        },
        opts.cancel_flag);
    report_progress(1.0);
    return stats;
}

} // namespace

} // namespace mirage::mobile

extern "C" MirageSceneMobileExportResult MirageSceneMobileExport(
    const MirageSceneMobileExportOptions* options) {
    MirageSceneMobileExportResult res{};
    if (!options) {
        res.status = MIRAGE_MOBILE_INVALID_PROJECT;
        res.error_detail = strdup("null options");
        return res;
    }
    try {
        auto stats = mirage::mobile::RunSceneExport(*options);
        res.status = MIRAGE_MOBILE_OK;
        std::snprintf(res.mpkg_magic, sizeof(res.mpkg_magic), "%s", stats.mpkg_magic.c_str());
        res.total_entries = stats.total_entries;
        res.tex_transcoded = stats.tex_transcoded;
        res.tex_skipped = stats.tex_skipped;
        res.shaders_rewritten = stats.shaders_rewritten;
        res.shaders_total = stats.shaders_total;
        res.sounds_removed = stats.sounds_removed;
        res.output_size_bytes = stats.output_size_bytes;
    } catch (const mirage::mobile::ConvertError& e) {
        res.status = static_cast<int32_t>(e.status());
        res.detail_int = e.detail_int();
        if (!e.detail().empty()) {
            res.error_detail = strdup(e.detail().c_str());
        }
    } catch (const std::exception& e) {
        res.status = MIRAGE_MOBILE_IO_ERROR;
        res.error_detail = strdup(e.what());
    } catch (...) {
        res.status = MIRAGE_MOBILE_IO_ERROR;
        res.error_detail = strdup("unknown internal error");
    }
    return res;
}

extern "C" int32_t MirageSceneMobileExportFlat(
    const char* wallpaper_dir,
    const char* render_dir,
    const char* const* overlay_dirs,
    size_t overlay_dir_count,
    const char* output_path,
    const char* zcompat_dir,
    const char* property_overrides_json,
    int32_t texture_reduction,
    int32_t pixel_art,
    int32_t compression_mode,
    int32_t keep_sounds,
    MirageMobileProgressCallback progress_cb,
    void* progress_context,
    const volatile int32_t* cancel_flag,
    int32_t* out_detail_int,
    char** out_error_detail) {
    MirageSceneMobileExportOptions opts{};
    opts.wallpaper_dir = wallpaper_dir;
    opts.render_dir = render_dir;
    opts.overlay_dirs = overlay_dirs;
    opts.overlay_dir_count = overlay_dir_count;
    opts.output_path = output_path;
    opts.zcompat_dir = zcompat_dir;
    opts.property_overrides_json = property_overrides_json;
    opts.texture_reduction = texture_reduction;
    opts.pixel_art = pixel_art;
    opts.compression_mode = compression_mode;
    opts.keep_sounds = keep_sounds;
    opts.progress_cb = progress_cb;
    opts.progress_context = progress_context;
    opts.cancel_flag = cancel_flag;

    MirageSceneMobileExportResult res = MirageSceneMobileExport(&opts);
    if (out_detail_int) *out_detail_int = res.detail_int;
    if (out_error_detail) {
        *out_error_detail = res.error_detail;
    } else {
        MirageSceneMobileFreeString(res.error_detail);
    }
    return res.status;
}

extern "C" void MirageSceneMobileFreeString(char* value) {
    std::free(value);
}

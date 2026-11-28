//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_INTERNAL_HPP
#define MIRAGE_SCENE_MOBILE_INTERNAL_HPP

#include "SceneMobileConverter.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace mirage::mobile {

class ConvertError : public std::runtime_error {
public:
    ConvertError(MirageMobileStatus status, std::string detail = "", int32_t detail_int = 0)
        : std::runtime_error(detail.empty() ? "scene mobile export error" : detail),
          status_(status),
          detail_(std::move(detail)),
          detail_int_(detail_int) {}

    MirageMobileStatus status() const noexcept { return status_; }
    const std::string& detail() const noexcept { return detail_; }
    int32_t detail_int() const noexcept { return detail_int_; }

private:
    MirageMobileStatus status_;
    std::string detail_;
    int32_t detail_int_;
};

inline bool IsCancelled(const volatile int32_t* cancel_flag) noexcept {
    if (!cancel_flag) return false;
    return __atomic_load_n(cancel_flag, __ATOMIC_RELAXED) != 0;
}

inline void CheckCancelled(const volatile int32_t* cancel_flag) {
    if (IsCancelled(cancel_flag)) {
        throw ConvertError(MIRAGE_MOBILE_CANCELLED, "cancelled");
    }
}

inline std::string AsciiLower(std::string_view input) {
    std::string out(input);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

inline bool AsciiEqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

inline bool EndsWithIgnoreCase(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    return AsciiEqualsIgnoreCase(s.substr(s.size() - suffix.size()), suffix);
}

inline bool StartsWithIgnoreCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    return AsciiEqualsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

inline std::string TrimAscii(std::string_view sv) {
    size_t b = 0;
    while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    size_t e = sv.size();
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(b, e - b));
}

// Strict archive entry path validation matching Mirage's MobilePackageArchive.isSafeEntryName.
inline bool IsSafeEntryName(std::string_view name) {
    if (name.empty() || name.size() > 1024) return false;
    if (name.front() == '/') return false;
    for (char c : name) {
        if (c == '\0' || c == '\\') return false;
    }
    size_t start = 0;
    while (true) {
        size_t slash = name.find('/', start);
        std::string_view seg = (slash == std::string_view::npos)
            ? name.substr(start)
            : name.substr(start, slash - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

// Normalizes Windows/relative archive paths safely without allowing traversal above root.
// Returns std::nullopt if the path attempts to escape via `..` or contains NUL or is empty.
inline std::optional<std::string> NormalizeSafeArchivePath(std::string_view raw) {
    if (raw.empty()) return std::nullopt;
    std::string replaced;
    replaced.reserve(raw.size());
    for (char c : raw) {
        if (c == '\0') return std::nullopt;
        replaced.push_back(c == '\\' ? '/' : c);
    }
    if (!replaced.empty() && replaced.front() == '/') return std::nullopt;

    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= replaced.size()) {
        size_t slash = replaced.find('/', start);
        std::string_view seg = (slash == std::string::npos)
            ? std::string_view(replaced).substr(start)
            : std::string_view(replaced).substr(start, slash - start);
        if (seg == "..") {
            return std::nullopt;
        }
        if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (parts.empty()) return std::nullopt;

    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.push_back('/');
        out.append(parts[i]);
    }
    if (!IsSafeEntryName(out)) return std::nullopt;
    return out;
}

// Permissive relative-path normalization for internal shader/asset references (collapses `.` and
// internal `a/../b`, still rejects escaping root or NUL).
inline std::optional<std::string> NormalizeRelativeAssetRef(std::string_view raw) {
    std::string trimmed = TrimAscii(raw);
    if (trimmed.empty()) return std::nullopt;
    for (char& c : trimmed) {
        if (c == '\0') return std::nullopt;
        if (c == '\\') c = '/';
    }
    if (!trimmed.empty() && trimmed.front() == '/') return std::nullopt;

    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= trimmed.size()) {
        size_t slash = trimmed.find('/', start);
        std::string_view seg = (slash == std::string::npos)
            ? std::string_view(trimmed).substr(start)
            : std::string_view(trimmed).substr(start, slash - start);
        if (seg == "..") {
            if (parts.empty()) return std::nullopt;
            parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (parts.empty()) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.push_back('/');
        out.append(parts[i]);
    }
    if (!IsSafeEntryName(out)) return std::nullopt;
    return out;
}

inline std::optional<std::filesystem::path> CanonicalPath(const std::filesystem::path& p) {
    char* resolved = ::realpath(p.c_str(), nullptr);
    if (!resolved) return std::nullopt;
    std::filesystem::path out(resolved);
    std::free(resolved);
    return out;
}

// Symlink-safe containment check matching PathContainment.swift.
inline std::optional<std::filesystem::path> ResolveContainedFile(
    const std::filesystem::path& root,
    std::string_view relative_path) {
    auto safe_rel = NormalizeSafeArchivePath(relative_path);
    if (!safe_rel) return std::nullopt;

    auto canon_root = CanonicalPath(root);
    if (!canon_root) return std::nullopt;

    auto canon_candidate = CanonicalPath(*canon_root / *safe_rel);
    if (!canon_candidate) return std::nullopt;

    std::string root_str = canon_root->string();
    std::string cand_str = canon_candidate->string();
    if (!root_str.empty() && root_str.back() != '/') {
        root_str.push_back('/');
    }
    if (cand_str.size() <= root_str.size() ||
        cand_str.compare(0, root_str.size(), root_str) != 0) {
        return std::nullopt;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(*canon_candidate, ec) || ec) {
        return std::nullopt;
    }
    return canon_candidate;
}

inline std::vector<uint8_t> ReadFileBytes(
    const std::filesystem::path& path,
    uint64_t max_bytes = 512ULL * 1024ULL * 1024ULL) {
    std::error_code ec;
    auto fsize = std::filesystem::file_size(path, ec);
    if (ec) {
        throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "cannot stat file: " + path.string());
    }
    if (fsize > max_bytes) {
        throw ConvertError(MIRAGE_MOBILE_WALLPAPER_TOO_LARGE, "file exceeds size limit: " + path.string());
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "cannot open file: " + path.string());
    }
    std::vector<uint8_t> out(static_cast<size_t>(fsize));
    if (fsize > 0) {
        ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(fsize));
        if (!ifs || static_cast<uint64_t>(ifs.gcount()) != fsize) {
            throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "truncated read: " + path.string());
        }
    }
    return out;
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        std::error_code ec;
        auto base = std::filesystem::temp_directory_path(ec);
        if (ec) base = "/tmp";
        std::string tmpl = (base / (std::string(prefix) + "-XXXXXX")).string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        char* created = ::mkdtemp(buf.data());
        if (!created) {
            throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "mkdtemp failed");
        }
        path_ = created;
    }

    ~ScopedTempDir() {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Little-endian helpers
inline uint16_t ReadU16LE(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t ReadU32LE(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t ReadI32LE(const uint8_t* p) noexcept {
    uint32_t u = ReadU32LE(p);
    int32_t v = 0;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

inline float ReadF32LE(const uint8_t* p) noexcept {
    uint32_t u = ReadU32LE(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline uint64_t ReadU64LE(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(ReadU32LE(p)) |
           (static_cast<uint64_t>(ReadU32LE(p + 4)) << 32);
}

inline uint64_t ReadU64BE(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) |
           static_cast<uint64_t>(p[7]);
}

inline void WriteU64BE(uint8_t* p, uint64_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 56) & 0xffU);
    p[1] = static_cast<uint8_t>((v >> 48) & 0xffU);
    p[2] = static_cast<uint8_t>((v >> 40) & 0xffU);
    p[3] = static_cast<uint8_t>((v >> 32) & 0xffU);
    p[4] = static_cast<uint8_t>((v >> 24) & 0xffU);
    p[5] = static_cast<uint8_t>((v >> 16) & 0xffU);
    p[6] = static_cast<uint8_t>((v >> 8) & 0xffU);
    p[7] = static_cast<uint8_t>(v & 0xffU);
}

inline void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xffU));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xffU));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xffU));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xffU));
}

inline void AppendI32LE(std::vector<uint8_t>& out, int32_t v) {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    AppendU32LE(out, u);
}

inline void AppendF32LE(std::vector<uint8_t>& out, float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    AppendU32LE(out, u);
}

inline void AppendStamp9(std::vector<uint8_t>& out, const char stamp[8]) {
    out.insert(out.end(), stamp, stamp + 8);
    out.push_back(0);
}

} // namespace mirage::mobile

#endif /* MIRAGE_SCENE_MOBILE_INTERNAL_HPP */

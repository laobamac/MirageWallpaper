//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#ifndef MIRAGE_SCENE_MOBILE_PKG_ARCHIVE_HPP
#define MIRAGE_SCENE_MOBILE_PKG_ARCHIVE_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mirage::mobile {

struct PkgIndexEntry {
    std::string name;
    uint64_t absolute_offset = 0;
    uint64_t size = 0;
};

// Streaming, thread-safe indexed reader for PKGV / PKGM archives.
// Reads ONLY the header directory; entry payloads are read on-demand via pread().
class PkgIndex {
public:
    PkgIndex() = default;
    ~PkgIndex();

    PkgIndex(PkgIndex&& other) noexcept;
    PkgIndex& operator=(PkgIndex&& other) noexcept;
    PkgIndex(const PkgIndex&) = delete;
    PkgIndex& operator=(const PkgIndex&) = delete;

    static PkgIndex Open(const std::filesystem::path& path);

    const std::filesystem::path& path() const noexcept { return path_; }
    const std::string& magic() const noexcept { return magic_; }
    bool is_mobile() const noexcept { return magic_.rfind("PKGM", 0) == 0; }
    std::optional<uint32_t> version_number() const noexcept;
    const std::vector<PkgIndexEntry>& entries() const noexcept { return entries_; }

    const PkgIndexEntry* FindEntry(std::string_view rel_path) const noexcept;
    std::vector<uint8_t> ReadEntry(const PkgIndexEntry& entry) const;
    std::vector<uint8_t> ReadEntryPrefix(const PkgIndexEntry& entry, size_t max_bytes) const;

private:
    std::filesystem::path path_;
    std::string magic_;
    std::vector<PkgIndexEntry> entries_;
    int fd_ = -1;
};

struct MpkgWriteEntry {
    std::string name;
    // Either in-memory bytes or a file slice (zero-copy streaming from source pkg / disk / staging).
    bool is_file = false;
    std::vector<uint8_t> memory_data;
    std::filesystem::path file_path;
    uint64_t file_offset = 0;
    uint64_t file_size = 0;

    uint64_t size() const noexcept {
        return is_file ? file_size : static_cast<uint64_t>(memory_data.size());
    }

    static MpkgWriteEntry FromMemory(std::string name, std::vector<uint8_t> data);
    static MpkgWriteEntry FromFileSlice(
        std::string name,
        std::filesystem::path path,
        uint64_t offset,
        uint64_t size);
    static MpkgWriteEntry FromFile(std::string name, std::filesystem::path path);
};

// Maps PKGV version number (1..24+) to PKGM version number (14..20).
uint32_t MapPkgToMpkgVersion(uint32_t pkg_ver, bool force_legacy = false) noexcept;
std::string FormatMpkgMagic(uint32_t mpkg_ver);

// Streams entries atomically into a PKGM archive (writes to a same-directory temporary file,
// fsyncs, and renames atomically so an existing output is never corrupted on failure).
// Returns total output size in bytes.
uint64_t WriteMpkgArchive(
    const std::vector<MpkgWriteEntry>& entries,
    const std::string& version_magic,
    const std::filesystem::path& output_path,
    const std::function<void(uint64_t written, uint64_t total)>& progress = nullptr,
    const volatile int32_t* cancel_flag = nullptr);

} // namespace mirage::mobile

#endif /* MIRAGE_SCENE_MOBILE_PKG_ARCHIVE_HPP */

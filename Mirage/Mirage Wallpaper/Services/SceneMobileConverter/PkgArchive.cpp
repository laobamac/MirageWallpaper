//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "PkgArchive.hpp"
#include "MobileInternal.hpp"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

namespace mirage::mobile {

namespace {

constexpr uint64_t kMaxEntryCount = 100000ULL;
constexpr uint64_t kMinEntryHeaderBytes = 13ULL;
constexpr size_t kStreamChunkBytes = 1024 * 1024;

void PreadExact(int fd, uint8_t* dst, size_t count, uint64_t offset, MirageMobileStatus eof_status, const std::string& label) {
    size_t done = 0;
    while (done < count) {
        ssize_t n = ::pread(fd, dst + done, count - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw ConvertError(MIRAGE_MOBILE_IO_ERROR, label);
        }
        if (n == 0) {
            throw ConvertError(eof_status, label);
        }
        done += static_cast<size_t>(n);
    }
}

void WriteAllFd(int fd, const uint8_t* data, size_t count, const std::string& label) {
    size_t done = 0;
    while (done < count) {
        ssize_t n = ::write(fd, data + done, count - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw ConvertError(MIRAGE_MOBILE_IO_ERROR, label);
        }
        done += static_cast<size_t>(n);
    }
}

bool IsValidMagic(std::string_view m, bool mobile_only) {
    if (m.size() != 8) return false;
    bool ok_prefix = mobile_only
        ? (m.compare(0, 4, "PKGM") == 0)
        : (m.compare(0, 4, "PKGV") == 0 || m.compare(0, 4, "PKGM") == 0);
    if (!ok_prefix) return false;
    for (size_t i = 4; i < 8; ++i) {
        if (m[i] < '0' || m[i] > '9') return false;
    }
    return true;
}

} // namespace

PkgIndex::~PkgIndex() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

PkgIndex::PkgIndex(PkgIndex&& other) noexcept
    : path_(std::move(other.path_)),
      magic_(std::move(other.magic_)),
      entries_(std::move(other.entries_)),
      fd_(other.fd_) {
    other.fd_ = -1;
}

PkgIndex& PkgIndex::operator=(PkgIndex&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        path_ = std::move(other.path_);
        magic_ = std::move(other.magic_);
        entries_ = std::move(other.entries_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

PkgIndex PkgIndex::Open(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, path.string());
    }
    PkgIndex idx;
    idx.path_ = path;
    idx.fd_ = fd;

    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 16) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, path.string());
    }
    uint64_t file_size = static_cast<uint64_t>(st.st_size);

    uint64_t cursor = 0;
    auto read_u32 = [&]() -> uint32_t {
        if (cursor + 4 > file_size) {
            throw ConvertError(MIRAGE_MOBILE_TRUNCATED_PACKAGE, path.string());
        }
        uint8_t buf[4];
        PreadExact(fd, buf, 4, cursor, MIRAGE_MOBILE_TRUNCATED_PACKAGE, path.string());
        cursor += 4;
        return ReadU32LE(buf);
    };

    uint32_t magic_len = read_u32();
    if (magic_len != 8 || cursor + magic_len > file_size) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, path.string());
    }
    std::string magic(magic_len, '\0');
    PreadExact(fd, reinterpret_cast<uint8_t*>(magic.data()), magic_len, cursor,
               MIRAGE_MOBILE_TRUNCATED_PACKAGE, path.string());
    cursor += magic_len;
    if (!IsValidMagic(magic, false)) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, magic);
    }
    idx.magic_ = std::move(magic);

    uint32_t entry_count = read_u32();
    if (entry_count > kMaxEntryCount ||
        static_cast<uint64_t>(entry_count) > file_size / kMinEntryHeaderBytes) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, path.string());
    }

    struct RawEntry {
        std::string name;
        uint64_t rel_offset = 0;
        uint64_t size = 0;
    };
    std::vector<RawEntry> raw;
    raw.reserve(entry_count);
    std::unordered_set<std::string> folded_names;
    folded_names.reserve(entry_count);

    for (uint32_t i = 0; i < entry_count; ++i) {
        uint32_t name_len = read_u32();
        if (name_len == 0 || name_len > 1024 || cursor + name_len + 8 > file_size) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, path.string());
        }
        std::string raw_name(name_len, '\0');
        PreadExact(fd, reinterpret_cast<uint8_t*>(raw_name.data()), name_len, cursor,
                   MIRAGE_MOBILE_TRUNCATED_PACKAGE, path.string());
        cursor += name_len;

        auto safe_name = NormalizeSafeArchivePath(raw_name);
        if (!safe_name) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, raw_name);
        }
        std::string folded = AsciiLower(*safe_name);
        if (!folded_names.insert(std::move(folded)).second) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, *safe_name);
        }

        uint64_t rel_offset = read_u32();
        uint64_t len = read_u32();
        raw.push_back(RawEntry{std::move(*safe_name), rel_offset, len});
    }

    uint64_t data_base = cursor;
    idx.entries_.reserve(raw.size());
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    ranges.reserve(raw.size());

    for (auto& r : raw) {
        if (r.rel_offset > file_size - data_base) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, r.name);
        }
        uint64_t start = data_base + r.rel_offset;
        if (r.size > file_size - start) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, r.name);
        }
        uint64_t end = start + r.size;
        idx.entries_.push_back(PkgIndexEntry{std::move(r.name), start, r.size});
        ranges.emplace_back(start, end);
    }

    // Reject overlapping payload ranges (untrusted Workshop package protection).
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].second) {
            throw ConvertError(MIRAGE_MOBILE_INVALID_PACKAGE, path.string());
        }
    }

    return idx;
}

std::optional<uint32_t> PkgIndex::version_number() const noexcept {
    if (magic_.size() != 8) return std::nullopt;
    uint32_t ver = 0;
    for (size_t i = 4; i < 8; ++i) {
        if (magic_[i] < '0' || magic_[i] > '9') return std::nullopt;
        ver = ver * 10U + static_cast<uint32_t>(magic_[i] - '0');
    }
    return ver;
}

const PkgIndexEntry* PkgIndex::FindEntry(std::string_view rel_path) const noexcept {
    for (const auto& e : entries_) {
        if (AsciiEqualsIgnoreCase(e.name, rel_path)) return &e;
    }
    return nullptr;
}

std::vector<uint8_t> PkgIndex::ReadEntry(const PkgIndexEntry& entry) const {
    if (entry.size > 512ULL * 1024ULL * 1024ULL) {
        throw ConvertError(MIRAGE_MOBILE_WALLPAPER_TOO_LARGE, entry.name);
    }
    std::vector<uint8_t> out(static_cast<size_t>(entry.size));
    if (entry.size > 0) {
        PreadExact(fd_, out.data(), out.size(), entry.absolute_offset,
                   MIRAGE_MOBILE_TRUNCATED_PACKAGE, entry.name);
    }
    return out;
}

std::vector<uint8_t> PkgIndex::ReadEntryPrefix(const PkgIndexEntry& entry, size_t max_bytes) const {
    size_t count = static_cast<size_t>(std::min<uint64_t>(entry.size, max_bytes));
    std::vector<uint8_t> out(count);
    if (count > 0) {
        PreadExact(fd_, out.data(), count, entry.absolute_offset,
                   MIRAGE_MOBILE_TRUNCATED_PACKAGE, entry.name);
    }
    return out;
}

MpkgWriteEntry MpkgWriteEntry::FromMemory(std::string name, std::vector<uint8_t> data) {
    MpkgWriteEntry e;
    e.name = std::move(name);
    e.is_file = false;
    e.memory_data = std::move(data);
    return e;
}

MpkgWriteEntry MpkgWriteEntry::FromFileSlice(
    std::string name,
    std::filesystem::path path,
    uint64_t offset,
    uint64_t size) {
    MpkgWriteEntry e;
    e.name = std::move(name);
    e.is_file = true;
    e.file_path = std::move(path);
    e.file_offset = offset;
    e.file_size = size;
    return e;
}

MpkgWriteEntry MpkgWriteEntry::FromFile(std::string name, std::filesystem::path path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PROJECT, name);
    }
    return FromFileSlice(std::move(name), std::move(path), 0, static_cast<uint64_t>(st.st_size));
}

uint32_t MapPkgToMpkgVersion(uint32_t pkg_ver, bool force_legacy) noexcept {
    if (pkg_ver <= 16) return force_legacy ? 20U : 14U;
    switch (pkg_ver) {
        case 17: return 15U;
        case 18: return 16U;
        case 19: return 17U;
        case 20: return 18U;
        case 21:
        case 22:
        case 23: return 19U;
        default: return 20U;
    }
}

std::string FormatMpkgMagic(uint32_t mpkg_ver) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "PKGM%04u", mpkg_ver);
    return std::string(buf);
}

uint64_t WriteMpkgArchive(
    const std::vector<MpkgWriteEntry>& entries,
    const std::string& version_magic,
    const std::filesystem::path& output_path,
    const std::function<void(uint64_t written, uint64_t total)>& progress,
    const volatile int32_t* cancel_flag) {
    if (entries.empty() || entries.size() > kMaxEntryCount || !IsValidMagic(version_magic, true)) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_PROJECT, "invalid archive metadata");
    }

    auto dest_parent = output_path.parent_path();
    if (!dest_parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dest_parent, ec);
        if (ec) {
            throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "cannot create output directory");
        }
    }

    auto canon_dest_parent = CanonicalPath(dest_parent.empty() ? std::filesystem::path(".") : dest_parent);
    std::filesystem::path resolved_dest = canon_dest_parent
        ? (*canon_dest_parent / output_path.filename())
        : output_path;

    std::unordered_set<std::string> names;
    names.reserve(entries.size());

    struct TableItem {
        const MpkgWriteEntry* entry = nullptr;
        uint32_t offset = 0;
    };
    std::vector<TableItem> table;
    table.reserve(entries.size());

    uint64_t payload_size = 0;
    uint64_t header_size = 8ULL + version_magic.size();
    constexpr uint64_t kMaxArchive = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());

    for (const auto& entry : entries) {
        if (!IsSafeEntryName(entry.name)) {
            throw ConvertError(MIRAGE_MOBILE_UNSAFE_PROJECT_PATH, entry.name);
        }
        std::string folded = AsciiLower(entry.name);
        if (!names.insert(folded).second) {
            throw ConvertError(MIRAGE_MOBILE_UNSAFE_PROJECT_PATH, entry.name);
        }
        uint64_t sz = entry.size();
        if (sz > kMaxArchive || payload_size > kMaxArchive - sz) {
            throw ConvertError(MIRAGE_MOBILE_WALLPAPER_TOO_LARGE, entry.name);
        }
        if (entry.is_file) {
            if (auto canon_src = CanonicalPath(entry.file_path); canon_src && *canon_src == resolved_dest) {
                throw ConvertError(MIRAGE_MOBILE_UNSAFE_PROJECT_PATH, entry.name);
            }
        }
        table.push_back(TableItem{&entry, static_cast<uint32_t>(payload_size)});
        payload_size += sz;
        header_size += 12ULL + entry.name.size();
    }

    // Prevent file and directory sharing the same path on the receiving device.
    for (const auto& folded : names) {
        size_t pos = folded.rfind('/');
        while (pos != std::string::npos) {
            if (names.count(folded.substr(0, pos)) > 0) {
                throw ConvertError(MIRAGE_MOBILE_UNSAFE_PROJECT_PATH, folded);
            }
            pos = folded.rfind('/', pos == 0 ? 0 : pos - 1);
            if (pos == 0) break;
        }
    }

    if (header_size > kMaxArchive || payload_size > kMaxArchive - header_size) {
        throw ConvertError(MIRAGE_MOBILE_WALLPAPER_TOO_LARGE, "archive exceeds 4 GiB");
    }

    // Build header in memory first and write in one shot.
    std::vector<uint8_t> header_bytes;
    header_bytes.reserve(static_cast<size_t>(header_size));
    AppendU32LE(header_bytes, static_cast<uint32_t>(version_magic.size()));
    header_bytes.insert(header_bytes.end(), version_magic.begin(), version_magic.end());
    AppendU32LE(header_bytes, static_cast<uint32_t>(table.size()));
    for (const auto& item : table) {
        AppendU32LE(header_bytes, static_cast<uint32_t>(item.entry->name.size()));
        header_bytes.insert(header_bytes.end(), item.entry->name.begin(), item.entry->name.end());
        AppendU32LE(header_bytes, item.offset);
        AppendU32LE(header_bytes, static_cast<uint32_t>(item.entry->size()));
    }

    // Same-directory unique temporary file for atomic rename().
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t nonce = gen();
    std::filesystem::path tmp_path = (dest_parent.empty() ? std::filesystem::path(".") : dest_parent) /
        ("." + output_path.filename().string() + "." + std::to_string(::getpid()) + "." + std::to_string(nonce) + ".tmp");

    int out_fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out_fd < 0) {
        throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "cannot create temp output: " + tmp_path.string());
    }

    struct TempCleanup {
        std::filesystem::path path;
        int fd = -1;
        bool committed = false;
        ~TempCleanup() {
            if (fd >= 0) ::close(fd);
            if (!committed) ::unlink(path.c_str());
        }
    } guard{tmp_path, out_fd, false};

    WriteAllFd(out_fd, header_bytes.data(), header_bytes.size(), tmp_path.string());

    if (progress) progress(0, payload_size);
    uint64_t written = 0;
    std::vector<uint8_t> io_buf;

    // Cache open read FD across consecutive slices from the same source .pkg file.
    std::filesystem::path cached_path;
    int cached_fd = -1;
    struct CachedFdGuard {
        int& fd;
        ~CachedFdGuard() { if (fd >= 0) ::close(fd); }
    } cached_guard{cached_fd};

    for (const auto& item : table) {
        CheckCancelled(cancel_flag);
        const auto& entry = *item.entry;
        if (!entry.is_file) {
            if (!entry.memory_data.empty()) {
                WriteAllFd(out_fd, entry.memory_data.data(), entry.memory_data.size(), entry.name);
                written += entry.memory_data.size();
            }
            if (progress) progress(written, payload_size);
        } else {
            if (entry.file_size > 0) {
                if (cached_fd < 0 || cached_path != entry.file_path) {
                    if (cached_fd >= 0) {
                        ::close(cached_fd);
                        cached_fd = -1;
                    }
                    cached_fd = ::open(entry.file_path.c_str(), O_RDONLY);
                    if (cached_fd < 0) {
                        throw ConvertError(MIRAGE_MOBILE_IO_ERROR, entry.name);
                    }
                    cached_path = entry.file_path;
                }
                if (io_buf.empty()) io_buf.resize(kStreamChunkBytes);
                uint64_t remaining = entry.file_size;
                uint64_t cur_off = entry.file_offset;
                while (remaining > 0) {
                    CheckCancelled(cancel_flag);
                    size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, io_buf.size()));
                    PreadExact(cached_fd, io_buf.data(), chunk, cur_off,
                               MIRAGE_MOBILE_TRUNCATED_PACKAGE, entry.name);
                    WriteAllFd(out_fd, io_buf.data(), chunk, entry.name);
                    cur_off += chunk;
                    remaining -= chunk;
                    written += chunk;
                    if (progress) progress(written, payload_size);
                }
            } else {
                if (progress) progress(written, payload_size);
            }
        }
    }

    if (::fsync(out_fd) != 0) {
        throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "fsync failed");
    }
    if (::close(out_fd) != 0) {
        guard.fd = -1;
        throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "close failed");
    }
    guard.fd = -1;

    if (::rename(tmp_path.c_str(), output_path.c_str()) != 0) {
        throw ConvertError(MIRAGE_MOBILE_IO_ERROR, "atomic rename failed");
    }
    guard.committed = true;

    return header_size + payload_size;
}

} // namespace mirage::mobile

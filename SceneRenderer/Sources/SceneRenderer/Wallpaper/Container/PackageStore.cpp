module;

#include <rstd/macro.hpp>

module sr.pkg_fs;
import sr.core;
import rstd;
import rstd.log;
import rstd.cppstd;

import sr.fs;

using namespace sr;
using namespace sr::fs;

namespace
{
std::optional<std::string> ReadSizedString(IBinaryStream& f, usize max_len) {
    idx ilen = f.ReadInt32();
    if (ilen < 0) return std::nullopt;

    usize len = (usize)ilen;
    if (len > max_len) return std::nullopt;
    std::string result;
    result.resize(len);
    if (f.Read(result.data(), len) != len) return std::nullopt;
    return result;
}

bool IsPkgVersionStamp(std::string_view stamp) {
    constexpr std::string_view kPrefix = "PKGV";
    return stamp.size() > kPrefix.size() && stamp.substr(0, kPrefix.size()) == kPrefix;
}

// WE pkgs were authored on Windows where NTFS is case-insensitive; some
// shaders reference `effects/foo` while the pkg stores `Effects/foo`. Lower
// every path going through the map so lookups match regardless of case.
std::string LowerPath(std::string_view p) {
    std::string s(p);
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

// Builds the rooted component path used as the pkg map key. `..` is resolved
// by popping the previous component instead of being preserved, so `a/../b`
// and `b` name the same entry and a leading `..` cannot walk out of the
// package (pop() is a no-op once we are back at "/"). Resolving rather than
// dropping keeps producer and consumer consistent: both the keys stored from
// the pkg header and the keys built from lookup paths go through here.
std::string RootedComponentPath(RstdPath path) {
    auto out        = rstd::path::PathBuf::from("/");
    auto components = path.components();
    while (true) {
        auto component = components.next();
        if (component.is_none()) break;
        if ((*component).is_root_dir() || (*component).is_cur_dir()) continue;
        if ((*component).is_parent_dir()) {
            (void)out.pop();
            continue;
        }
        out.push(RstdPath((*component).as_os_str()));
    }
    return ToStdString(out.as_path());
}

std::string PkgLookupKey(RstdPath path) { return LowerPath(RootedComponentPath(path)); }
} // namespace

std::unique_ptr<WPPkgFs> WPPkgFs::CreatePkgFs(std::string_view pkgpath,
                                              bool load_from_memory) {
    std::shared_ptr<std::vector<uint8_t>> memory_data;
    std::shared_ptr<IBinaryStream> ppkg;
    if (load_from_memory) {
        auto disk = fs::CreateCBinaryStream(pkgpath);
        if (! disk || disk->Size() < 0) return nullptr;
        memory_data = std::make_shared<std::vector<uint8_t>>((usize)disk->Size());
        if (disk->Read(memory_data->data(), memory_data->size()) != memory_data->size()) {
            return nullptr;
        }
        ppkg = std::make_shared<SharedMemBinaryStream>(memory_data);
        rstd_info("loaded pkg into memory: {} ({} bytes)", pkgpath, memory_data->size());
    } else {
        ppkg = fs::CreateCBinaryStream(pkgpath);
        if (! ppkg) return nullptr;
    }

    auto& pkg       = *ppkg;
    auto  maybe_ver = ReadSizedString(pkg, 64);
    if (! maybe_ver || ! IsPkgVersionStamp(*maybe_ver)) return nullptr;
    std::string ver = std::move(*maybe_ver);
    rstd_info("pkg version: {}", ver);

    const isize signed_size = pkg.Size();
    if (signed_size < 0) return nullptr;
    const u64 file_size = static_cast<u64>(signed_size);

    std::vector<PkgFile> pkgfiles;
    i32                  entryCount = pkg.ReadInt32();
    if (entryCount < 0) return nullptr;
    // On disk every entry costs at least a 4 byte path length, one path byte
    // and the 4 byte offset/length pair, so a pkg of this size cannot possibly
    // describe more than file_size/13 entries. Without this bound a ~20 byte
    // pkg claiming 0x7FFFFFFF entries makes us push_back two billion structs
    // before any read fails (ReadInt32 returns 0 at EOF and ReadSizedString
    // happily yields an empty string).
    constexpr u64 kMinEntrySize = 4 + 1 + 4 + 4;
    if (static_cast<u64>(entryCount) > file_size / kMinEntrySize) {
        rstd_error("pkg declares {} entries, too many for a {} byte pkg", entryCount, file_size);
        return nullptr;
    }
    // The count is bounded by the file size now, but a big pkg could still
    // name millions of entries, so reserve a fixed amount rather than the
    // declared one; the vector grows on its own if the entries are real.
    constexpr usize kInitialEntryReserve = 1024;
    pkgfiles.reserve(std::min<usize>(static_cast<usize>(entryCount), kInitialEntryReserve));
    for (i32 i = 0; i < entryCount; i++) {
        auto maybe_path = ReadSizedString(pkg, 4096);
        if (! maybe_path) return nullptr;
        std::string path   = RootedComponentPath(ToPath(*maybe_path));
        idx         offset = pkg.ReadInt32();
        idx         length = pkg.ReadInt32();
        if (offset < 0 || length < 0) return nullptr;
        pkgfiles.push_back({ path, offset, length });
    }
    auto pkgfs           = std::unique_ptr<WPPkgFs>(new WPPkgFs());
    pkgfs->m_pkgPath     = pkgpath;
    pkgfs->m_pkgData     = std::move(memory_data);
    pkgfs->m_pkg_version = std::move(ver);
    idx headerSize       = pkg.Tell();
    if (headerSize < 0) return nullptr;
    for (auto& el : pkgfiles) {
        // Entry offsets are relative to the end of the header. Check the
        // absolute [begin, end) range against the real file size; offset and
        // length are non-negative i32 and headerSize <= file_size, so the u64
        // sum cannot overflow.
        const u64 begin = static_cast<u64>(headerSize) + static_cast<u64>(el.offset);
        const u64 end   = begin + static_cast<u64>(el.length);
        if (end > file_size) {
            rstd_error("pkg entry \"{}\" range [{}, {}) outside the {} byte pkg",
                       el.path,
                       begin,
                       end,
                       file_size);
            return nullptr;
        }
        el.offset += headerSize;
        pkgfs->m_files.insert({ LowerPath(el.path), el });
    }
    return pkgfs;
}

bool WPPkgFs::Contains(RstdPath path) const { return m_files.count(PkgLookupKey(path)) > 0; }

std::shared_ptr<IBinaryStream> WPPkgFs::Open(RstdPath path) {
    auto it = m_files.find(PkgLookupKey(path));
    if (it != m_files.end()) {
        if (m_pkgData) {
            return std::make_shared<SharedMemBinaryStream>(
                m_pkgData, it->second.offset, it->second.length);
        }
        auto pkg = fs::CreateCBinaryStream(m_pkgPath);
        if (! pkg) return nullptr;
        return std::make_shared<LimitedBinaryStream>(pkg, it->second.offset, it->second.length);
    }
    return nullptr;
}

std::shared_ptr<IBinaryStreamW> WPPkgFs::OpenW(RstdPath) { return nullptr; }

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

std::string RootedComponentPath(RstdPath path) {
    auto out        = rstd::path::PathBuf::from("/");
    auto components = path.components();
    while (true) {
        auto component = components.next();
        if (component.is_none()) break;
        if ((*component).is_root_dir() || (*component).is_cur_dir()) continue;
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

    std::vector<PkgFile> pkgfiles;
    i32                  entryCount = pkg.ReadInt32();
    if (entryCount < 0) return nullptr;
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
    for (auto& el : pkgfiles) {
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

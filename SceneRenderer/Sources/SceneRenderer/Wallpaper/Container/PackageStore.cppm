module;

export module sr.pkg_fs;
import sr.core;
import rstd;
import rstd.cppstd;

export import sr.fs;

export namespace sr::fs
{

class WPPkgFs : public Fs {
public:
    virtual ~WPPkgFs() = default;
    static std::unique_ptr<WPPkgFs> CreatePkgFs(std::string_view pkgpath,
                                                bool load_from_memory = false);

private:
    WPPkgFs() = default;

public:
    using Fs::Contains;
    using Fs::Open;
    using Fs::OpenW;

    bool                            Contains(RstdPath path) const override;
    std::shared_ptr<IBinaryStream>  Open(RstdPath path) override;
    std::shared_ptr<IBinaryStreamW> OpenW(RstdPath path) override;

    // Pkg-format version stamp from the binary header (e.g. "PKGV0023").
    // Empty if the pkg was malformed.
    std::string_view pkg_version_stamp() const noexcept { return m_pkg_version; }

private:
    struct PkgFile {
        std::string path;

        idx offset { 0 };
        idx length { 0 };
    };
    std::string                              m_pkgPath;
    std::shared_ptr<const std::vector<uint8_t>> m_pkgData;
    std::string                              m_pkg_version;
    std::unordered_map<std::string, PkgFile> m_files;
};

} // namespace sr::fs

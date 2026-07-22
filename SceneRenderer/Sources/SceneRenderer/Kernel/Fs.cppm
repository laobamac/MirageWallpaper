module;

#include <rstd/macro.hpp>
#include <cstdio>
#include <cstring>

export module sr.fs;
import sr.core;
import rstd;
import rstd.log;
import rstd.cppstd;

export namespace sr::fs
{

using RstdPath = rstd::ref<rstd::path::Path>;

inline RstdPath ToPath(std::string_view path) { return RstdPath(rstd::ref<rstd::str>(path)); }

inline std::string ToStdString(RstdPath path) {
    return std::string(reinterpret_cast<const char*>(path.data()), path.len());
}

// -- Bswap -----------------------------------------------------------------

template<typename T>
constexpr T bswap(T source);

template<>
inline constexpr uint64_t bswap<uint64_t>(uint64_t source) {
    return 0 | ((source & uint64_t(0x00000000000000ffull)) << 56) |
           ((source & uint64_t(0x000000000000ff00ull)) << 40) |
           ((source & uint64_t(0x0000000000ff0000ull)) << 24) |
           ((source & uint64_t(0x00000000ff000000ull)) << 8) |
           ((source & uint64_t(0x000000ff00000000ull)) >> 8) |
           ((source & uint64_t(0x0000ff0000000000ull)) >> 24) |
           ((source & uint64_t(0x00ff000000000000ull)) >> 40) |
           ((source & uint64_t(0xff00000000000000ull)) >> 56);
}

template<>
inline constexpr uint32_t bswap<uint32_t>(uint32_t source) {
    return 0 | ((source & 0x000000ff) << 24) | ((source & 0x0000ff00) << 8) |
           ((source & 0x00ff0000) >> 8) | ((source & 0xff000000) >> 24);
}

template<>
inline constexpr uint16_t bswap<uint16_t>(uint16_t source) {
    return 0 | (uint16_t)((source & 0x00ff) << 8) | (uint16_t)((source & 0xff00) >> 8);
}

template<>
inline constexpr uint8_t bswap<uint8_t>(uint8_t source) {
    return source;
}

template<>
inline constexpr int64_t bswap<int64_t>(int64_t source) {
    return (int64_t)(bswap<uint64_t>((uint64_t)(source)));
}
template<>
inline constexpr int32_t bswap<int32_t>(int32_t source) {
    return (int32_t)(bswap<uint32_t>((uint32_t)(source)));
}
template<>
inline constexpr int16_t bswap<int16_t>(int16_t source) {
    return (int16_t)(bswap<uint16_t>((uint16_t)(source)));
}
template<>
inline constexpr int8_t bswap<int8_t>(int8_t source) {
    return (int8_t)(bswap<uint8_t>((uint8_t)(source)));
}

// -- IBinaryStream ---------------------------------------------------------

class IBinaryStream : NoCopy, NoMove {
public:
    enum class ByteOrder
    {
        BigEndian,
        LittleEndian
    };

    constexpr static ByteOrder sys_byte_order {
#ifdef SCENERENDERER_BIG_ENDIAN
        ByteOrder::BigEndian
#else
        ByteOrder::LittleEndian
#endif
    };

protected:
    template<typename T>
    T _ReadInt() {
        T x { 0 };
        if (Read(reinterpret_cast<char*>(&x), sizeof(x)) != sizeof(x)) {
            x = T { 0 };
        } else {
            if (! m_noswap) {
                x = bswap<T>(x);
            }
        }
        return x;
    }
    template<typename T>
    bool _WriteInt(T x) {
        if (! m_noswap) {
            x = bswap<T>(x);
        }
        return Write_impl(reinterpret_cast<char*>(&x), sizeof(x)) != sizeof(x);
    }

public:
    IBinaryStream()          = default;
    virtual ~IBinaryStream() = default;

    void SetByteOrder(ByteOrder order) {
        m_byte_order = order;
        m_noswap     = order == sys_byte_order;
    }

    float ReadFloat() {
        float x { 0 };
        Read(reinterpret_cast<char*>(&x), sizeof(x));
        return x;
    }

    i64 ReadInt64() { return _ReadInt<i64>(); }
    u64 ReadUint64() { return _ReadInt<u64>(); }

    i32 ReadInt32() { return _ReadInt<i32>(); }
    u32 ReadUint32() { return _ReadInt<u32>(); }

    i16 ReadInt16() { return _ReadInt<i16>(); }
    u16 ReadUint16() { return _ReadInt<u16>(); }

    i8 ReadInt8() { return _ReadInt<i8>(); }
    u8 ReadUint8() { return _ReadInt<u8>(); }

    std::string ReadStr() {
        std::string str;
        char        c;
        while (Read(&c, 1)) {
            if (c == '\0') break;
            str.push_back(c);
        }
        return str;
    }

    std::string ReadAllStr() {
        std::string str;
        str.resize(Usize());
        Read(str.data(), std::size(str));
        return str;
    }

    bool Rewind() { return SeekSet(0); }

    usize Usize() const noexcept { return static_cast<usize>(Size()); }

public:
    virtual usize Read(void* buffer, usize sizeInByte) = 0;
    virtual char* Gets(char* buffer, usize sizeStr)    = 0;
    virtual idx   Tell() const                         = 0;
    virtual bool  SeekSet(idx offset)                  = 0;
    virtual bool  SeekCur(idx offset)                  = 0;
    virtual bool  SeekEnd(idx offset)                  = 0;

    virtual isize Size() const = 0;

protected:
    virtual usize Write_impl(const void* buffer, usize sizeInByte) = 0;

private:
    constexpr static ByteOrder default_byte_order { ByteOrder::LittleEndian };
    ByteOrder                  m_byte_order { default_byte_order };
    bool                       m_noswap { sys_byte_order == default_byte_order };
};

class IBinaryStreamW : IBinaryStream {
public:
    virtual ~IBinaryStreamW() = default;
    usize Write(const void* buffer, usize sizeInByte) { return Write_impl(buffer, sizeInByte); }
    i32   WriteInt32(i32 x) { return _WriteInt<i32>(x); }
    i32   WriteUint32(u32 x) { return _WriteInt<u32>(x); }
};

// -- Fs (abstract) ---------------------------------------------------------

class Fs : NoCopy, NoMove {
public:
    virtual bool                            Contains(RstdPath path) const = 0;
    virtual std::shared_ptr<IBinaryStream>  Open(RstdPath path)           = 0;
    virtual std::shared_ptr<IBinaryStreamW> OpenW(RstdPath path)          = 0;

    bool Contains(const char* path) const { return Contains(ToPath(path)); }
    bool Contains(std::string_view path) const { return Contains(ToPath(path)); }
    std::shared_ptr<IBinaryStream>  Open(const char* path) { return Open(ToPath(path)); }
    std::shared_ptr<IBinaryStream>  Open(std::string_view path) { return Open(ToPath(path)); }
    std::shared_ptr<IBinaryStreamW> OpenW(const char* path) { return OpenW(ToPath(path)); }
    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view path) { return OpenW(ToPath(path)); }

public:
    Fs()          = default;
    virtual ~Fs() = default;
};

// -- CBinaryStream ---------------------------------------------------------

template<typename TBinaryStream>
class CBinaryStream : public TBinaryStream {
public:
    virtual ~CBinaryStream() { std::fclose(m_file); }

protected:
    CBinaryStream(std::string_view path, std::FILE* file): m_path(path), m_file(file) {}
    virtual usize Write_impl(const void* buffer, usize sizeInBytes) override {
        return std::fwrite(buffer, sizeInBytes, 1, m_file);
    }

public:
    virtual usize Read(void* buffer, usize sizeInBytes) override {
        return sizeInBytes * std::fread(buffer, sizeInBytes, 1, m_file);
    }
    virtual char* Gets(char* buffer, usize sizeStr) override {
        rstd_assert(sizeStr < std::numeric_limits<int>::max());
        return std::fgets(buffer, (int)sizeStr, m_file);
    }
    virtual idx   Tell() const override { return std::ftell(m_file); }
    virtual bool  SeekSet(idx offset) override { return std::fseek(m_file, offset, SEEK_SET) == 0; }
    virtual bool  SeekCur(idx offset) override { return std::fseek(m_file, offset, SEEK_CUR) == 0; }
    virtual bool  SeekEnd(idx offset) override { return std::fseek(m_file, offset, SEEK_END) == 0; }
    virtual isize Size() const override {
        idx cur = std::ftell(m_file);
        std::fseek(m_file, 0, SEEK_END);
        idx size = std::ftell(m_file);
        std::fseek(m_file, cur, SEEK_SET);
        return size;
    }

private:
    std::string m_path;
    std::FILE*  m_file;
};

template<typename TBinaryStream>
inline std::shared_ptr<TBinaryStream> t_CreateCBinaryStream(std::string_view path,
                                                            const char*      mode) {
    struct Shared : public CBinaryStream<TBinaryStream> {
        Shared(std::string_view path, std::FILE* file): CBinaryStream<TBinaryStream>(path, file) {};
    };
    if (std::filesystem::is_directory(path)) {
        rstd_error("can't open: \'{}\', which is a directory", path);
        return nullptr;
    }
    auto* file = std::fopen(std::string(path).c_str(), mode);
    if (file == NULL) {
        rstd_error("can't open: {}", path);
        return nullptr;
    }
    auto cb = std::make_shared<Shared>(path, file);
    return cb;
}

inline std::shared_ptr<IBinaryStream> CreateCBinaryStream(std::string_view path) {
    return t_CreateCBinaryStream<IBinaryStream>(path, "rb");
}
inline std::shared_ptr<IBinaryStreamW> CreateCBinaryStreamW(std::string_view path) {
    return t_CreateCBinaryStream<IBinaryStreamW>(path, "wb+");
}

// -- MemBinaryStream -------------------------------------------------------

class MemBinaryStream : public IBinaryStream {
public:
    MemBinaryStream(std::vector<uint8_t>&& data): m_pos(0), m_data(std::move(data)) {}
    MemBinaryStream(IBinaryStream& f): m_pos(0) {
        m_data = std::vector<uint8_t>((usize)f.Size());
        f.Read(m_data.data(), m_data.size());
    }

    virtual ~MemBinaryStream() = default;

public:
    virtual usize Read(void* buffer, usize sizeInByte) {
        auto start_pos = m_data.begin() + m_pos;
        idx  moved     = moveForward((idx)sizeInByte);
        std::copy(start_pos, start_pos + moved, (uint8_t*)buffer);
        return (usize)moved;
    }
    virtual char* Gets(char* buffer, usize sizeStr) {
        Read(buffer, sizeStr);
        return buffer;
    }
    virtual idx  Tell() const { return m_pos; }
    virtual bool SeekSet(idx offset) {
        if (InArea(offset)) {
            m_pos = offset;
            return true;
        }
        return false;
    }
    virtual bool SeekCur(idx offset) {
        idx new_pos = m_pos + offset;
        if (InArea(new_pos)) {
            m_pos = new_pos;
            return true;
        }
        return false;
    }
    virtual bool SeekEnd(idx offset) {
        idx new_pos = Size() + offset;
        if (InArea(new_pos)) {
            m_pos = new_pos;
            return true;
        }
        return false;
    }
    virtual isize Size() const { return std::ssize(m_data); }

protected:
    virtual usize Write_impl(const void*, usize) { return 0; }

private:
    bool InArea(idx pos) const noexcept { return pos >= 0 && pos <= Size(); }
    idx  moveForward(idx step) noexcept {
        idx end     = m_pos + step;
        end         = end > Size() ? Size() : end;
        idx stepped = end - m_pos;
        m_pos       = end;
        return stepped;
    };

    idx                  m_pos;
    std::vector<uint8_t> m_data;
};

// Read-only cursor over shared immutable bytes. Each caller gets an independent
// cursor while every stream keeps the same backing allocation alive.
class SharedMemBinaryStream : public IBinaryStream {
public:
    SharedMemBinaryStream(std::shared_ptr<const std::vector<uint8_t>> data,
                          idx start = 0, isize length = -1)
        : m_data(std::move(data)), m_start(start), m_pos(0) {
        const isize available = m_data && start >= 0 && (usize)start <= m_data->size()
                                    ? (isize)m_data->size() - start
                                    : 0;
        m_size = length < 0 ? available : std::clamp<isize>(length, 0, available);
    }

    usize Read(void* buffer, usize sizeInByte) override {
        const isize remaining = m_size - m_pos;
        const usize count = std::min<usize>(sizeInByte, remaining > 0 ? (usize)remaining : 0);
        if (count > 0) {
            std::memcpy(buffer, m_data->data() + m_start + m_pos, count);
            m_pos += (idx)count;
        }
        return count;
    }
    char* Gets(char* buffer, usize sizeStr) override {
        Read(buffer, sizeStr);
        return buffer;
    }
    idx Tell() const override { return m_pos; }
    bool SeekSet(idx offset) override {
        if (offset < 0 || offset > m_size) return false;
        m_pos = offset;
        return true;
    }
    bool SeekCur(idx offset) override { return SeekSet(m_pos + offset); }
    bool SeekEnd(idx offset) override { return SeekSet(m_size + offset); }
    isize Size() const override { return m_size; }

protected:
    usize Write_impl(const void*, usize) override { return 0; }

private:
    std::shared_ptr<const std::vector<uint8_t>> m_data;
    idx m_start { 0 };
    idx m_pos { 0 };
    isize m_size { 0 };
};

// -- LimitedBinaryStream ---------------------------------------------------

class LimitedBinaryStream : public IBinaryStream {
public:
    LimitedBinaryStream(std::shared_ptr<IBinaryStream> infs, idx start, isize size)
        : m_pos(0), m_start(start), m_end(start + size), m_infs(infs) {}
    virtual ~LimitedBinaryStream() = default;

private:
    bool CheckInArea(idx pos) const { return pos > 0 && pos <= Size(); }

    bool SeekInMPos(void) { return m_infs->SeekSet(m_start + m_pos); }
    bool SeekInPos(idx pos) {
        if (CheckInArea(pos)) {
            m_pos = pos;
            return SeekInMPos();
        }
        return false;
    }
    bool End() const { return m_pos < 0 || m_pos == Size(); };

protected:
    virtual usize Write_impl(const void*, usize) { return 0; }

public:
    virtual usize Read(void* buffer, usize sizeInByte) {
        if (End()) return 0;

        isize isizeInByte = (isize)sizeInByte;

        if (! CheckInArea(m_pos + isizeInByte)) {
            isizeInByte = Size() - m_pos;
        }
        SeekInMPos();
        m_pos += isizeInByte;

        return m_infs->Read(buffer, (usize)isizeInByte);
    }
    virtual char* Gets(char* buffer, usize sizeStr) {
        Read(buffer, sizeStr);
        return buffer;
    }
    virtual idx  Tell() const { return m_pos; }
    virtual bool SeekSet(idx offset) {
        idx pos = offset;
        return SeekInPos(pos);
    }
    virtual bool SeekCur(idx offset) {
        idx pos = m_pos + offset;
        return SeekInPos(pos);
    }
    virtual bool SeekEnd(idx offset) {
        idx pos = Size() - 1 - offset;
        return SeekInPos(pos);
    }
    virtual isize Size() const { return m_end - m_start; }

private:
    idx                            m_pos;
    const idx                      m_start;
    const idx                      m_end;
    std::shared_ptr<IBinaryStream> m_infs;
};

// -- VFS -------------------------------------------------------------------

class VFS : NoCopy, NoMove {
public:
    struct MountedFs {
        std::string         name;
        rstd::path::PathBuf mountPoint;
        std::unique_ptr<Fs> fs;
        static bool         CheckMountPoint(RstdPath mountPoint) {
            auto components = mountPoint.components();
            auto root       = components.next();
            if (root.is_none() || ! (*root).is_root_dir()) return false;
            return components.next().is_some();
        }
        static bool SamePath(RstdPath lhs, RstdPath rhs) {
            return lhs.starts_with(rhs) && rhs.starts_with(lhs);
        }
        RstdPath               path() const { return mountPoint.as_path(); }
        rstd::Option<RstdPath> PathInMount(RstdPath path) const {
            return path.strip_prefix(mountPoint.as_path());
        }
    };

public:
    VFS()  = default;
    ~VFS() = default;

    bool Mount(const char* mountpoint, std::unique_ptr<Fs> fs, std::string_view name = "") {
        return Mount(ToPath(mountpoint), std::move(fs), name);
    }
    bool Mount(std::string_view mountpoint, std::unique_ptr<Fs> fs, std::string_view name = "") {
        return Mount(ToPath(mountpoint), std::move(fs), name);
    }
    bool Mount(RstdPath mountpoint, std::unique_ptr<Fs> fs, std::string_view name = "") {
        if (! MountedFs::CheckMountPoint(mountpoint) || ! fs) return false;

        m_mountedFss.push_back(
            { std::string(name), rstd::path::PathBuf::from(mountpoint), std::move(fs) });
        return true;
    }
    bool Unmount(std::string_view mountpoint) { return Unmount(ToPath(mountpoint)); }
    bool Unmount(const char* mountpoint) { return Unmount(ToPath(mountpoint)); }
    bool Unmount(RstdPath mountpoint) {
        for (auto iter = m_mountedFss.rbegin(); iter < m_mountedFss.rend(); iter++) {
            if (MountedFs::SamePath(iter->path(), mountpoint)) {
                m_mountedFss.erase((++iter).base());
                return true;
            }
        }
        rstd_info("mount point not exist");
        return false;
    }
    bool IsMounted(std::string_view name) {
        for (const auto& el : m_mountedFss) {
            if (el.name == name) return true;
        }
        return false;
    }
    std::shared_ptr<IBinaryStream> Open(std::string_view path) { return Open(ToPath(path)); }
    std::shared_ptr<IBinaryStream> Open(const char* path) { return Open(ToPath(path)); }
    std::shared_ptr<IBinaryStream> Open(RstdPath path) {
        for (auto iter = m_mountedFss.rbegin(); iter < m_mountedFss.rend(); iter++) {
            auto mounted_path = iter->PathInMount(path);
            if (mounted_path.is_none() || ! iter->fs->Contains(*mounted_path)) continue;
            return iter->fs->Open(*mounted_path);
        }
        rstd_error("not found \"{}\" in vfs", path);
        return nullptr;
    }
    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view path) { return OpenW(ToPath(path)); }
    std::shared_ptr<IBinaryStreamW> OpenW(const char* path) { return OpenW(ToPath(path)); }
    std::shared_ptr<IBinaryStreamW> OpenW(RstdPath path) {
        for (auto iter = m_mountedFss.rbegin(); iter < m_mountedFss.rend(); iter++) {
            auto mounted_path = iter->PathInMount(path);
            if (mounted_path.is_none() || ! iter->fs->Contains(*mounted_path)) continue;
            return iter->fs->OpenW(*mounted_path);
        }
        for (auto iter = m_mountedFss.rbegin(); iter < m_mountedFss.rend(); iter++) {
            auto mounted_path = iter->PathInMount(path);
            if (mounted_path.is_some()) return iter->fs->OpenW(*mounted_path);
        }
        rstd_error("not found \"{}\" in vfs", path);
        return nullptr;
    }
    bool Contains(std::string_view path) const { return Contains(ToPath(path)); }
    bool Contains(const char* path) const { return Contains(ToPath(path)); }
    bool Contains(RstdPath path) const {
        for (auto iter = m_mountedFss.rbegin(); iter < m_mountedFss.rend(); iter++) {
            auto& el           = *iter;
            auto  mounted_path = el.PathInMount(path);
            if (mounted_path.is_some() && el.fs->Contains(*mounted_path)) return true;
        }
        return false;
    }

private:
    std::vector<MountedFs> m_mountedFss;
};

inline std::string GetFileContent(VFS& vfs, std::string_view path) {
    auto f = vfs.Open(path);
    if (f) return f->ReadAllStr();
    return "";
}

// -- PhysicalFs ------------------------------------------------------------

class PhysicalFs : public Fs {
public:
    PhysicalFs(std::string_view physicalPath): m_path(physicalPath) {}
    virtual ~PhysicalFs() = default;
    using Fs::Contains;
    using Fs::Open;
    using Fs::OpenW;

    bool Contains(RstdPath path) const override {
        auto fullpath = FullPath(path);
        return std::filesystem::exists(fullpath);
    }
    std::shared_ptr<IBinaryStream> Open(RstdPath path) override {
        return CreateCBinaryStream(FullPath(path));
    }
    std::shared_ptr<IBinaryStreamW> OpenW(RstdPath path) override {
        std::filesystem::path full_path { FullPath(path) };
        std::filesystem::create_directories(full_path.parent_path());
        return CreateCBinaryStreamW(full_path.native());
    }

private:
    std::string FullPath(RstdPath path) const {
        auto local_path = path;
        if (path.has_root()) {
            auto stripped = path.strip_prefix(RstdPath("/"));
            if (stripped.is_some()) local_path = *stripped;
        }
        auto fullpath = m_path / ToStdString(local_path);
        return fullpath.string();
    }
    std::filesystem::path m_path;
};

inline std::unique_ptr<PhysicalFs> CreatePhysicalFs(std::string_view path, bool create = false) {
    if (! std::filesystem::exists(path)) {
        if (create) {
            if (! std::filesystem::create_directories(path)) {
                rstd_error("mkdir \"{}\" failed", path);
                return nullptr;
            }
        } else {
            rstd_error("\"{}\" not exists", path);
            return nullptr;
        }
    }
    return std::make_unique<PhysicalFs>(path);
}

} // namespace sr::fs

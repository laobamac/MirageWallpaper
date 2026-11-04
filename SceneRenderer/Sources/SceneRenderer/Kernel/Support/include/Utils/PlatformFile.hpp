#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace sr::platform {

inline std::filesystem::path NativePath(std::string_view utf8) {
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

inline std::string Utf8Path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline std::uint32_t ProcessId() {
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

inline std::FILE* OpenFile(std::string_view path, const char* mode) {
#if defined(_WIN32)
    // 只读句柄带上 FILE_SHARE_DELETE，让读取期间允许其它写入方替换文件
    // （CRT 的 fopen 默认不带该共享位，与 POSIX 的 rename 语义不一致）。
    const bool read_only = mode[0] == 'r' && std::char_traits<char>::find(mode, std::char_traits<char>::length(mode), '+') == nullptr;
    if (read_only) {
        HANDLE handle = ::CreateFileW(NativePath(path).c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return nullptr;
        const int binary = std::char_traits<char>::find(mode, std::char_traits<char>::length(mode), 'b') != nullptr
                               ? _O_BINARY
                               : _O_TEXT;
        const int descriptor = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_RDONLY | binary);
        if (descriptor < 0) {
            ::CloseHandle(handle);
            return nullptr;
        }
        std::FILE* file = ::_fdopen(descriptor, mode);
        if (file == nullptr) ::_close(descriptor);
        return file;
    }
    const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
    std::FILE* file = nullptr;
    if (::_wfopen_s(&file, NativePath(path).c_str(), wide_mode.c_str()) != 0) return nullptr;
    return file;
#else
    return std::fopen(std::string(path).c_str(), mode);
#endif
}

inline std::FILE* OpenExclusiveFile(const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    const int fd = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_BINARY);
    if (fd < 0) { ::CloseHandle(handle); return nullptr; }
    std::FILE* file = ::_fdopen(fd, "wb");
    if (!file) ::_close(fd);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return nullptr;
    std::FILE* file = ::fdopen(fd, "wb");
    if (!file) ::close(fd);
#endif
    return file;
}

inline bool RenameFileReplace(const std::filesystem::path& source,
                              const std::filesystem::path& target) {
#if defined(_WIN32)
    // MoveFileEx 在目标文件仍被打开时会返回 ERROR_ACCESS_DENIED（即使对方共享了
    // 删除位），而 POSIX 的 rename 允许读取期间替换。Win10 1709 起的
    // FileRenameInfoEx + POSIX 语义提供等价行为：以 DELETE 权限打开源文件后改名，
    // 目标句柄继续指向旧文件对象，目录项立刻切换到新文件。
    HANDLE source_handle = ::CreateFileW(source.c_str(), DELETE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (source_handle == INVALID_HANDLE_VALUE) return false;

    const std::wstring target_name = target.wstring();
    const auto name_bytes = static_cast<std::uint32_t>(target_name.size() * sizeof(wchar_t));
    // FILE_RENAME_INFORMATION 布局：Flags(4) 补齐(4) RootDirectory(ptr) FileNameLength(4) FileName。
    struct alignas(8) RenameInformation {
        std::uint32_t flags;
        std::uint32_t reserved;
        void*         root_directory;
        std::uint32_t name_bytes;
        wchar_t       name[1];
    };
    std::vector<std::uint8_t> buffer(sizeof(RenameInformation) + name_bytes);
    auto* info            = reinterpret_cast<RenameInformation*>(buffer.data());
    info->flags           = 0x1u | 0x2u; // FILE_RENAME_FLAG_REPLACE_IF_EXISTS | POSIX_SEMANTICS
    info->reserved        = 0;
    info->root_directory  = nullptr;
    info->name_bytes      = name_bytes;
    if (name_bytes > 0) std::memcpy(info->name, target_name.data(), name_bytes);

    constexpr FILE_INFO_BY_HANDLE_CLASS kFileRenameInfoEx =
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
    const BOOL renamed = ::SetFileInformationByHandle(source_handle, kFileRenameInfoEx, buffer.data(),
                                                       static_cast<DWORD>(buffer.size()));
    ::CloseHandle(source_handle);
    return renamed != FALSE;
#else
    std::error_code error;
    std::filesystem::rename(source, target, error);
    return ! error;
#endif
}

inline bool PublishFile(const std::filesystem::path& temporary, const std::filesystem::path& target) {
#if defined(_WIN32)
    if (RenameFileReplace(temporary, target)) return true;
    // 老系统或文件系统不支持 FileRenameInfoEx 时退回 MoveFileEx。
    return ::MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    return ! error;
#endif
}

} // namespace sr::platform

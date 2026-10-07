module;

#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif
#include <vector>

module viewer.common;

import rstd.cppstd;

namespace viewer
{

std::filesystem::path ExecutableDir(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
#if defined(__APPLE__)
    // macOS has no /proc/self/exe — use _NSGetExecutablePath. The first
    // call with size=0 sets *size to the required buffer length.
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, 0);
    if (size > 0 && _NSGetExecutablePath(buf.data(), &size) == 0) {
        auto p = fs::canonical(buf.data(), ec);
        if (! ec) return p.parent_path();
        return fs::path(buf.data()).parent_path();
    }
#elif defined(__linux__)
    std::vector<char> buf(PATH_MAX + 1, 0);
    const ssize_t size = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (size > 0) {
        buf[static_cast<std::size_t>(size)] = '\0';
        auto p = fs::canonical(buf.data(), ec);
        if (! ec) return p.parent_path();
        return fs::path(buf.data()).parent_path();
    }
#endif
    return fs::path(argv0 ? argv0 : "").parent_path();
}

} // namespace viewer

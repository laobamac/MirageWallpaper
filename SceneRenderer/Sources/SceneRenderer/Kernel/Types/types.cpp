module;

#include <cstdio>
#include <dlfcn.h>

module sr.types;
import rstd.cppstd;

namespace sr
{

std::string ToString(const ImageType& type) {
#define IMG(x) \
    case ImageType::x: return #x;
    switch (type) {
        IMG(UNKNOWN);
        IMG(BMP);
        IMG(ICO);
        IMG(JPEG);
        IMG(JNG);
        IMG(PNG);
        IMG(VIDEO);
    default: std::fprintf(stderr, "[ERROR] Not valid image type: %d\n", (int)type); return "";
    }
#undef IMG
}

std::string ToString(const TextureFormat& format) {
#define FMT(x) \
    case TextureFormat::x: return #x;
    switch (format) {
        FMT(RGBA8);
        FMT(BC1);
        FMT(BC2);
        FMT(BC3);
        FMT(RGB8);
        FMT(RG8);
        FMT(R8);
        FMT(D32F);
    default: std::fprintf(stderr, "[ERROR] Not valid tex format: %d\n", (int)format); return "";
    }
#undef FMT
}

} // namespace sr

namespace utils
{

DynamicLibrary::DynamicLibrary() = default;
DynamicLibrary::DynamicLibrary(const char* filename) { Open(filename); }
DynamicLibrary::~DynamicLibrary() { Close(); }
DynamicLibrary::DynamicLibrary(DynamicLibrary&& o) noexcept
    : handle(std::exchange(o.handle, nullptr)) {}
DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& o) noexcept {
    Close();
    handle = std::exchange(o.handle, nullptr);
    return *this;
}
bool DynamicLibrary::Open(const char* filename) {
    handle = dlopen(filename, RTLD_NOW);
    return IsOpen();
}
bool DynamicLibrary::IsOpen() const { return handle != nullptr; }
void DynamicLibrary::Close() {
    if (IsOpen()) {
        dlclose(handle);
        handle = nullptr;
    }
}
void* DynamicLibrary::GetSymbolAddr(const char* name) const {
    return reinterpret_cast<void*>(dlsym(handle, name));
}

} // namespace utils

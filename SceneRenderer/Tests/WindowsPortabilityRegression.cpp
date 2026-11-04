#include "../Sources/SceneRenderer/Kernel/Support/include/Utils/PlatformFile.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

import rstd;
import sr.fs;
import sr.types;

static_assert(sizeof(void*) == sizeof(rstd::usize));
static_assert(sizeof(rstd::Option<rstd::num::nonzero::NonZero<rstd::u64>>) == sizeof(rstd::u64));

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}

void TestSynchronization() {
    auto [sender, receiver] = rstd::sync::mpsc::sync_channel<int>(1);
    auto worker = rstd::thread::spawn([sender = rstd::move(sender)]() mutable {
        sender.send(17).unwrap();
        sender.send(29).unwrap();
    }).unwrap();
    Check(receiver.recv().unwrap() == 17, "首条跨线程消息不匹配");
    Check(receiver.recv().unwrap() == 29, "有界通道唤醒后消息不匹配");
    rstd::move(worker).join().unwrap();

    rstd::sync::Mutex<bool> mutex(false);
    rstd::sync::Condvar condition;
    auto guard = mutex.lock().unwrap();
    const auto elapsed = condition.wait_timeout(guard, rstd::time::Duration::from_millis(10));
    Check(elapsed.timed_out(), "Windows 条件变量没有正确报告超时");
}

void TestDynamicLibrary() {
    utils::DynamicLibrary library("kernel32.dll");
    Check(library.IsOpen(), "Win32 DLL 加载失败");
    unsigned long (__stdcall* get_process_id)() = nullptr;
    Check(library.GetSymbol("GetCurrentProcessId", get_process_id), "DLL 符号解析失败");
    if (get_process_id) Check(get_process_id() == sr::platform::ProcessId(), "DLL 调用约定不匹配");
    utils::DynamicLibrary moved(std::move(library));
    Check(!library.IsOpen() && moved.IsOpen(), "DLL 移动所有权失败");
    moved.Close();
    Check(!moved.IsOpen(), "DLL 关闭失败");
}

void TestFilesystem() {
    const auto directory = std::filesystem::temp_directory_path() /
        sr::platform::NativePath("Mirage-场景-" + std::to_string(sr::platform::ProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto target = directory / L"资源.json";
    Check(sr::platform::NativePath(sr::platform::Utf8Path(target)) == target, "UTF-8 路径转换失败");
    auto* file = sr::platform::OpenFile(sr::platform::Utf8Path(target), "wb+");
    Check(file != nullptr, "宽字符文件打开失败");
    if (file) {
        Check(::_fseeki64(file, INT64_C(0x100000000), SEEK_SET) == 0 &&
              ::_ftelli64(file) == INT64_C(0x100000000), "文件偏移被截断为32位");
        std::fclose(file);
    }
    const auto temporary = directory / L"暂存.json";
    for (const char* text : {"first", "second"}) {
        file = sr::platform::OpenExclusiveFile(temporary);
        Check(file != nullptr, "独占暂存文件创建失败");
        if (!file) break;
        std::fputs(text, file);
        std::fclose(file);
        Check(sr::platform::PublishFile(temporary, target), "原子替换已有缓存失败");
    }
    file = sr::platform::OpenFile(sr::platform::Utf8Path(target), "rb");
    char text[16] {};
    if (file) { std::fread(text, 1, sizeof(text) - 1, file); std::fclose(file); }
    Check(std::string(text) == "second", "缓存替换内容不匹配");
    Check(sr::platform::OpenExclusiveFile(target) == nullptr, "独占文件创建覆盖了已有文件");

    sr::fs::VFS vfs;
    Check(vfs.Mount("/assets", sr::fs::CreatePhysicalFs(sr::platform::Utf8Path(directory))), "VFS 挂载失败");
    Check(vfs.Publish("/assets/子目录/字体.txt", [](sr::fs::IBinaryStreamW& stream) {
        return stream.WriteAll("first", 5);
    }), "VFS Unicode 发布失败");
    Check(vfs.Publish("/assets/子目录/字体.txt", [](sr::fs::IBinaryStreamW& stream) {
        return stream.WriteAll("second", 6);
    }), "VFS 原子替换失败");
    auto stream = vfs.Open("/assets/子目录/字体.txt");
    Check(stream && stream->ReadAllStr() == "second", "VFS Unicode 读取失败");
    Check(stream && stream->SeekSet(INT64_C(0x100000000)) && stream->Tell() == INT64_C(0x100000000),
          "VFS 文件偏移被截断");
    stream.reset();
    Check(!vfs.Open("/assets/../outside.txt"), "VFS 允许父目录穿越");
    Check(!vfs.OpenW("/assets/资源.txt:hidden"), "VFS 允许 Windows ADS 路径");
    const auto found = vfs.FindUniqueByBasenameInTopMount("/assets", "字体.txt");
    Check(found && *found == "/assets/子目录/字体.txt", "VFS basename 返回了非 UTF-8 路径");
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}
} // namespace

int main() {
    TestSynchronization();
    TestDynamicLibrary();
    TestFilesystem();
    if (!failures) std::cout << "WindowsPortabilityRegression: ok\n";
    return failures ? 1 : 0;
}

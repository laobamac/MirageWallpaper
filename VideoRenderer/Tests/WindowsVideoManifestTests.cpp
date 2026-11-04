#include "VideoManifest.h"
#include "VideoRendererTypes.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool rejected(const QString& directory) {
    try { return !VRVideoManifest::loadFromDirectory(directory.toStdString()); }
    catch (const VideoRendererManifestError&) { return true; }
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    if (!temp.isValid()) return 1;
    bool passed = true;
    auto check = [&passed](bool condition, const char* message) {
        if (!condition) { std::fprintf(stderr, "VideoManifestTests: %s\n", message); passed = false; }
    };
    const QString project = QDir(temp.path()).filePath(QStringLiteral("project.json"));
    check(rejected(temp.path()), "missing project rejected");
    check(write(project, "{"), "write malformed project");
    check(rejected(temp.path()), "malformed JSON rejected");
    check(write(project, R"({"type":"scene"})"), "write wrong type");
    check(rejected(temp.path()), "wrong type rejected");
    check(write(project, R"({"type":"video","file":42})"), "write wrong field type");
    check(rejected(temp.path()), "typed JSON failure converted to manifest error");
    check(write(project, R"({"type":"video","file":"..\\outside.mp4"})"), "write traversal");
    check(rejected(temp.path()), "Windows backslash traversal rejected");
    check(write(project, R"({"type":"video","file":"C:\\private.mp4"})"), "write absolute path");
    check(rejected(temp.path()), "absolute Windows path rejected");
    check(write(project, R"({"type":"video","file":"\\\\server\\share\\video.mp4"})"), "write UNC path");
    check(rejected(temp.path()), "UNC path rejected");
    check(write(project, R"({"type":"video","file":"video.mp4:stream"})"), "write alternate data stream");
    check(rejected(temp.path()), "alternate data stream rejected");
    const QString video = QDir(temp.path()).filePath(QStringLiteral("测试.MKV"));
    check(write(video, "test"), "write Unicode video filename");
    check(write(project, R"({"type":"VIDEO","general":{"properties":{"test":{"value":1}}}})"), "write fallback manifest");
    try {
        const auto manifest = VRVideoManifest::loadFromDirectory(temp.path().toStdString());
        check(QString::fromStdWString(manifest->videoPath().native()) == QDir::toNativeSeparators(video), "Unicode video resolved");
        check(manifest->userProperties().contains("test"), "user properties preserved");
        check(!manifest->title().empty(), "missing title falls back to directory");
        check(VRVideoManifest::loadFromPath(video.toStdString())->videoPath() == manifest->videoPath(), "direct file accepted");
    } catch (const VideoRendererManifestError& error) {
        std::fprintf(stderr, "%s\n", error.what()); passed = false;
    }
    VRVideoFillMode mode;
    check(VRParseVideoFillMode("fit", mode) && mode == VRVideoFillModeContain, "Linux fit alias accepted");
    check(!VRParseVideoFillMode("invalid", mode), "invalid fill rejected");
    check(VRClampVideoVolume(-1) == 0 && VRClampVideoVolume(2) == 1, "volume clamped");
    check(VRClampVideoVolume(std::numeric_limits<float>::quiet_NaN()) == 1, "nonfinite volume default");
    return passed ? 0 : 1;
}

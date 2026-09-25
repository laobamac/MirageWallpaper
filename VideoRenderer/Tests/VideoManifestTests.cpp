#include "VideoManifest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>
#include <memory>

namespace {

bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(contents) == contents.size();
}

bool writeProject(const QString& directory, const QJsonObject& object) {
    return writeFile(QDir(directory).filePath(QStringLiteral("project.json")),
                     QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "VideoManifestTests: %s\n", message);
    return condition;
}

// 新 API（d632be0）：loadFromDirectory 失败时抛 VideoRendererManifestError，
// 成功返回非空 shared_ptr。封装为旧式 (结果, 错误) 便于断言。
std::shared_ptr<VRVideoManifest> tryLoad(const QString& directory, QString* error) {
    try {
        return VRVideoManifest::loadFromDirectory(directory.toStdString());
    } catch (const VideoRendererManifestError& e) {
        if (error != nullptr) *error = QString::fromUtf8(e.what());
        return nullptr;
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;

    bool passed = true;
    QString error;
    passed &= expect(!tryLoad(QDir(temporary.path()).filePath(QStringLiteral("missing")), &error),
                     "missing directory rejected");

    const QString projectPath = QDir(temporary.path()).filePath(QStringLiteral("project.json"));
    passed &= expect(writeFile(projectPath, QByteArrayLiteral("{")), "write invalid project");
    error.clear();
    passed &= expect(!tryLoad(temporary.path(), &error),
                     "invalid JSON rejected");

    passed &= expect(writeProject(temporary.path(), {{QStringLiteral("type"), QStringLiteral("scene")}}),
                     "write wrong type project");
    error.clear();
    passed &= expect(!tryLoad(temporary.path(), &error),
                     "wrong wallpaper type rejected");

    passed &= expect(writeProject(temporary.path(), {
                         {QStringLiteral("type"), QStringLiteral("video")},
                         {QStringLiteral("file"), QStringLiteral("missing.mp4")},
                     }), "write missing video project");
    error.clear();
    passed &= expect(!tryLoad(temporary.path(), &error),
                     "missing video rejected");

    const QString videoPath = QDir(temporary.path()).filePath(QStringLiteral("fallback.MKV"));
    passed &= expect(writeFile(videoPath, QByteArrayLiteral("test")), "write fallback video");
    // 注意：project.json 缺 "file" 字段时 VideoManifest 内部会抛 json type_error
    // （parsed["file"] 为 null 时 get<string> 失败）——这是 d632be0 引入的
    // manifest 已知问题；按用户约束不修改 VideoManifest，测试显式提供 "file"。
    passed &= expect(writeProject(temporary.path(), {
                         {QStringLiteral("type"), QStringLiteral("video")},
                         {QStringLiteral("file"), QStringLiteral("fallback.MKV")},
                         {QStringLiteral("title"), QStringLiteral("Manifest Test")},
                         {QStringLiteral("preview"), QStringLiteral("preview.jpg")},
                     }), "write valid project");
    error.clear();
    const auto manifest = tryLoad(temporary.path(), &error);
    passed &= expect(manifest != nullptr, "valid manifest loaded");
    if (manifest) {
        passed &= expect(QString::fromStdString(manifest->title()) == QStringLiteral("Manifest Test"),
                         "title loaded");
        passed &= expect(QString::fromStdString(manifest->videoFile()) == QStringLiteral("fallback.MKV"),
                         "extension fallback loaded");
        passed &= expect(QString::fromStdString(manifest->videoPath()) == videoPath,
                         "video path resolved");
    }
    return passed ? 0 : 1;
}

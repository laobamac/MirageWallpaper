#include "SelfTest.h"

#include "Services/DisplayBrokerService.h"
#include "Services/Paths.h"
#include "Services/PlaybackController.h"
#include "Services/WallpaperLibrary.h"
#include "Services/WallpaperRuntimeStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QTimer>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJSEngine>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

namespace Mirage {

int runSelfTests() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        QTextStream(stderr) << "无法创建管理器自测临时目录\n";
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MirageSelfTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MirageSelfTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    Paths::setTestRoot(temporary.path());

    int failures = 0;
    int checks = 0;
    const auto check = [&failures, &checks](bool passed, const QString& name) {
        ++checks;
        if (!passed) ++failures;
        QTextStream(stdout) << QJsonDocument(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("self-test-check")},
            {QStringLiteral("name"), name}, {QStringLiteral("ok"), passed},
        }).toJson(QJsonDocument::Compact) << Qt::endl;
    };
    const QString qtScreenName = QStringLiteral("\\\\.\\DISPLAY1");
    const QString stableOutputId = QStringLiteral("\\\\.\\DISPLAY2");
    check(!DisplayBrokerService::fullscreenOutputMatchesStableId(stableOutputId, qtScreenName) &&
          DisplayBrokerService::fullscreenOutputMatchesStableId(qtScreenName, qtScreenName),
          QStringLiteral("全屏显示器按稳定设备ID匹配"));
    check(PlaybackController::strongestActionForRules(
              {QStringLiteral("mute"), QStringLiteral("pause")}) ==
              PlaybackController::PlaybackAction::Pause &&
          PlaybackController::strongestActionForRules(
              {QStringLiteral("stop"), QStringLiteral("pause")}) ==
              PlaybackController::PlaybackAction::Stop &&
          PlaybackController::strongestActionForRules(
              {QStringLiteral("unknown")}) == PlaybackController::PlaybackAction::KeepRunning,
          QStringLiteral("多来源播放规则取最强动作"));
    const auto write = [](const QString& path, const QByteArray& bytes) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const QString workshop = temporary.path() + QStringLiteral("/中文壁纸 空格");
    const QString basePath = workshop + QStringLiteral("/100");
    const QString presetPath = workshop + QStringLiteral("/200");
    const QJsonArray comboOptions{
        QJsonObject{{QStringLiteral("label"), QStringLiteral("一")}, {QStringLiteral("value"), 1}},
        QJsonObject{{QStringLiteral("label"), QStringLiteral("二")}, {QStringLiteral("value"), 2}},
    };
    const QJsonObject projectProperties{
        {QStringLiteral("enabled"), QJsonObject{{QStringLiteral("type"), QStringLiteral("bool")}, {QStringLiteral("value"), true}}},
        {QStringLiteral("mode"), QJsonObject{{QStringLiteral("type"), QStringLiteral("combo")},
            {QStringLiteral("value"), 1}, {QStringLiteral("options"), comboOptions}}},
        {QStringLiteral("texture"), QJsonObject{{QStringLiteral("type"), QStringLiteral("file")}, {QStringLiteral("value"), QStringLiteral("base.png")}}},
    };
    const QJsonObject baseJson{
        {QStringLiteral("title"), QStringLiteral("基础场景")},
        {QStringLiteral("type"), QStringLiteral("scene")},
        {QStringLiteral("file"), QStringLiteral("scene.json")},
        {QStringLiteral("workshopid"), 100},
        {QStringLiteral("general"), QJsonObject{{QStringLiteral("properties"), projectProperties}}},
    };
    const Project base = Project::fromJson(baseJson);
    check(base.kind() == WallpaperKind::Scene && base.workshopId == QStringLiteral("100") &&
          base.properties.value(QStringLiteral("enabled")).boolValue(), QStringLiteral("项目与属性解析"));
    check(Project::fromJson(base.toJson()).toJson() == base.toJson(), QStringLiteral("项目 JSON 往返"));
    const QVariant combo = base.properties.value(QStringLiteral("mode")).normalizedComboValue(QStringLiteral("2"));
    check(combo.typeId() != QMetaType::QString && combo.toInt() == 2, QStringLiteral("组合属性恢复数值类型"));

    const QJsonObject presetJson{
        {QStringLiteral("title"), QStringLiteral("预设")}, {QStringLiteral("dependency"), 100},
        {QStringLiteral("preset"), QJsonObject{{QStringLiteral("enabled"), false},
            {QStringLiteral("texture"), QStringLiteral("files/custom.png")}}},
    };
    check(write(basePath + QStringLiteral("/project.json"), QJsonDocument(baseJson).toJson()) &&
          write(basePath + QStringLiteral("/scene.json"), QByteArrayLiteral("{}")) &&
          write(presetPath + QStringLiteral("/project.json"), QJsonDocument(presetJson).toJson()) &&
          write(presetPath + QStringLiteral("/files/custom.png"), QByteArrayLiteral("fixture")),
          QStringLiteral("临时中文路径测试数据"));
    WallpaperLibrary library(nullptr);
    library.setAdditionalSourceDirectories({workshop});
    const Wallpaper preset = library.loadWallpaper(presetPath);
    check(preset.isValid() && preset.presetStatus == PresetStatus::Resolved &&
          preset.renderDirectory == basePath && !preset.project.properties.value(QStringLiteral("enabled")).boolValue(),
          QStringLiteral("预设依赖与覆盖解析"));
    QJsonObject missingJson = presetJson;
    missingJson.insert(QStringLiteral("dependency"), 999);
    const QString missingPath = workshop + QStringLiteral("/300");
    check(write(missingPath + QStringLiteral("/project.json"), QJsonDocument(missingJson).toJson()) &&
          library.loadWallpaper(missingPath).presetStatus == PresetStatus::MissingDependency,
          QStringLiteral("缺失预设依赖"));
    QJsonObject circularJson = presetJson;
    circularJson.insert(QStringLiteral("dependency"), 400);
    const QString circularPath = workshop + QStringLiteral("/400");
    check(write(circularPath + QStringLiteral("/project.json"), QJsonDocument(circularJson).toJson()) &&
          library.loadWallpaper(circularPath).presetStatus == PresetStatus::CircularDependency,
          QStringLiteral("循环预设依赖"));

    // 导入功能：外部目录/视频要能被复制进库目录，并能被再次扫描到。
    const QString external = temporary.path() + QStringLiteral("/外部 壁纸");
    check(write(external + QStringLiteral("/project.json"), QJsonDocument(baseJson).toJson()) &&
          write(external + QStringLiteral("/scene.json"), QByteArrayLiteral("{}")),
          QStringLiteral("准备导入用外部壁纸"));
    WallpaperLibrary importLibrary(nullptr);
    const QString importedFolder = importLibrary.importWallpaperFolder(external);
    const Wallpaper importedWallpaper = importLibrary.loadWallpaper(importedFolder);
    bool importedListed = false;
    for (const Wallpaper& item : importLibrary.loadAll())
        importedListed = importedListed || item.id() == importedWallpaper.id();
    check(!importedFolder.isEmpty() && importedWallpaper.isValid() &&
          importedWallpaper.kind() == WallpaperKind::Scene &&
          QFileInfo::exists(importedFolder + QStringLiteral("/scene.json")) && importedListed,
          QStringLiteral("从文件夹导入壁纸"));

    const QString notVideo = temporary.path() + QStringLiteral("/说明.txt");
    check(write(notVideo, QByteArrayLiteral("not a video")) &&
          importLibrary.importAny(notVideo).isEmpty(),
          QStringLiteral("导入拒绝非视频文件"));

    // 视频导入：复制媒体、生成（或占位生成）预览图并写出 project.json。
    const QString videoFile = temporary.path() + QStringLiteral("/导入 视频.mp4");
    QFile videoSource(QStringLiteral(":/MirageQt/Resources/WallpaperNotFound.mp4"));
    if (videoSource.open(QIODevice::ReadOnly)) {
        QFile videoTarget(videoFile);
        if (videoTarget.open(QIODevice::WriteOnly)) videoTarget.write(videoSource.readAll());
    }
    const QString videoFolder = importLibrary.importAny(videoFile);
    const Wallpaper importedVideo = importLibrary.loadWallpaper(videoFolder);
    // JPEG 编码依赖 qjpeg 插件；没有插件的环境（例如只跑 build 目录的自测）
    // 只校验导入结果，不把插件缺失算成导入失败。
    const bool jpegAvailable = QImageWriter::supportedImageFormats().contains("jpg");
    check(!videoFolder.isEmpty() && importedVideo.isValid() &&
          importedVideo.kind() == WallpaperKind::Video &&
          (!jpegAvailable || QFileInfo::exists(videoFolder + QStringLiteral("/preview.jpg"))),
          QStringLiteral("导入视频并生成预览"));

    WallpaperRuntimeStore runtime;
    check(runtime.effectiveProperties(preset).value(QStringLiteral("texture")).stringValue() ==
          presetPath + QStringLiteral("/files/custom.png"), QStringLiteral("预设覆盖资源路径"));
    WallpaperRuntimeState state;
    state.volume = 0.25;
    state.speed = 1.5;
    state.fillMode = FillMode::Contain;
    state.position = QPointF(0.2, 0.8);
    state.propertyOverrides.insert(QStringLiteral("mode"), QStringLiteral("2"));
    state.scriptStorage.insert(QStringLiteral("中文键"), QStringLiteral("值"));
    runtime.setRuntime(preset, state);
    QCoreApplication::processEvents();
    // 定时器真实超时后才断言持久化；不以固定睡眠代替状态。
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&loop, &preset] {
        if (QSettings().contains(QStringLiteral("Runtime_%1").arg(preset.id()))) loop.quit();
    });
    poll.start(10);
    timeout.start(3000);
    loop.exec();
    QSettings settings;
    settings.sync();
    WallpaperRuntimeStore restored;
    const WallpaperRuntimeState loaded = restored.loadRuntime(preset);
    check(settings.status() == QSettings::NoError && loaded.volume == state.volume && loaded.speed == state.speed &&
          loaded.fillMode == state.fillMode && loaded.position == state.position && loaded.scriptStorage == state.scriptStorage &&
          restored.effectiveProperties(preset).value(QStringLiteral("mode")).value.toInt() == 2,
          QStringLiteral("运行时状态持久化往返"));
    restored.setPosition(preset, QPointF(-1.0, 0.5));
    check(restored.loadRuntime(preset).position == state.position, QStringLiteral("拒绝无效裁切位置"));

    QFile evaluator(QStringLiteral(":/MirageQt/Qml/ConditionEvaluator.js"));
    const bool opened = evaluator.open(QIODevice::ReadOnly);
    QString script = opened ? QString::fromUtf8(evaluator.readAll()) : QString();
    script.remove(QStringLiteral(".pragma library"));
    QJSEngine engine;
    const QJSValue compiled = engine.evaluate(script, evaluator.fileName());
    const QString expression = QStringLiteral("enabled.value && mode.value === 2");
    const QVariantList properties{
        QVariantMap{{QStringLiteral("key"), QStringLiteral("enabled")}, {QStringLiteral("value"), true}, {QStringLiteral("condition"), expression}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("mode")}, {QStringLiteral("value"), 1}},
    };
    const QJSValue evaluate = engine.globalObject().property(QStringLiteral("evaluate"));
    const QJSValue enabledConditions = evaluate.call({engine.toScriptValue(properties), engine.toScriptValue(QVariantMap{{QStringLiteral("mode"), 2}})});
    const QJSValue disabledConditions = evaluate.call({engine.toScriptValue(properties), engine.toScriptValue(QVariantMap{{QStringLiteral("mode"), 2}, {QStringLiteral("enabled"), false}})});
    check(opened && !compiled.isError() && !enabledConditions.isError() && !disabledConditions.isError() &&
          enabledConditions.property(expression).toBool() && !disabledConditions.property(expression).toBool(), QStringLiteral("实际 QML 条件表达式与覆盖值"));

    QTextStream(stdout) << QJsonDocument(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("self-test-complete")},
        {QStringLiteral("ok"), failures == 0}, {QStringLiteral("checks"), checks},
        {QStringLiteral("failures"), failures},
    }).toJson(QJsonDocument::Compact) << Qt::endl;
    Paths::setTestRoot({});
    return failures == 0 ? 0 : 1;
}

} // namespace Mirage

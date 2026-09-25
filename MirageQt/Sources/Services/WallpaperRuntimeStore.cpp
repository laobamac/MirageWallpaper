#include "Services/WallpaperRuntimeStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

#include <cmath>

namespace Mirage {
namespace {

QString fillModeToString(FillMode mode) {
    switch (mode) {
    case FillMode::Cover: return QStringLiteral("cover");
    case FillMode::Contain: return QStringLiteral("contain");
    case FillMode::Stretch: return QStringLiteral("stretch");
    }
    return QStringLiteral("cover");
}

FillMode fillModeFromString(const QString& raw) {
    if (raw == QStringLiteral("contain")) return FillMode::Contain;
    if (raw == QStringLiteral("stretch")) return FillMode::Stretch;
    return FillMode::Cover;
}

QJsonObject runtimeToJson(const WallpaperRuntimeState& state) {
    QJsonObject object;
    object.insert(QStringLiteral("volume"), state.volume);
    object.insert(QStringLiteral("speed"), state.speed);
    object.insert(QStringLiteral("muted"), state.muted);
    object.insert(QStringLiteral("fillMode"), fillModeToString(state.fillMode));
    object.insert(QStringLiteral("position"), QJsonObject{
        {QStringLiteral("x"), state.position.x()},
        {QStringLiteral("y"), state.position.y()},
    });

    QJsonObject scriptStorage;
    for (auto it = state.scriptStorage.constBegin(); it != state.scriptStorage.constEnd(); ++it) {
        scriptStorage.insert(it.key(), it.value());
    }
    object.insert(QStringLiteral("scriptStorage"), scriptStorage);

    QJsonObject overrides;
    for (auto it = state.propertyOverrides.constBegin(); it != state.propertyOverrides.constEnd(); ++it) {
        overrides.insert(it.key(), variantToJsonValue(it.value()));
    }
    object.insert(QStringLiteral("propertyOverrides"), overrides);
    return object;
}

WallpaperRuntimeState runtimeFromJson(const QJsonObject& object) {
    WallpaperRuntimeState state;
    state.volume = object.value(QStringLiteral("volume")).toDouble(1.0);
    state.speed = object.value(QStringLiteral("speed")).toDouble(1.0);
    state.muted = object.value(QStringLiteral("muted")).toBool(false);
    state.fillMode = fillModeFromString(object.value(QStringLiteral("fillMode")).toString());
    if (object.contains(QStringLiteral("position"))) {
        const QJsonObject position = object.value(QStringLiteral("position")).toObject();
        const QJsonValue x = position.value(QStringLiteral("x"));
        const QJsonValue y = position.value(QStringLiteral("y"));
        if (x.isDouble() && y.isDouble() && std::isfinite(x.toDouble()) &&
            std::isfinite(y.toDouble()) && x.toDouble() >= 0.0 && x.toDouble() <= 1.0 &&
            y.toDouble() >= 0.0 && y.toDouble() <= 1.0) {
            state.position = QPointF(x.toDouble(), y.toDouble());
        }
    }

    const QJsonObject scriptStorage = object.value(QStringLiteral("scriptStorage")).toObject();
    if (scriptStorage.size() <= 1024) {
        bool valid = true;
        for (auto it = scriptStorage.constBegin(); it != scriptStorage.constEnd(); ++it) {
            if (!it.value().isString()) {
                valid = false;
                break;
            }
        }
        if (valid) {
            for (auto it = scriptStorage.constBegin(); it != scriptStorage.constEnd(); ++it) {
                state.scriptStorage.insert(it.key(), it.value().toString());
            }
        }
    }

    const QJsonObject overrides = object.value(QStringLiteral("propertyOverrides")).toObject();
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
        state.propertyOverrides.insert(it.key(), jsonValueToVariant(it.value()));
    }
    return state;
}

} // namespace

WallpaperRuntimeStore::WallpaperRuntimeStore(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<Mirage::WallpaperRuntimeState>();
}

WallpaperRuntimeState WallpaperRuntimeStore::loadRuntime(const Wallpaper& wallpaper) const {
    if (!wallpaper.isValid()) return {};

    if (m_runtimes.contains(wallpaper.id())) {
        return normalizedRuntime(m_runtimes.value(wallpaper.id()), wallpaper);
    }

    WallpaperRuntimeState state;
    const QByteArray data = QSettings().value(runtimeKey(wallpaper.id())).toByteArray();
    if (!data.isEmpty()) {
        const auto doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) state = runtimeFromJson(doc.object());
    }

    state = normalizedRuntime(state, wallpaper);
    m_runtimes.insert(wallpaper.id(), state);
    m_wallpapers.insert(wallpaper.id(), wallpaper);
    return state;
}

void WallpaperRuntimeStore::setRuntime(const Wallpaper& wallpaper, const WallpaperRuntimeState& state, bool scheduleSaveFlag) {
    if (!wallpaper.isValid()) return;
    const WallpaperRuntimeState normalized = normalizedRuntime(state, wallpaper);
    m_runtimes.insert(wallpaper.id(), normalized);
    m_wallpapers.insert(wallpaper.id(), wallpaper);
    emit runtimeChanged(wallpaper.id(), normalized);
    if (scheduleSaveFlag) scheduleSave(wallpaper);
}

void WallpaperRuntimeStore::resetRuntime(const Wallpaper& wallpaper) {
    if (!wallpaper.isValid()) return;
    WallpaperRuntimeState empty;
    m_runtimes.insert(wallpaper.id(), empty);
    m_wallpapers.insert(wallpaper.id(), wallpaper);
    persist(wallpaper, empty);
    emit runtimeChanged(wallpaper.id(), empty);
}

QHash<QString, ProjectProperty> WallpaperRuntimeStore::effectiveProperties(const Wallpaper& wallpaper) const {
    return effectiveProperties(wallpaper, loadRuntime(wallpaper));
}

QHash<QString, ProjectProperty> WallpaperRuntimeStore::effectiveProperties(const Wallpaper& wallpaper,
                                                                           const WallpaperRuntimeState& state) const {
    QHash<QString, ProjectProperty> result = wallpaper.project.properties;
    for (auto it = state.propertyOverrides.constBegin(); it != state.propertyOverrides.constEnd(); ++it) {
        if (!result.contains(it.key())) continue;
        ProjectProperty property = result.value(it.key());
        property.value = property.normalizedComboValue(it.value());
        result.insert(it.key(), property);
    }

    // Workshop presets may store file/directory/scenetexture values relative to
    // their own overlay (for example "files/background.jpg"). Resolve those the
    // same way as macOS so scene apply payloads and live setProperty stay valid.
    if (!wallpaper.assetOverlayDirectories.isEmpty()) {
        const QHash<QString, ProjectProperty> baseProperties = loadBaseProperties(wallpaper);
        const QSet<QString> presetKeys = QSet<QString>(wallpaper.project.preset.keyBegin(),
                                                       wallpaper.project.preset.keyEnd());
        for (auto it = result.begin(); it != result.end(); ++it) {
            const PropertyKind kind = it.value().propertyKind();
            if (kind != PropertyKind::File
                && kind != PropertyKind::SceneTexture
                && kind != PropertyKind::Directory) {
                continue;
            }

            ProjectProperty property = it.value();
            const QString path = property.stringValue();
            if (path.isEmpty()) continue;

            const bool absolute = QFileInfo(path).isAbsolute() || isWindowsAbsolutePath(path);
            if (absolute && !QFileInfo::exists(path)) {
                if (presetKeys.contains(it.key()) && baseProperties.contains(it.key())) {
                    property.value = baseProperties.value(it.key()).value;
                    it.value() = property;
                }
            } else if (!absolute) {
                QStringList searchDirectories = wallpaper.assetOverlayDirectories;
                if (!searchDirectories.contains(wallpaper.renderDirectory)) {
                    searchDirectories.append(wallpaper.renderDirectory);
                }
                const QString resolved = resolvedPresetAsset(path, searchDirectories);
                if (!resolved.isEmpty()) {
                    property.value = resolved;
                    it.value() = property;
                } else if (presetKeys.contains(it.key())
                           && path.startsWith(QStringLiteral("files/"))
                           && baseProperties.contains(it.key())) {
                    property.value = baseProperties.value(it.key()).value;
                    it.value() = property;
                }
            }
        }
    }
    return result;
}

ProjectProperty WallpaperRuntimeStore::setProperty(const Wallpaper& wallpaper, const QString& key, const QVariant& value) {
    if (!wallpaper.isValid() || !wallpaper.project.properties.contains(key)) return {};

    ProjectProperty property = wallpaper.project.properties.value(key);
    const QVariant normalized = property.normalizedComboValue(value);
    property.value = normalized;

    WallpaperRuntimeState state = loadRuntime(wallpaper);
    state.propertyOverrides.insert(key, normalized);
    setRuntime(wallpaper, state, true);
    return property;
}

void WallpaperRuntimeStore::setVolume(const Wallpaper& wallpaper, double volume) {
    WallpaperRuntimeState state = loadRuntime(wallpaper);
    state.volume = volume;
    state.muted = volume <= 0.0 ? state.muted : state.muted;
    setRuntime(wallpaper, state, true);
}

void WallpaperRuntimeStore::setSpeed(const Wallpaper& wallpaper, double speed) {
    WallpaperRuntimeState state = loadRuntime(wallpaper);
    state.speed = speed;
    setRuntime(wallpaper, state, true);
}

void WallpaperRuntimeStore::setMuted(const Wallpaper& wallpaper, bool muted) {
    WallpaperRuntimeState state = loadRuntime(wallpaper);
    state.muted = muted;
    setRuntime(wallpaper, state, true);
}

void WallpaperRuntimeStore::setFillMode(const Wallpaper& wallpaper, FillMode mode) {
    WallpaperRuntimeState state = loadRuntime(wallpaper);
    state.fillMode = mode;
    setRuntime(wallpaper, state, true);
}

void WallpaperRuntimeStore::setPosition(const Wallpaper& wallpaper, const QPointF& position) {
    if (!std::isfinite(position.x()) || !std::isfinite(position.y()) ||
        position.x() < 0.0 || position.x() > 1.0 ||
        position.y() < 0.0 || position.y() > 1.0) {
        qWarning() << "[Runtime] Rejected invalid crop position for wallpaper" << wallpaper.id();
        return;
    }
    WallpaperRuntimeState state = loadRuntime(wallpaper);
    state.position = position;
    setRuntime(wallpaper, state, true);
}

void WallpaperRuntimeStore::setScriptStorage(
    const Wallpaper& wallpaper, const QHash<QString, QString>& scriptStorage) {
    if (scriptStorage.size() > 1024) {
        qWarning() << "[Runtime] Rejected script storage with" << scriptStorage.size()
                   << "entries for wallpaper" << wallpaper.id();
        return;
    }
    WallpaperRuntimeState state = loadRuntime(wallpaper);
    state.scriptStorage = scriptStorage;
    setRuntime(wallpaper, state, true);
}

QString WallpaperRuntimeStore::runtimeKey(const QString& wallpaperId) const {
    return QStringLiteral("Runtime_%1").arg(wallpaperId);
}

WallpaperRuntimeState WallpaperRuntimeStore::normalizedRuntime(const WallpaperRuntimeState& source,
                                                               const Wallpaper& wallpaper) const {
    WallpaperRuntimeState result = source;
    for (auto it = result.propertyOverrides.begin(); it != result.propertyOverrides.end(); ++it) {
        const ProjectProperty property = wallpaper.project.properties.value(it.key());
        if (property.type.isEmpty()) continue;
        it.value() = property.normalizedComboValue(it.value());
    }
    return result;
}

QHash<QString, ProjectProperty>
WallpaperRuntimeStore::loadBaseProperties(const Wallpaper& wallpaper) const {
    const QString projectPath = QDir(wallpaper.renderDirectory).filePath(QStringLiteral("project.json"));

    // 性能缓存：effectiveProperties() 对 preset 壁纸每次调用都会走到这里，
    // 原实现每次读盘 + 解析整个 project.json（无缓存），而 QML 绑定风暴与
    // 滑块拖动（每格触发 selectedRuntimeChanged → 重建 selectedProperties）
    // 会让该重活反复执行。缓存以 project.json 的 mtime + size 作失效校验：
    // 命中且两者未变时一次 stat 替代一次全量读盘解析；mtime 或 size 任一
    // 变化（文件被更新）或文件不存在时重新解析。注：替换后同时保留 mtime
    // 与 size 的极端场景可能命中陈旧缓存，属缓存一致性通用权衡。这是缓存
    // 失效检查而非输入探测，解析结果语义与未缓存路径完全一致。
    const QFileInfo fileInfo(projectPath);
    const QDateTime modified = fileInfo.lastModified();
    const qint64 fileSize = fileInfo.size();

    const auto cached = m_basePropertiesCache.constFind(wallpaper.renderDirectory);
    if (cached != m_basePropertiesCache.constEnd()
        && cached->lastModified == modified
        && cached->fileSize == fileSize) {
        return cached->properties;
    }

    QHash<QString, ProjectProperty> properties;
    QFile projectFile(projectPath);
    if (projectFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(projectFile.readAll());
        if (document.isObject()) properties = Project::fromJson(document.object()).properties;
    }
    m_basePropertiesCache.insert(wallpaper.renderDirectory, {modified, fileSize, properties});
    return properties;
}

bool WallpaperRuntimeStore::isWindowsAbsolutePath(const QString& path) const {
    static const QRegularExpression drivePath(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    return drivePath.match(path).hasMatch() || path.startsWith(QStringLiteral("\\\\"));
}

QString WallpaperRuntimeStore::resolvedPresetAsset(const QString& relativePath,
                                                   const QStringList& directories) const {
    QString normalized = relativePath;
    normalized.replace('\\', '/');
    if (normalized.isEmpty() || QFileInfo(normalized).isAbsolute() ||
        isWindowsAbsolutePath(normalized)) {
        return {};
    }
    for (const QString& directory : directories) {
        const QDir root(directory);
        const QString candidate = QDir::cleanPath(root.filePath(normalized));
        const QString rootPath = QDir::cleanPath(root.absolutePath()) + '/';
        const QString absoluteCandidate = QFileInfo(candidate).absoluteFilePath();
        if (!absoluteCandidate.startsWith(rootPath) || !QFileInfo::exists(absoluteCandidate)) {
            continue;
        }
        return absoluteCandidate;
    }
    return {};
}

void WallpaperRuntimeStore::scheduleSave(const Wallpaper& wallpaper) {
    QTimer* timer = m_saveTimers.value(wallpaper.id());
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        m_saveTimers.insert(wallpaper.id(), timer);
        connect(timer, &QTimer::timeout, this, [this, wallpaperId = wallpaper.id()] {
            const Wallpaper wallpaper = m_wallpapers.value(wallpaperId);
            if (!wallpaper.isValid()) return;
            persist(wallpaper, m_runtimes.value(wallpaperId));
        });
    }
    timer->start(250);
}

void WallpaperRuntimeStore::persist(const Wallpaper& wallpaper, const WallpaperRuntimeState& state) const {
    if (!wallpaper.isValid()) return;
    const QByteArray data = QJsonDocument(runtimeToJson(state)).toJson(QJsonDocument::Compact);
    QSettings().setValue(runtimeKey(wallpaper.id()), data);
}

} // namespace Mirage

#include "Services/WallpaperRuntimeStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

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
    QFile projectFile(QDir(wallpaper.renderDirectory).filePath(QStringLiteral("project.json")));
    if (!projectFile.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(projectFile.readAll());
    if (!document.isObject()) return {};
    return Project::fromJson(document.object()).properties;
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

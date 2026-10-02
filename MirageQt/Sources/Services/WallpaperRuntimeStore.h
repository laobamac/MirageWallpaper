// WallpaperRuntimeStore — per-wallpaper playback state persisted to QSettings;
// the shared source of truth for the UI and renderer processes.

#pragma once

#include "Services/RendererController.h"
#include "Services/WEProject.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QTimer>
#include <QVariant>

namespace Mirage {

struct WallpaperRuntimeState {
    double volume = 1.0;
    double speed = 1.0;
    bool muted = false;
    FillMode fillMode = FillMode::Cover;
    // Normalized crop anchor. Both components are finite and within [0, 1].
    QPointF position {0.5, 0.5};
    // Scene-script localStorage snapshot. Keys and values follow the renderer's
    // fixed string-to-string protocol and are capped at 1024 entries.
    QHash<QString, QString> scriptStorage;
    QHash<QString, QVariant> propertyOverrides;
};

class WallpaperRuntimeStore : public QObject {
    Q_OBJECT

public:
    explicit WallpaperRuntimeStore(QObject* parent = nullptr);

    WallpaperRuntimeState loadRuntime(const Wallpaper& wallpaper) const;
    void setRuntime(const Wallpaper& wallpaper, const WallpaperRuntimeState& state, bool scheduleSave = true);
    void resetRuntime(const Wallpaper& wallpaper);

    QHash<QString, ProjectProperty> effectiveProperties(const Wallpaper& wallpaper) const;
    QHash<QString, ProjectProperty> effectiveProperties(const Wallpaper& wallpaper,
                                                        const WallpaperRuntimeState& state) const;

    ProjectProperty setProperty(const Wallpaper& wallpaper, const QString& key, const QVariant& value);
    void setVolume(const Wallpaper& wallpaper, double volume);
    void setSpeed(const Wallpaper& wallpaper, double speed);
    void setMuted(const Wallpaper& wallpaper, bool muted);
    void setFillMode(const Wallpaper& wallpaper, FillMode mode);
    void setPosition(const Wallpaper& wallpaper, const QPointF& position);
    void setScriptStorage(const Wallpaper& wallpaper,
                          const QHash<QString, QString>& scriptStorage);

signals:
    void runtimeChanged(const QString& wallpaperId, const Mirage::WallpaperRuntimeState& state);

private:
    // loadBaseProperties 的缓存条目：记录解析时的 project.json mtime 与
    // size，命中缓存时用一次 lastModified/size stat 与之一致性校验（见
    // .cpp 注释）。
    struct BasePropertiesCacheEntry {
        QDateTime lastModified;
        qint64 fileSize = 0;
        QHash<QString, ProjectProperty> properties;
    };

    QString runtimeKey(const QString& wallpaperId) const;
    WallpaperRuntimeState normalizedRuntime(const WallpaperRuntimeState& source, const Wallpaper& wallpaper) const;
    QHash<QString, ProjectProperty> loadBaseProperties(const Wallpaper& wallpaper) const;
    bool isWindowsAbsolutePath(const QString& path) const;
    QString resolvedPresetAsset(const QString& relativePath, const QStringList& directories) const;
    void scheduleSave(const Wallpaper& wallpaper);
    void persist(const Wallpaper& wallpaper, const WallpaperRuntimeState& state) const;

    mutable QHash<QString, WallpaperRuntimeState> m_runtimes;
    mutable QHash<QString, Wallpaper> m_wallpapers;
    mutable QHash<QString, BasePropertiesCacheEntry> m_basePropertiesCache;
    QHash<QString, QTimer*> m_saveTimers;
};

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::WallpaperRuntimeState)

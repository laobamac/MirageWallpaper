#pragma once

#include "Services/RendererController.h"
#include "Services/WEProject.h"

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariant>

namespace Mirage {

struct WallpaperRuntimeState {
    double volume = 1.0;
    double speed = 1.0;
    bool muted = false;
    FillMode fillMode = FillMode::Cover;
    QHash<QString, QVariant> propertyOverrides;
};

class WallpaperRuntimeStore : public QObject {
    Q_OBJECT

public:
    explicit WallpaperRuntimeStore(QObject* parent = nullptr);

    WallpaperRuntimeState runtime(const QString& wallpaperId) const;
    WallpaperRuntimeState loadRuntime(const Wallpaper& wallpaper) const;
    void setRuntime(const Wallpaper& wallpaper, const WallpaperRuntimeState& state, bool scheduleSave = true);
    void saveRuntime(const Wallpaper& wallpaper);
    void resetRuntime(const Wallpaper& wallpaper);

    QHash<QString, ProjectProperty> effectiveProperties(const Wallpaper& wallpaper) const;
    QHash<QString, ProjectProperty> effectiveProperties(const Wallpaper& wallpaper,
                                                        const WallpaperRuntimeState& state) const;

    ProjectProperty setProperty(const Wallpaper& wallpaper, const QString& key, const QVariant& value);
    void setVolume(const Wallpaper& wallpaper, double volume);
    void setSpeed(const Wallpaper& wallpaper, double speed);
    void setMuted(const Wallpaper& wallpaper, bool muted);
    void setFillMode(const Wallpaper& wallpaper, FillMode mode);

signals:
    void runtimeChanged(const QString& wallpaperId, const Mirage::WallpaperRuntimeState& state);

private:
    QString runtimeKey(const QString& wallpaperId) const;
    WallpaperRuntimeState normalizedRuntime(const WallpaperRuntimeState& source, const Wallpaper& wallpaper) const;
    QHash<QString, ProjectProperty> loadBaseProperties(const Wallpaper& wallpaper) const;
    bool isWindowsAbsolutePath(const QString& path) const;
    QString resolvedPresetAsset(const QString& relativePath, const QStringList& directories) const;
    void scheduleSave(const Wallpaper& wallpaper);
    void persist(const Wallpaper& wallpaper, const WallpaperRuntimeState& state) const;

    mutable QHash<QString, WallpaperRuntimeState> m_runtimes;
    mutable QHash<QString, Wallpaper> m_wallpapers;
    QHash<QString, QTimer*> m_saveTimers;
};

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::WallpaperRuntimeState)

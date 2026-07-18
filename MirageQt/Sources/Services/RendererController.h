#pragma once

#include "Services/GlobalSettingsService.h"
#include "Services/WEProject.h"

#include <QObject>
#include <QProcess>
#include <QSet>

namespace Mirage {

enum class FillMode {
    Cover,
    Contain,
    Stretch,
};

struct RenderOptions {
    int fps = 30;
    double volume = 1.0;
    bool muted = false;
    double speed = 1.0;
    FillMode fillMode = FillMode::Cover;
    bool enableSpectrum = true;
    QHash<QString, ProjectProperty> userProperties;
};

class RendererController : public QObject {
    Q_OBJECT

public:
    explicit RendererController(GlobalSettingsService* settings, QObject* parent = nullptr);
    ~RendererController() override;

    bool render(const Wallpaper& wallpaper, int screenIndex, const RenderOptions& options, QString* error = nullptr);
    void stop(int screenIndex);
    void stopAll();
    bool isRendering(int screenIndex) const;
    QVector<int> activeScreens() const;
    QSet<qint64> processIds() const;

    static QString fillModeKey(FillMode mode);

public slots:
    void setVolume(double volume, int screenIndex = -1);
    void setMuted(bool muted, int screenIndex = -1);
    void pause(int screenIndex = -1);
    void resume(int screenIndex = -1);
    void setFps(int fps, int screenIndex = -1);
    void setSpeed(double speed, int screenIndex = -1);
    void setFillMode(Mirage::FillMode mode, int screenIndex = -1);
    void setProperty(const QString& key, const Mirage::ProjectProperty& property, int screenIndex = -1);

signals:
    void rendererExited(int screenIndex, bool abnormal);
    void rendererMessage(const QString& message);

private:
    struct RunningProcess {
        QProcess* process = nullptr;
        Wallpaper wallpaper;
        int screenIndex = 0;
        bool stopping = false;
        QStringList tempFiles;
    };

    QString binaryForKind(WallpaperKind kind) const;
    QString sceneWallpaperBinary() const;
    QString webWallpaperBinary() const;
    QString videoWallpaperBinary() const;
    QString writeUserPropertiesFile(const QHash<QString, ProjectProperty>& props, const Wallpaper& wallpaper) const;
    QJsonObject propertyCommand(const QString& key, const ProjectProperty& property) const;
    void sendCommand(RunningProcess* running, const QJsonObject& command);
    void forEachTarget(int screenIndex, const std::function<void(RunningProcess*)>& body);

    GlobalSettingsService* m_settings = nullptr;
    QHash<int, RunningProcess*> m_running;
};

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::FillMode)

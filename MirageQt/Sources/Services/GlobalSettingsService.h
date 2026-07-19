#pragma once

#include <QObject>
#include <QString>

namespace Mirage {

struct GlobalSettings {
    QString otherApplicationFocused = QStringLiteral("keepRunning");
    QString otherApplicationFullscreen = QStringLiteral("keepRunning");
    QString otherApplicationPlayingAudio = QStringLiteral("keepRunning");
    QString displayAsleep = QStringLiteral("keepRunning");
    QString laptopOnBattery = QStringLiteral("keepRunning");

    QString antiAliasing = QStringLiteral("msaa_x2");
    QString postProcessing = QStringLiteral("disabled");
    QString textureResolution = QStringLiteral("automatic");
    bool reflections = false;
    int fps = 30;

    bool autoStart = false;
    bool safeMode = false;
    QString language = QStringLiteral("followSystem");
    QString appearance = QStringLiteral("followSystem");

    bool audioOutput = true;
    bool reloadWhenChangingOutputDevice = true;
    double masterVolume = 1.0;
    bool globalMuted = false;
    bool enableSpectrum = true;

    QString videoFramework = QStringLiteral("qt");
    QString processPriority = QStringLiteral("normal");
    bool pauseOnVRAMExhausted = false;
    bool restartAfterCrashing = false;
    QString logLevel = QStringLiteral("none");
    bool verboseLog = false;
    bool autoRefresh = true;

    QString steamAPIEndpoint = QStringLiteral("official");
    QString steamAPIKey;
    QString customWorkshopDirectory;
    QString customImportedDirectory;

    bool operator==(const GlobalSettings&) const = default;
};

class GlobalSettingsService : public QObject {
    Q_OBJECT

public:
    explicit GlobalSettingsService(QObject* parent = nullptr);

    const GlobalSettings& settings() const;
    bool setSettings(const GlobalSettings& settings, QString* error = nullptr);
    bool save(QString* error = nullptr) const;
    void reload();

    QString normalizedSteamAPIKey() const;
    bool hasValidCustomSteamAPIKey() const;
    QString steamAPIBaseUrl() const;

    void setQualityPreset(const QString& preset);

signals:
    void settingsChanged(const Mirage::GlobalSettings& settings);

private:
    GlobalSettings m_settings;
};

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::GlobalSettings)

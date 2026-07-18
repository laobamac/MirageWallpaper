#include "Services/GlobalSettingsService.h"

#include "Services/LinuxSystemIntegration.h"
#include "Services/Paths.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace Mirage {
namespace {

QString value(const QJsonObject& object, const char* key, const QString& fallback) {
    return object.value(QString::fromLatin1(key)).toString(fallback);
}

bool value(const QJsonObject& object, const char* key, bool fallback) {
    return object.value(QString::fromLatin1(key)).toBool(fallback);
}

int value(const QJsonObject& object, const char* key, int fallback) {
    return object.value(QString::fromLatin1(key)).toInt(fallback);
}

double value(const QJsonObject& object, const char* key, double fallback) {
    return object.value(QString::fromLatin1(key)).toDouble(fallback);
}

QJsonObject toJson(const GlobalSettings& s) {
    QJsonObject object;
    object["otherApplicationFocused"] = s.otherApplicationFocused;
    object["otherApplicationFullscreen"] = s.otherApplicationFullscreen;
    object["otherApplicationPlayingAudio"] = s.otherApplicationPlayingAudio;
    object["displayAsleep"] = s.displayAsleep;
    object["laptopOnBattery"] = s.laptopOnBattery;
    object["antiAliasing"] = s.antiAliasing;
    object["postProcessing"] = s.postProcessing;
    object["textureResolution"] = s.textureResolution;
    object["reflections"] = s.reflections;
    object["fps"] = s.fps;
    object["autoStart"] = s.autoStart;
    object["safeMode"] = s.safeMode;
    object["language"] = s.language;
    object["appearance"] = s.appearance;
    object["audioOutput"] = s.audioOutput;
    object["reloadWhenChangingOutputDevice"] = s.reloadWhenChangingOutputDevice;
    object["masterVolume"] = s.masterVolume;
    object["globalMuted"] = s.globalMuted;
    object["enableSpectrum"] = s.enableSpectrum;
    object["videoFramework"] = s.videoFramework;
    object["processPriority"] = s.processPriority;
    object["pauseOnVRAMExhausted"] = s.pauseOnVRAMExhausted;
    object["restartAfterCrashing"] = s.restartAfterCrashing;
    object["logLevel"] = s.logLevel;
    object["verboseLog"] = s.verboseLog;
    object["autoRefresh"] = s.autoRefresh;
    object["steamAPIEndpoint"] = s.steamAPIEndpoint;
    object["steamAPIKey"] = s.steamAPIKey;
    object["customWorkshopDirectory"] = s.customWorkshopDirectory;
    object["customImportedDirectory"] = s.customImportedDirectory;
    return object;
}

GlobalSettings fromJson(const QJsonObject& object) {
    GlobalSettings s;
    s.otherApplicationFocused = value(object, "otherApplicationFocused", s.otherApplicationFocused);
    s.otherApplicationFullscreen = value(object, "otherApplicationFullscreen", s.otherApplicationFullscreen);
    s.otherApplicationPlayingAudio = value(object, "otherApplicationPlayingAudio", s.otherApplicationPlayingAudio);
    s.displayAsleep = value(object, "displayAsleep", s.displayAsleep);
    s.laptopOnBattery = value(object, "laptopOnBattery", s.laptopOnBattery);
    s.antiAliasing = value(object, "antiAliasing", s.antiAliasing);
    s.postProcessing = value(object, "postProcessing", s.postProcessing);
    s.textureResolution = value(object, "textureResolution", s.textureResolution);
    s.reflections = value(object, "reflections", s.reflections);
    s.fps = value(object, "fps", s.fps);
    s.autoStart = value(object, "autoStart", s.autoStart);
    s.safeMode = value(object, "safeMode", s.safeMode);
    s.language = value(object, "language", s.language);
    s.appearance = value(object, "appearance", s.appearance);
    s.audioOutput = value(object, "audioOutput", s.audioOutput);
    s.reloadWhenChangingOutputDevice = value(object, "reloadWhenChangingOutputDevice", s.reloadWhenChangingOutputDevice);
    s.masterVolume = value(object, "masterVolume", s.masterVolume);
    s.globalMuted = value(object, "globalMuted", s.globalMuted);
    s.enableSpectrum = value(object, "enableSpectrum", s.enableSpectrum);
    s.videoFramework = value(object, "videoFramework", s.videoFramework);
    s.processPriority = value(object, "processPriority", s.processPriority);
    s.pauseOnVRAMExhausted = value(object, "pauseOnVRAMExhausted", s.pauseOnVRAMExhausted);
    s.restartAfterCrashing = value(object, "restartAfterCrashing", s.restartAfterCrashing);
    s.logLevel = value(object, "logLevel", s.logLevel);
    s.verboseLog = value(object, "verboseLog", s.verboseLog);
    s.autoRefresh = value(object, "autoRefresh", s.autoRefresh);
    s.steamAPIEndpoint = value(object, "steamAPIEndpoint", s.steamAPIEndpoint);
    s.steamAPIKey = value(object, "steamAPIKey", s.steamAPIKey);
    s.customWorkshopDirectory = value(object, "customWorkshopDirectory", s.customWorkshopDirectory);
    s.customImportedDirectory = value(object, "customImportedDirectory", s.customImportedDirectory);
    return s;
}

QString settingsPath() {
    return Paths::configDir() + "/settings.json";
}

} // namespace

GlobalSettingsService::GlobalSettingsService(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<Mirage::GlobalSettings>();
    reload();
}

const GlobalSettings& GlobalSettingsService::settings() const {
    return m_settings;
}

void GlobalSettingsService::setSettings(const GlobalSettings& settings) {
    const bool autoStartChanged = settings.autoStart != m_settings.autoStart;
    m_settings = settings;
    if (autoStartChanged) {
        LinuxSystemIntegration::setAutoStartEnabled(
            m_settings.autoStart, QCoreApplication::applicationFilePath());
    }
    save();
    emit settingsChanged(m_settings);
}

void GlobalSettingsService::save() const {
    QFile file(settingsPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(toJson(m_settings)).toJson(QJsonDocument::Indented));
    }
}

void GlobalSettingsService::reload() {
    QFile file(settingsPath());
    if (file.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) m_settings = fromJson(doc.object());
    }
    emit settingsChanged(m_settings);
}

QString GlobalSettingsService::normalizedSteamAPIKey() const {
    return m_settings.steamAPIKey.trimmed();
}

bool GlobalSettingsService::hasValidCustomSteamAPIKey() const {
    static const QRegularExpression re(QStringLiteral("^[A-Fa-f0-9]{32}$"));
    return re.match(normalizedSteamAPIKey()).hasMatch();
}

QString GlobalSettingsService::steamAPIBaseUrl() const {
    return m_settings.steamAPIEndpoint == "mirror"
        ? QStringLiteral("https://steams.524228.xyz/")
        : QStringLiteral("https://api.steampowered.com/");
}

void GlobalSettingsService::setQualityPreset(const QString& preset) {
    GlobalSettings next = m_settings;
    if (preset == "low") {
        next.antiAliasing = "none";
        next.postProcessing = "disabled";
        next.textureResolution = "highQuality";
        next.fps = 10;
        next.reflections = false;
    } else if (preset == "medium") {
        next.antiAliasing = "none";
        next.postProcessing = "enabled";
        next.textureResolution = "highQuality";
        next.fps = 15;
        next.reflections = true;
    } else if (preset == "high") {
        next.antiAliasing = "msaa_x2";
        next.postProcessing = "enabled";
        next.textureResolution = "highQuality";
        next.fps = 25;
        next.reflections = true;
    } else if (preset == "ultra") {
        next.antiAliasing = "msaa_x2";
        next.postProcessing = "ultra";
        next.textureResolution = "highQuality";
        next.fps = 30;
        next.reflections = true;
    }
    setSettings(next);
}

} // namespace Mirage

#include "Services/MirageController.h"

#include "Services/GlobalSettingsService.h"
#include "Services/PlaylistModels.h"
#include "Services/WEProject.h"
#include "Services/WorkshopModels.h"
#include "Services/WorkshopViewModel.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace Mirage {
namespace {

QVariantMap settingsMap(const GlobalSettings& settings) {
    return {
        {QStringLiteral("otherApplicationFocused"), settings.otherApplicationFocused},
        {QStringLiteral("otherApplicationFullscreen"), settings.otherApplicationFullscreen},
        {QStringLiteral("otherApplicationPlayingAudio"), settings.otherApplicationPlayingAudio},
        {QStringLiteral("displayAsleep"), settings.displayAsleep},
        {QStringLiteral("laptopOnBattery"), settings.laptopOnBattery},
        {QStringLiteral("antiAliasing"), settings.antiAliasing},
        {QStringLiteral("postProcessing"), settings.postProcessing},
        {QStringLiteral("textureResolution"), settings.textureResolution},
        {QStringLiteral("reflections"), settings.reflections},
        {QStringLiteral("fps"), settings.fps},
        {QStringLiteral("wallpaperLoadSource"), settings.wallpaperLoadSource},
        {QStringLiteral("autoStart"), settings.autoStart},
        {QStringLiteral("safeMode"), settings.safeMode},
        {QStringLiteral("language"), settings.language},
        {QStringLiteral("appearance"), settings.appearance},
        {QStringLiteral("audioOutput"), settings.audioOutput},
        {QStringLiteral("reloadWhenChangingOutputDevice"), settings.reloadWhenChangingOutputDevice},
        {QStringLiteral("masterVolume"), settings.masterVolume},
        {QStringLiteral("globalMuted"), settings.globalMuted},
        {QStringLiteral("enableSpectrum"), settings.enableSpectrum},
        {QStringLiteral("videoFramework"), settings.videoFramework},
        {QStringLiteral("processPriority"), settings.processPriority},
        {QStringLiteral("pauseOnVRAMExhausted"), settings.pauseOnVRAMExhausted},
        {QStringLiteral("restartAfterCrashing"), settings.restartAfterCrashing},
        {QStringLiteral("logLevel"), settings.logLevel},
        {QStringLiteral("verboseLog"), settings.verboseLog},
        {QStringLiteral("autoRefresh"), settings.autoRefresh},
        {QStringLiteral("steamAPIEndpoint"), settings.steamAPIEndpoint},
        {QStringLiteral("steamAPIKey"), settings.steamAPIKey},
        {QStringLiteral("customWorkshopDirectory"), settings.customWorkshopDirectory},
        {QStringLiteral("customImportedDirectory"), settings.customImportedDirectory},
    };
}

QVariantMap playlistSettingsMap(const PlaylistSettings& settings) {
    QVariantList anchors;
    for (int hour : settings.daytimeAnchors) anchors.append(hour);
    QVariantList weekOrder;
    for (int day : settings.dayOfWeekOrder) weekOrder.append(day);
    return {
        {QStringLiteral("order"), playlistOrderKey(settings.order)},
        {QStringLiteral("timing"), playlistTimingKey(settings.timing)},
        {QStringLiteral("timerHours"), settings.timerHours},
        {QStringLiteral("timerMinutes"), settings.timerMinutes},
        {QStringLiteral("updateOnPause"), settings.updateOnPause},
        {QStringLiteral("transition"), playlistTransitionKey(settings.transition)},
        {QStringLiteral("transitionSeconds"), settings.transitionSeconds},
        {QStringLiteral("alwaysBeginFirst"), settings.alwaysBeginFirst},
        {QStringLiteral("introOnStartup"), settings.introOnStartup},
        {QStringLiteral("videoSequence"), settings.videoSequence},
        {QStringLiteral("daytimeAnchors"), anchors},
        {QStringLiteral("dayOfWeekOrder"), weekOrder},
    };
}

} // namespace

void updatePlaylistSettingsFromMap(PlaylistSettings& settings, const QVariantMap& values) {
    if (values.contains(QStringLiteral("order"))) {
        settings.order = playlistOrderFromKey(values.value(QStringLiteral("order")).toString());
    }
    if (values.contains(QStringLiteral("timing"))) {
        settings.timing = playlistTimingFromKey(values.value(QStringLiteral("timing")).toString());
    }
    if (values.contains(QStringLiteral("timerHours"))) {
        settings.timerHours = values.value(QStringLiteral("timerHours")).toInt();
    }
    if (values.contains(QStringLiteral("timerMinutes"))) {
        settings.timerMinutes = values.value(QStringLiteral("timerMinutes")).toInt();
    }
    if (values.contains(QStringLiteral("updateOnPause"))) {
        settings.updateOnPause = values.value(QStringLiteral("updateOnPause")).toBool();
    }
    if (values.contains(QStringLiteral("transition"))) {
        settings.transition = playlistTransitionFromKey(values.value(QStringLiteral("transition")).toString());
    }
    if (values.contains(QStringLiteral("transitionSeconds"))) {
        settings.transitionSeconds = values.value(QStringLiteral("transitionSeconds")).toDouble();
    }
    if (values.contains(QStringLiteral("alwaysBeginFirst"))) {
        settings.alwaysBeginFirst = values.value(QStringLiteral("alwaysBeginFirst")).toBool();
    }
    if (values.contains(QStringLiteral("introOnStartup"))) {
        settings.introOnStartup = values.value(QStringLiteral("introOnStartup")).toBool();
    }
    if (values.contains(QStringLiteral("videoSequence"))) {
        settings.videoSequence = values.value(QStringLiteral("videoSequence")).toBool();
    }
    if (values.contains(QStringLiteral("daytimeAnchors"))) {
        QVector<int> anchors;
        for (const QVariant& value : values.value(QStringLiteral("daytimeAnchors")).toList()) {
            anchors.push_back(value.toInt());
        }
        settings.daytimeAnchors = anchors;
    }
    if (values.contains(QStringLiteral("dayOfWeekOrder"))) {
        QVector<int> weekOrder;
        for (const QVariant& value : values.value(QStringLiteral("dayOfWeekOrder")).toList()) {
            const int day = value.toInt();
            if (day >= 0 && day <= 6 && !weekOrder.contains(day)) weekOrder.push_back(day);
        }
        if (!weekOrder.isEmpty()) settings.dayOfWeekOrder = weekOrder;
    }
}

QString downloadStateKey(DownloadStateKind kind) {
    switch (kind) {
    case DownloadStateKind::Queued: return QStringLiteral("queued");
    case DownloadStateKind::Starting: return QStringLiteral("starting");
    case DownloadStateKind::Connecting: return QStringLiteral("connecting");
    case DownloadStateKind::Downloading: return QStringLiteral("downloading");
    case DownloadStateKind::Resolving: return QStringLiteral("resolving");
    case DownloadStateKind::Completed: return QStringLiteral("completed");
    case DownloadStateKind::Failed: return QStringLiteral("failed");
    case DownloadStateKind::Cancelled: return QStringLiteral("cancelled");
    }
    return {};
}

bool isActiveDownload(DownloadStateKind kind) {
    return kind == DownloadStateKind::Starting || kind == DownloadStateKind::Connecting ||
           kind == DownloadStateKind::Downloading || kind == DownloadStateKind::Resolving;
}

QVariantList MirageController::discoverSections() const {
    QVariantList result;
    for (const DiscoverSectionDefinition& definition : kDiscoverSections) {
        const QVector<WorkshopItem>& items = m_workshop.discoverItems(definition.collection);
        if (items.isEmpty()) continue;
        QVariantList sectionItems;
        sectionItems.reserve(items.size());
        for (const WorkshopItem& item : items) sectionItems.append(workshopItemMap(item));
        result.append(QVariantMap{
            {QStringLiteral("title"), QString::fromUtf8(definition.title)},
            {QStringLiteral("items"), sectionItems},
        });
    }
    return result;
}

QVariantMap MirageController::settings() const {
    return settingsMap(m_settings.settings());
}

QVariantMap MirageController::playlistSettings() const {
    return playlistSettingsMap(m_playlist.current(m_playlistScreen).settings);
}

QVariantMap MirageController::wallpaperMap(const Wallpaper& item) const {
    if (!item.isValid()) return {};
    QString typeKey = item.isPreset() ? QStringLiteral("preset") : wallpaperKindKey(item.kind());
    if (item.kind() == WallpaperKind::Unsupported
        && item.project.type.compare(QStringLiteral("application"), Qt::CaseInsensitive) == 0) {
        typeKey = QStringLiteral("application");
    }
    const QString typeLabel = typeKey == QStringLiteral("preset")
        ? QStringLiteral("预设")
        : typeKey == QStringLiteral("application") ? QStringLiteral("应用程序") : wallpaperKindName(item.kind());
    const QString importedRoot = QDir::cleanPath(m_library.importedDirectory()) + QDir::separator();
    const QString wallpaperPath = QDir::cleanPath(item.wallpaperDirectory) + QDir::separator();
    const qint64 size = QFileInfo(item.previewPath()).size() + QFileInfo(item.entryPath()).size();
    const QString searchText = QStringList{item.project.title,
                                           item.project.resolvedAuthor(),
                                           item.project.type,
                                           item.project.description,
                                           item.project.workshopId,
                                           QFileInfo(item.wallpaperDirectory).fileName(),
                                           item.project.tags.join(' ')}.join(' ');
    return {
        {QStringLiteral("id"), item.id()},
        {QStringLiteral("title"), item.project.title},
        {QStringLiteral("author"), item.project.resolvedAuthor()},
        {QStringLiteral("type"), typeKey},
        {QStringLiteral("typeLabel"), typeLabel},
        {QStringLiteral("kind"), wallpaperKindKey(item.kind())},
        {QStringLiteral("preview"), QUrl::fromLocalFile(item.previewPath())},
        {QStringLiteral("tags"), item.project.tags},
        {QStringLiteral("description"), item.project.description},
        {QStringLiteral("location"), QUrl::fromLocalFile(item.wallpaperDirectory)},
        {QStringLiteral("favorite"), m_favorites.contains(item.id())},
        {QStringLiteral("customizable"), !item.project.properties.isEmpty()},
        {QStringLiteral("approved"), item.project.approved},
        {QStringLiteral("rating"), item.project.contentRating},
        {QStringLiteral("source"), wallpaperPath.startsWith(importedRoot)
                                        ? QStringLiteral("imported") : QStringLiteral("workshop")},
        {QStringLiteral("size"), size},
        {QStringLiteral("searchText"), searchText},
        {QStringLiteral("preset"), item.isPreset()},
        {QStringLiteral("presetStatus"), item.presetStatusDescription()},
    };
}

QVariantMap MirageController::workshopItemMap(const WorkshopItem& item) const {
    QString downloadState;
    QString downloadMessage;
    double downloadProgress = -1.0;
    bool downloadActive = false;
    if (const std::optional<DownloadState> state = m_workshop.downloadStateFor(item.publishedFileId)) {
        downloadState = downloadStateKey(state->kind);
        downloadMessage = state->message;
        downloadProgress = state->percent;
        downloadActive = isActiveDownload(state->kind);
    }
    return {
        {QStringLiteral("id"), item.publishedFileId},
        {QStringLiteral("title"), item.title},
        {QStringLiteral("description"), item.description},
        {QStringLiteral("preview"), item.previewImageUrl},
        {QStringLiteral("tags"), item.tags},
        {QStringLiteral("type"), item.isPreset() ? QStringLiteral("preset") : item.wallpaperType},
        {QStringLiteral("typeLabel"), item.displayTypeName()},
        {QStringLiteral("rating"), item.ageRating},
        {QStringLiteral("subscriptions"), item.formattedSubscriptions()},
        {QStringLiteral("favorited"), item.formattedFavorited()},
        {QStringLiteral("views"), item.formattedViews()},
        {QStringLiteral("creatorSteamId"), item.creatorSteamId},
        {QStringLiteral("size"), item.fileSize},
        {QStringLiteral("sizeLabel"), item.formattedFileSize()},
        {QStringLiteral("updatedAt"), item.timeUpdated.toString(Qt::ISODate)},
        {QStringLiteral("downloaded"), m_workshop.isItemDownloaded(item.publishedFileId)},
        {QStringLiteral("needsDependency"), m_workshop.presetNeedsDependency(item.publishedFileId)},
        {QStringLiteral("downloadState"), downloadState},
        {QStringLiteral("downloadMessage"), downloadMessage},
        {QStringLiteral("downloadProgress"), downloadProgress},
        {QStringLiteral("downloadActive"), downloadActive},
    };
}

QVariantMap MirageController::playlistMap(const Playlist& playlist) const {
    return {
        {QStringLiteral("id"), playlist.id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("name"), playlist.name},
        {QStringLiteral("itemCount"), playlist.items.size()},
        {QStringLiteral("updatedAt"), playlist.updatedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))},
    };
}

QVariantMap MirageController::propertyMap(const QString& key, const ProjectProperty& property) const {
    QVariantList options;
    options.reserve(property.options.size());
    for (const ProjectPropertyOption& option : property.options) {
        options.append(QVariantMap{
            {QStringLiteral("label"), option.label},
            {QStringLiteral("value"), option.value},
            {QStringLiteral("condition"), option.condition},
        });
    }
    return {
        {QStringLiteral("key"), key},
        {QStringLiteral("text"), property.text},
        {QStringLiteral("type"), property.type.toLower()},
        {QStringLiteral("condition"), property.condition},
        {QStringLiteral("value"), property.value},
        {QStringLiteral("min"), property.min},
        {QStringLiteral("max"), property.max},
        {QStringLiteral("step"), property.step},
        {QStringLiteral("hasMin"), property.hasMin},
        {QStringLiteral("hasMax"), property.hasMax},
        {QStringLiteral("hasStep"), property.hasStep},
        {QStringLiteral("fraction"), property.fraction},
        {QStringLiteral("options"), options},
    };
}

} // namespace Mirage

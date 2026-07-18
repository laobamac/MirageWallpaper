#pragma once

#include "Services/GlobalSettingsService.h"
#include "Services/WEProject.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

namespace Mirage {

struct WorkshopItem {
    QString publishedFileId;
    QString title;
    QString description;
    QUrl previewImageUrl;
    QStringList tags;
    int subscriptions = 0;
    int favorited = 0;
    int views = 0;
    qint64 fileSize = 0;
    QDateTime timeCreated;
    QDateTime timeUpdated;
    QString creatorSteamId;
    QString wallpaperType = QStringLiteral("scene");

    WallpaperKind kind() const;
    bool isPreset() const;
    QString displayTypeName() const;
};

enum class WorkshopSortOrder {
    Trending,
    MostRecent,
    MostSubscribed,
    TopRated,
};

enum class WorkshopTypeFilter {
    All,
    Scene,
    Web,
    Video,
    Preset,
};

struct WorkshopQuery {
    QString searchText;
    QStringList tags;
    WorkshopSortOrder sortOrder = WorkshopSortOrder::Trending;
    WorkshopTypeFilter typeFilter = WorkshopTypeFilter::All;
    int page = 1;
    int perPage = 30;
};

struct WorkshopQueryResult {
    QVector<WorkshopItem> items;
    int total = 0;
    QString error;
};

class SteamWebAPI : public QObject {
    Q_OBJECT

public:
    explicit SteamWebAPI(GlobalSettingsService* settings, QObject* parent = nullptr);

    QUrl queryFilesUrl(const WorkshopQuery& query) const;
    QByteArray detailsPostBody(const QStringList& workshopIds) const;

    void queryFiles(const WorkshopQuery& query);
    void getFileDetails(const QStringList& workshopIds);
    void downloadPreviewImage(const QUrl& url);

signals:
    void queryFinished(const Mirage::WorkshopQueryResult& result);
    void detailsFinished(const QVector<Mirage::WorkshopItem>& items, const QString& error);
    void previewImageFinished(const QUrl& url, const QByteArray& bytes, const QString& error);

private:
    QString apiKey() const;
    int sortApiValue(WorkshopSortOrder order) const;
    QString typeFilterTag(WorkshopTypeFilter filter) const;
    QVector<WorkshopItem> parsePublishedFiles(const QJsonObject& response) const;

    GlobalSettingsService* m_settings = nullptr;
    QNetworkAccessManager m_network;
};

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::WorkshopItem)
Q_DECLARE_METATYPE(Mirage::WorkshopQueryResult)

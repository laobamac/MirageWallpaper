#pragma once

#include "Services/GlobalSettingsService.h"
#include "Services/WorkshopModels.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QUrl>

namespace Mirage {

class SteamWebAPI : public QObject {
    Q_OBJECT

public:
    explicit SteamWebAPI(GlobalSettingsService* settings, QObject* parent = nullptr);

    QUrl queryFilesUrl(const WorkshopQuery& query) const;
    QByteArray detailsPostBody(const QStringList& workshopIds) const;

    quint64 queryFiles(const WorkshopQuery& query);
    quint64 getFileDetails(const QStringList& workshopIds);
    void downloadPreviewImage(const QUrl& url);

signals:
    void queryFinished(quint64 requestId, const Mirage::WorkshopQueryResult& result);
    void detailsFinished(quint64 requestId, const QVector<Mirage::WorkshopItem>& items, const QString& error);
    void previewImageFinished(const QUrl& url, const QByteArray& bytes, const QString& error);

private:
    QString apiKey() const;
    int sortApiValue(WorkshopSortOrder order) const;
    QString typeFilterTag(WorkshopTypeFilter filter) const;
    QVector<WorkshopItem> parsePublishedFiles(const QJsonObject& response) const;

    GlobalSettingsService* m_settings = nullptr;
    QNetworkAccessManager m_network;
    quint64 m_nextRequestId = 1;
    QHash<QUrl, QByteArray> m_imageCache;
    QSet<QUrl> m_pendingImages;
};

} // namespace Mirage

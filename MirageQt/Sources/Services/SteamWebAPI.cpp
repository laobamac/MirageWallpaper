#include "Services/SteamWebAPI.h"

#include "Services/Paths.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrlQuery>

namespace Mirage {
namespace {

qint64 stringOrNumberToInt64(const QJsonValue& value) {
    if (value.isString()) return value.toString().toLongLong();
    if (value.isDouble()) return qRound64(value.toDouble());
    return 0;
}

QString tagName(const QJsonValue& value) {
    return value.toObject().value("tag").toString();
}

} // namespace

WallpaperKind WorkshopItem::kind() const {
    return wallpaperKindFromString(wallpaperType);
}

bool WorkshopItem::isPreset() const {
    for (const QString& tag : tags) {
        if (tag.compare(QStringLiteral("Preset"), Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

QString WorkshopItem::displayTypeName() const {
    return isPreset()
        ? QStringLiteral("预设 · %1").arg(wallpaperKindName(kind()))
        : wallpaperKindName(kind());
}

SteamWebAPI::SteamWebAPI(GlobalSettingsService* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings) {
    qRegisterMetaType<Mirage::WorkshopItem>();
    qRegisterMetaType<Mirage::WorkshopQueryResult>();
}

QUrl SteamWebAPI::queryFilesUrl(const WorkshopQuery& query) const {
    QUrl url(m_settings->steamAPIBaseUrl() + "IPublishedFileService/QueryFiles/v1/");
    QUrlQuery q;
    q.addQueryItem("key", apiKey());
    q.addQueryItem("query_type", QString::number(sortApiValue(query.sortOrder)));
    q.addQueryItem("page", QString::number(qMax(1, query.page)));
    q.addQueryItem("numperpage", QString::number(qBound(1, query.perPage, 100)));
    q.addQueryItem("appid", "431960");
    q.addQueryItem("return_tags", "true");
    q.addQueryItem("return_previews", "true");
    q.addQueryItem("return_metadata", "true");
    q.addQueryItem("strip_description_bbcode", "true");

    if (query.sortOrder == WorkshopSortOrder::Trending) {
        q.addQueryItem("days", "7");
        q.addQueryItem("include_recent_votes_only", "true");
    }
    if (!query.searchText.trimmed().isEmpty()) q.addQueryItem("search_text", query.searchText.trimmed());

    QStringList tags = query.tags;
    const QString typeTag = typeFilterTag(query.typeFilter);
    if (!typeTag.isEmpty()) tags << typeTag;
    for (int i = 0; i < tags.size(); ++i) q.addQueryItem(QStringLiteral("requiredtags[%1]").arg(i), tags.at(i));

    url.setQuery(q);
    return url;
}

QByteArray SteamWebAPI::detailsPostBody(const QStringList& workshopIds) const {
    QUrlQuery body;
    body.addQueryItem("itemcount", QString::number(workshopIds.size()));
    for (int i = 0; i < workshopIds.size(); ++i) {
        body.addQueryItem(QStringLiteral("publishedfileids[%1]").arg(i), workshopIds.at(i));
    }
    return body.query(QUrl::FullyEncoded).toUtf8();
}

void SteamWebAPI::queryFiles(const WorkshopQuery& query) {
    QNetworkReply* reply = m_network.get(QNetworkRequest(queryFilesUrl(query)));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        WorkshopQueryResult result;
        const QByteArray bytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            result.error = reply->errorString();
        } else {
            const QJsonDocument doc = QJsonDocument::fromJson(bytes);
            const QJsonObject response = doc.object().value("response").toObject();
            result.total = response.value("total").toInt();
            result.items = parsePublishedFiles(response);
        }
        reply->deleteLater();
        emit queryFinished(result);
    });
}

void SteamWebAPI::getFileDetails(const QStringList& workshopIds) {
    QUrl url(m_settings->steamAPIBaseUrl() + "ISteamRemoteStorage/GetPublishedFileDetails/v1/");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply* reply = m_network.post(request, detailsPostBody(workshopIds));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        QString error;
        QVector<WorkshopItem> items;
        const QByteArray bytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            error = reply->errorString();
        } else {
            const QJsonDocument doc = QJsonDocument::fromJson(bytes);
            items = parsePublishedFiles(doc.object().value("response").toObject());
        }
        reply->deleteLater();
        emit detailsFinished(items, error);
    });
}

void SteamWebAPI::downloadPreviewImage(const QUrl& url) {
    if (!url.isValid()) {
        emit previewImageFinished(url, {}, QStringLiteral("预览图 URL 无效"));
        return;
    }

    const QByteArray hash = QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha1).toHex();
    const QString cachePath = Paths::workshopCacheDir() + "/" + QString::fromLatin1(hash) + ".img";
    QFile cached(cachePath);
    if (cached.open(QIODevice::ReadOnly)) {
        emit previewImageFinished(url, cached.readAll(), {});
        return;
    }

    QNetworkReply* reply = m_network.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, cachePath] {
        QByteArray bytes = reply->readAll();
        QString error;
        if (reply->error() != QNetworkReply::NoError) {
            error = reply->errorString();
            bytes.clear();
        } else {
            QFile out(cachePath);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(bytes);
        }
        reply->deleteLater();
        emit previewImageFinished(url, bytes, error);
    });
}

QString SteamWebAPI::apiKey() const {
    return m_settings && m_settings->hasValidCustomSteamAPIKey()
        ? m_settings->normalizedSteamAPIKey()
        : QString();
}

int SteamWebAPI::sortApiValue(WorkshopSortOrder order) const {
    switch (order) {
    case WorkshopSortOrder::Trending: return 3;
    case WorkshopSortOrder::MostRecent: return 1;
    case WorkshopSortOrder::MostSubscribed: return 9;
    case WorkshopSortOrder::TopRated: return 0;
    }
    return 3;
}

QString SteamWebAPI::typeFilterTag(WorkshopTypeFilter filter) const {
    switch (filter) {
    case WorkshopTypeFilter::All: return {};
    case WorkshopTypeFilter::Scene: return QStringLiteral("Scene");
    case WorkshopTypeFilter::Web: return QStringLiteral("Web");
    case WorkshopTypeFilter::Video: return QStringLiteral("Video");
    case WorkshopTypeFilter::Preset: return QStringLiteral("Preset");
    }
    return {};
}

QVector<WorkshopItem> SteamWebAPI::parsePublishedFiles(const QJsonObject& response) const {
    QVector<WorkshopItem> items;
    const QJsonArray files = response.value("publishedfiledetails").toArray();
    for (const QJsonValue& fileValue : files) {
        const QJsonObject file = fileValue.toObject();
        WorkshopItem item;
        item.publishedFileId = file.value("publishedfileid").toString();
        item.title = file.value("title").toString(QStringLiteral("无标题"));
        item.description = file.value("file_description").toString();
        item.previewImageUrl = QUrl(file.value("preview_url").toString());
        item.subscriptions = file.value("subscriptions").toInt();
        item.favorited = file.value("favorited").toInt();
        item.views = file.value("views").toInt();
        item.fileSize = stringOrNumberToInt64(file.value("file_size"));
        item.timeCreated = QDateTime::fromSecsSinceEpoch(file.value("time_created").toInteger());
        item.timeUpdated = QDateTime::fromSecsSinceEpoch(file.value("time_updated").toInteger());
        item.creatorSteamId = file.value("creator").toString();

        const QJsonArray tags = file.value("tags").toArray();
        for (const QJsonValue& tagValue : tags) {
            const QString tag = tagName(tagValue);
            const QString lower = tag.toLower();
            if (lower == "scene" || lower == "web" || lower == "video") {
                item.wallpaperType = tag;
            } else if (!tag.isEmpty() &&
                       lower != "wallpaper" &&
                       lower != "approved" &&
                       lower != "everyone" &&
                       lower != "questionable" &&
                       lower != "mature") {
                item.tags << tag;
            }
        }
        items.push_back(item);
    }
    return items;
}

} // namespace Mirage

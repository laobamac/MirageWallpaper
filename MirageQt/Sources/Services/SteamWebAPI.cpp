#include "Services/SteamWebAPI.h"

#include "Services/Paths.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QCryptographicHash>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QThreadPool>
#include <QUrlQuery>

namespace Mirage {
namespace {

constexpr int kAPIRequestTimeoutMs = 15'000;
constexpr int kImageRequestTimeoutMs = 30'000;
constexpr int kAPIRequestIntervalMs = 350;

qint64 stringOrNumberToInt64(const QJsonValue& value) {
    if (value.isString()) return value.toString().toLongLong();
    if (value.isDouble()) return qRound64(value.toDouble());
    return 0;
}

QString tagName(const QJsonValue& value) {
    return value.toObject().value("tag").toString();
}

} // namespace

SteamWebAPI::SteamWebAPI(GlobalSettingsService* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings) {
    qRegisterMetaType<Mirage::WorkshopItem>();
    qRegisterMetaType<Mirage::WorkshopQueryResult>();
    m_network.setTransferTimeout(kAPIRequestTimeoutMs);
    m_apiRequestTimer.setSingleShot(true);
    connect(&m_apiRequestTimer, &QTimer::timeout, this, &SteamWebAPI::startNextAPIRequest);
}

QUrl SteamWebAPI::queryFilesUrl(const WorkshopQuery& query) const {
    QUrl url(m_settings->steamAPIBaseUrl() + "IPublishedFileService/QueryFiles/v1/");
    QUrlQuery q;
    q.addQueryItem("key", apiKey());
    q.addQueryItem("query_type", QString::number(sortApiValue(query.sortOrder)));
    q.addQueryItem("page", QString::number(qMax(1, query.page)));
    q.addQueryItem("numperpage", QString::number(qBound(1, query.perPage, 100)));
    q.addQueryItem("appid", "431960");
    q.addQueryItem("filetype", "0");
    q.addQueryItem("return_tags", "true");
    q.addQueryItem("return_previews", "true");
    q.addQueryItem("return_metadata", "true");
    q.addQueryItem("strip_description_bbcode", "true");

    if (workshopSortUsesTrendPeriod(query.sortOrder)) {
        q.addQueryItem("days", QString::number(qBound(1, query.trendDays, 365)));
    }
    if (query.sortOrder == WorkshopSortOrder::Trending) {
        q.addQueryItem("include_recent_votes_only", "true");
    }
    if (!query.searchText.trimmed().isEmpty()) q.addQueryItem("search_text", query.searchText.trimmed());

    QStringList tags = query.tags;
    QSet<QString> selectableTags;
    for (const WorkshopTag& tag : workshopTags()) selectableTags.insert(tag.value);
    if (QSet<QString>(tags.cbegin(), tags.cend()) == selectableTags) tags.clear();
    const QString typeTag = typeFilterTag(query.typeFilter);
    if (!typeTag.isEmpty()) tags << typeTag;
    for (int i = 0; i < tags.size(); ++i) q.addQueryItem(QStringLiteral("requiredtags[%1]").arg(i), tags.at(i));

    const int selectedRatings = query.ageRatingMask & 0x7;
    if (selectedRatings != 0 && selectedRatings != 0x7) {
        int index = 0;
        for (int rating = 0; rating < 3; ++rating) {
            if (selectedRatings & (1 << rating)) continue;
            q.addQueryItem(QStringLiteral("excludedtags[%1]").arg(index++),
                           workshopAgeRatingTag(static_cast<WorkshopAgeRating>(rating)));
        }
    }

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

quint64 SteamWebAPI::queryFiles(const WorkshopQuery& query) {
    const quint64 requestId = m_nextRequestId++;
    const QUrl url = queryFilesUrl(query);
    enqueueAPIRequest([this, requestId, url] {
        if (m_cancelledQueryIds.remove(requestId)) return;

        QNetworkRequest request(url);
        request.setTransferTimeout(kAPIRequestTimeoutMs);
        QNetworkReply* reply = m_network.get(request);
        m_queryReplies.insert(requestId, reply);
        applyTimeout(reply, kAPIRequestTimeoutMs);
        connect(reply, &QNetworkReply::finished, this, [this, reply, requestId] {
            m_queryReplies.remove(requestId);
            if (m_cancelledQueryIds.remove(requestId)) {
                reply->deleteLater();
                return;
            }

            WorkshopQueryResult result;
            const QByteArray bytes = reply->readAll();
            if (reply->error() != QNetworkReply::NoError) {
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 401 || status == 403) {
                    result.error = QStringLiteral("Steam API Key 无效、权限不足或当前线路拒绝访问");
                } else if (status == 429) {
                    result.error = QStringLiteral("Steam Web API 请求过于频繁，请稍后重试或设置专属 API Key");
                } else {
                    result.error = replyError(reply);
                }
            } else {
                QJsonParseError parseError;
                const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
                if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                    result.error = QStringLiteral("数据解析错误: %1").arg(parseError.errorString());
                } else {
                    const QJsonObject response = doc.object().value("response").toObject();
                    result.total = response.value("total").toInt();
                    result.items = parsePublishedFiles(response);
                }
            }
            reply->deleteLater();
            emit queryFinished(requestId, result);
        });
    });
    return requestId;
}

void SteamWebAPI::cancelQuery(quint64 requestId) {
    if (requestId == 0) return;
    m_cancelledQueryIds.insert(requestId);
    if (QPointer<QNetworkReply> reply = m_queryReplies.value(requestId)) reply->abort();
}

quint64 SteamWebAPI::getFileDetails(const QStringList& workshopIds) {
    const quint64 requestId = m_nextRequestId++;
    QUrl url(m_settings->steamAPIBaseUrl() + "ISteamRemoteStorage/GetPublishedFileDetails/v1/");
    const QByteArray body = detailsPostBody(workshopIds);
    enqueueAPIRequest([this, requestId, url, body] {
        QNetworkRequest request(url);
        request.setTransferTimeout(kAPIRequestTimeoutMs);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        QNetworkReply* reply = m_network.post(request, body);
        applyTimeout(reply, kAPIRequestTimeoutMs);
        connect(reply, &QNetworkReply::finished, this, [this, reply, requestId] {
            QString error;
            QVector<WorkshopItem> items;
            const QByteArray bytes = reply->readAll();
            if (reply->error() != QNetworkReply::NoError) {
                error = replyError(reply);
            } else {
                QJsonParseError parseError;
                const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
                if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                    error = QStringLiteral("数据解析错误: %1").arg(parseError.errorString());
                } else {
                    items = parsePublishedFiles(doc.object().value("response").toObject());
                }
            }
            reply->deleteLater();
            emit detailsFinished(requestId, items, error);
        });
    });
    return requestId;
}

void SteamWebAPI::downloadPreviewImage(const QUrl& url) {
    if (!url.isValid()) {
        emit previewImageFinished(url, {}, QStringLiteral("预览图 URL 无效"));
        return;
    }

    if (m_imageCache.contains(url)) {
        emit previewImageFinished(url, m_imageCache.value(url), {});
        return;
    }
    if (m_pendingImages.contains(url)) return;

    const QByteArray hash = QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha1).toHex();
    const QString cachePath = Paths::workshopCacheDir() + "/" + QString::fromLatin1(hash) + ".img";
    m_pendingImages.insert(url);
    auto* cacheRead = new QFutureWatcher<QByteArray>(this);
    connect(cacheRead, &QFutureWatcher<QByteArray>::finished, this, [this, cacheRead, url, cachePath] {
        const QByteArray cached = cacheRead->result();
        cacheRead->deleteLater();
        if (!cached.isEmpty()) {
            cacheImage(url, cached);
            m_pendingImages.remove(url);
            emit previewImageFinished(url, cached, {});
            return;
        }

        QNetworkRequest request(url);
        request.setTransferTimeout(kImageRequestTimeoutMs);
        QNetworkReply* reply = m_network.get(request);
        applyTimeout(reply, kImageRequestTimeoutMs);
        connect(reply, &QNetworkReply::finished, this, [this, reply, url, cachePath] {
            QByteArray bytes = reply->readAll();
            QString error;
            m_pendingImages.remove(url);
            if (reply->error() != QNetworkReply::NoError) {
                error = replyError(reply);
                bytes.clear();
            } else {
                cacheImage(url, bytes);
                QThreadPool::globalInstance()->start([cachePath, bytes] {
                    QFile out(cachePath);
                    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(bytes);
                });
            }
            reply->deleteLater();
            emit previewImageFinished(url, bytes, error);
        });
    });
    cacheRead->setFuture(QtConcurrent::run([cachePath] {
        QFile cached(cachePath);
        if (!cached.open(QIODevice::ReadOnly)) return QByteArray();
        return cached.readAll();
    }));
}

QString SteamWebAPI::apiKey() const {
    if (m_settings && m_settings->hasValidCustomSteamAPIKey()) return m_settings->normalizedSteamAPIKey();
#ifdef MIRAGE_STEAM_WEB_API_KEY
    return QStringLiteral(MIRAGE_STEAM_WEB_API_KEY).trimmed();
#else
    return {};
#endif
}

int SteamWebAPI::sortApiValue(WorkshopSortOrder order) const {
    switch (order) {
    case WorkshopSortOrder::Trending: return 3;
    case WorkshopSortOrder::MostRecent: return 1;
    case WorkshopSortOrder::MostSubscribed: return 12;
    case WorkshopSortOrder::TopRated: return 0;
    case WorkshopSortOrder::MostUpvoted: return 10;
    case WorkshopSortOrder::PlaytimeTrend: return 13;
    case WorkshopSortOrder::TotalPlaytime: return 14;
    case WorkshopSortOrder::AveragePlaytimeTrend: return 15;
    case WorkshopSortOrder::LifetimeAveragePlaytime: return 16;
    case WorkshopSortOrder::SessionsTrend: return 17;
    case WorkshopSortOrder::LifetimeSessions: return 18;
    case WorkshopSortOrder::LastUpdated: return 19;
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
        if (item.fileSize <= 0) continue;
        item.timeCreated = QDateTime::fromSecsSinceEpoch(file.value("time_created").toInteger());
        item.timeUpdated = QDateTime::fromSecsSinceEpoch(file.value("time_updated").toInteger());
        item.creatorSteamId = file.value("creator").toString();

        const QJsonArray tags = file.value("tags").toArray();
        for (const QJsonValue& tagValue : tags) {
            const QString tag = tagName(tagValue);
            const QString lower = tag.toLower();
            if (lower == "scene" || lower == "web" || lower == "video") {
                item.wallpaperType = tag;
            } else if (lower == "everyone" || lower == "questionable" || lower == "mature") {
                item.ageRating = tag;
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

void SteamWebAPI::enqueueAPIRequest(std::function<void()> request) {
    m_pendingAPIRequests.enqueue(std::move(request));
    if (!m_apiRequestTimer.isActive()) startNextAPIRequest();
}

void SteamWebAPI::startNextAPIRequest() {
    if (m_pendingAPIRequests.isEmpty()) return;
    if (m_lastAPIRequestStarted.isValid()) {
        const qint64 remaining = kAPIRequestIntervalMs - m_lastAPIRequestStarted.elapsed();
        if (remaining > 0) {
            m_apiRequestTimer.start(int(remaining));
            return;
        }
    }

    std::function<void()> request = m_pendingAPIRequests.dequeue();
    m_lastAPIRequestStarted.start();
    request();
    if (!m_pendingAPIRequests.isEmpty()) m_apiRequestTimer.start(kAPIRequestIntervalMs);
}

void SteamWebAPI::applyTimeout(QNetworkReply* reply, int milliseconds) const {
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, [reply] {
        if (reply->isFinished()) return;
        reply->setProperty("mirageTimedOut", true);
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, timer, &QTimer::stop);
    timer->start(milliseconds);
}

QString SteamWebAPI::replyError(const QNetworkReply* reply) const {
    return reply->property("mirageTimedOut").toBool()
        ? QStringLiteral("请求超时")
        : reply->errorString();
}

void SteamWebAPI::cacheImage(const QUrl& url, const QByteArray& bytes) {
    constexpr int kImageCacheLimit = 400;
    if (!m_imageCache.contains(url) && m_imageCache.size() >= kImageCacheLimit) {
        m_imageCache.erase(m_imageCache.begin());
    }
    m_imageCache.insert(url, bytes);
}

} // namespace Mirage

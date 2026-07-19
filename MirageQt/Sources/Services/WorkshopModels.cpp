#include "Services/WorkshopModels.h"

#include <QLocale>

namespace Mirage {
namespace {

QString formattedCount(int value) {
    if (value >= 1'000'000) return QString::number(double(value) / 1'000'000.0, 'f', 1) + QStringLiteral("M");
    if (value >= 1'000) return QString::number(double(value) / 1'000.0, 'f', 1) + QStringLiteral("K");
    return QString::number(value);
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

QString WorkshopItem::formattedFileSize() const {
    return fileSize > 0 ? QLocale().formattedDataSize(fileSize) : QStringLiteral("未知大小");
}

QString WorkshopItem::formattedSubscriptions() const {
    return formattedCount(subscriptions);
}

QString WorkshopItem::formattedFavorited() const {
    return formattedCount(favorited);
}

QString WorkshopItem::formattedViews() const {
    return formattedCount(views);
}

WorkshopItem WorkshopItem::dependencyPlaceholder(const QString& id) {
    WorkshopItem item;
    item.publishedFileId = id;
    item.title = QStringLiteral("基础壁纸 %1").arg(id);
    return item;
}

QString workshopSortLabel(WorkshopSortOrder order) {
    switch (order) {
    case WorkshopSortOrder::Trending: return QStringLiteral("热门趋势");
    case WorkshopSortOrder::MostRecent: return QStringLiteral("最新发布");
    case WorkshopSortOrder::MostSubscribed: return QStringLiteral("订阅最多");
    case WorkshopSortOrder::TopRated: return QStringLiteral("评分最高");
    }
    return {};
}

QString workshopTypeLabel(WorkshopTypeFilter filter) {
    switch (filter) {
    case WorkshopTypeFilter::All: return QStringLiteral("全部");
    case WorkshopTypeFilter::Scene: return QStringLiteral("场景");
    case WorkshopTypeFilter::Web: return QStringLiteral("网页");
    case WorkshopTypeFilter::Video: return QStringLiteral("视频");
    case WorkshopTypeFilter::Preset: return QStringLiteral("预设");
    }
    return {};
}

QVector<WorkshopTag> workshopTags() {
    return {
        {QStringLiteral("Anime"), QStringLiteral("动漫"), QStringLiteral("weather-stars")},
        {QStringLiteral("Nature"), QStringLiteral("自然"), QStringLiteral("emblem-photos")},
        {QStringLiteral("Abstract"), QStringLiteral("抽象"), QStringLiteral("applications-graphics")},
        {QStringLiteral("Landscape"), QStringLiteral("风景"), QStringLiteral("image-x-generic")},
        {QStringLiteral("Sci-Fi"), QStringLiteral("科幻"), QStringLiteral("applications-science")},
        {QStringLiteral("Cartoon"), QStringLiteral("卡通"), QStringLiteral("face-smile")},
        {QStringLiteral("Cyberpunk"), QStringLiteral("赛博朋克"), QStringLiteral("computer")},
        {QStringLiteral("Fantasy"), QStringLiteral("奇幻"), QStringLiteral("weather-stars")},
        {QStringLiteral("Girl"), QStringLiteral("女孩"), QStringLiteral("avatar-default")},
        {QStringLiteral("Game"), QStringLiteral("游戏"), QStringLiteral("applications-games")},
        {QStringLiteral("Animal"), QStringLiteral("动物"), QStringLiteral("face-smile")},
        {QStringLiteral("Music"), QStringLiteral("音乐"), QStringLiteral("audio-x-generic")},
        {QStringLiteral("Vehicle"), QStringLiteral("车辆"), QStringLiteral("applications-engineering")},
        {QStringLiteral("Technology"), QStringLiteral("科技"), QStringLiteral("computer")},
        {QStringLiteral("Retro"), QStringLiteral("复古"), QStringLiteral("document-open-recent")},
        {QStringLiteral("City"), QStringLiteral("城市"), QStringLiteral("go-home")},
        {QStringLiteral("Space"), QStringLiteral("太空"), QStringLiteral("weather-clear-night")},
        {QStringLiteral("Dark"), QStringLiteral("暗黑"), QStringLiteral("weather-clear-night")},
        {QStringLiteral("Pixel Art"), QStringLiteral("像素"), QStringLiteral("applications-graphics")},
        {QStringLiteral("Minimalist"), QStringLiteral("极简"), QStringLiteral("list-remove")},
        {QStringLiteral("Underwater"), QStringLiteral("水下"), QStringLiteral("weather-showers")},
        {QStringLiteral("Relaxing"), QStringLiteral("放松"), QStringLiteral("weather-few-clouds")},
        {QStringLiteral("Medieval"), QStringLiteral("中世纪"), QStringLiteral("security-high")},
        {QStringLiteral("Unspecified"), QStringLiteral("未分类"), QStringLiteral("dialog-question")},
    };
}

} // namespace Mirage

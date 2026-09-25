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
    case WorkshopSortOrder::MostUpvoted: return QStringLiteral("最多投票");
    case WorkshopSortOrder::PlaytimeTrend: return QStringLiteral("播放时长最多");
    case WorkshopSortOrder::TotalPlaytime: return QStringLiteral("总播放时长最多");
    case WorkshopSortOrder::AveragePlaytimeTrend: return QStringLiteral("平均播放时长最长");
    case WorkshopSortOrder::LifetimeAveragePlaytime: return QStringLiteral("终身平均播放时长");
    case WorkshopSortOrder::SessionsTrend: return QStringLiteral("播放次数最多");
    case WorkshopSortOrder::LifetimeSessions: return QStringLiteral("总播放次数最多");
    case WorkshopSortOrder::LastUpdated: return QStringLiteral("最近更新");
    }
    return {};
}

bool workshopSortUsesTrendPeriod(WorkshopSortOrder order) {
    return order == WorkshopSortOrder::Trending ||
           order == WorkshopSortOrder::PlaytimeTrend ||
           order == WorkshopSortOrder::AveragePlaytimeTrend ||
           order == WorkshopSortOrder::SessionsTrend;
}

QString workshopAgeRatingLabel(WorkshopAgeRating rating) {
    switch (rating) {
    case WorkshopAgeRating::Everyone: return QStringLiteral("所有人");
    case WorkshopAgeRating::Questionable: return QStringLiteral("轻度裸露");
    case WorkshopAgeRating::Mature: return QStringLiteral("成人");
    }
    return {};
}

QString workshopAgeRatingTag(WorkshopAgeRating rating) {
    switch (rating) {
    case WorkshopAgeRating::Everyone: return QStringLiteral("Everyone");
    case WorkshopAgeRating::Questionable: return QStringLiteral("Questionable");
    case WorkshopAgeRating::Mature: return QStringLiteral("Mature");
    }
    return {};
}

QString workshopTrendPeriodLabel(WorkshopTrendPeriod period) {
    switch (period) {
    case WorkshopTrendPeriod::Day: return QStringLiteral("今日");
    case WorkshopTrendPeriod::Week: return QStringLiteral("本周");
    case WorkshopTrendPeriod::Month: return QStringLiteral("本月");
    case WorkshopTrendPeriod::ThreeMonths: return QStringLiteral("三个月");
    case WorkshopTrendPeriod::SixMonths: return QStringLiteral("半年");
    case WorkshopTrendPeriod::Year: return QStringLiteral("一年");
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

// ---- 分辨率过滤实现 ----

namespace {

// 归一化 tag：小写并仅保留字母与数字（对齐 FRResolutionFilter.normalized 的
// "lowercased().filter { isLetter || isNumber }"，去掉空格与连字符）。
QString normalizeResolutionTag(const QString& tag) {
    QString result;
    result.reserve(tag.size());
    for (const QChar ch : tag) {
        const QChar lowered = ch.toLower();
        if (lowered.isLetterOrNumber()) result.append(lowered);
    }
    return result;
}

// 将归一化后的 tag 映射到 (group, bit)。命中返回 true 并写出 group/bit。
// 表结构与顺序严格对齐 FRResolutionFilter.kind(forTag:)。
bool resolutionKindForTag(const QString& normalized, int& group, int& bit) {
    // widescreen
    if (normalized == QLatin1String("standarddefinition")) { group = 0; bit = 0; return true; }
    if (normalized == QLatin1String("1280x720")) { group = 0; bit = 1; return true; }
    if (normalized == QLatin1String("1366x768")) { group = 0; bit = 2; return true; }
    if (normalized.startsWith(QLatin1String("1920x1080"))) { group = 0; bit = 3; return true; }
    if (normalized == QLatin1String("2560x1440")) { group = 0; bit = 4; return true; }
    if (normalized.startsWith(QLatin1String("3840x2160"))) { group = 0; bit = 5; return true; }
    if (normalized.startsWith(QLatin1String("7680x4320"))) { group = 0; bit = 6; return true; }
    // ultraWidescreen
    if (normalized == QLatin1String("ultrawidestandard") ||
        normalized == QLatin1String("ultrawidestandarddefinition")) { group = 1; bit = 0; return true; }
    if (normalized == QLatin1String("ultrawide2560x1080") ||
        normalized == QLatin1String("2560x1080")) { group = 1; bit = 1; return true; }
    if (normalized == QLatin1String("ultrawide3440x1440") ||
        normalized == QLatin1String("3440x1440")) { group = 1; bit = 2; return true; }
    // dualscreen
    if (normalized == QLatin1String("dualstandard") ||
        normalized == QLatin1String("dualstandarddefinition")) { group = 2; bit = 0; return true; }
    if (normalized == QLatin1String("dual3840x1080") ||
        normalized == QLatin1String("3840x1080")) { group = 2; bit = 1; return true; }
    if (normalized == QLatin1String("dual5120x1440") ||
        normalized == QLatin1String("5120x1440")) { group = 2; bit = 2; return true; }
    if (normalized == QLatin1String("dual7680x2160") ||
        normalized == QLatin1String("7680x2160")) { group = 2; bit = 3; return true; }
    // triplescreen
    if (normalized == QLatin1String("triplestandard") ||
        normalized == QLatin1String("triplestandarddefinition")) { group = 3; bit = 0; return true; }
    if (normalized == QLatin1String("triple4096x768") ||
        normalized == QLatin1String("4096x768")) { group = 3; bit = 1; return true; }
    if (normalized == QLatin1String("triple5760x1080") ||
        normalized == QLatin1String("5760x1080")) { group = 3; bit = 2; return true; }
    if (normalized == QLatin1String("triple7680x1440") ||
        normalized == QLatin1String("7680x1440")) { group = 3; bit = 3; return true; }
    if (normalized == QLatin1String("triple11520x2160") ||
        normalized == QLatin1String("11520x2160")) { group = 3; bit = 4; return true; }
    // portrait
    if (normalized == QLatin1String("portraitstandard") ||
        normalized == QLatin1String("portraitstandarddefinition") ||
        normalized == QLatin1String("potraitstandard")) { group = 4; bit = 0; return true; }
    if (normalized == QLatin1String("portrait720x1280") ||
        normalized == QLatin1String("720x1280")) { group = 4; bit = 1; return true; }
    if (normalized == QLatin1String("portrait1080x1920") ||
        normalized == QLatin1String("1080x1920")) { group = 4; bit = 2; return true; }
    if (normalized == QLatin1String("portrait1440x2560") ||
        normalized == QLatin1String("1440x2560")) { group = 4; bit = 3; return true; }
    if (normalized == QLatin1String("portrait2160x3840") ||
        normalized == QLatin1String("2160x3840")) { group = 4; bit = 4; return true; }
    // misc
    if (normalized == QLatin1String("otherresolution")) { group = 5; bit = 0; return true; }
    if (normalized == QLatin1String("dynamicresolution")) { group = 5; bit = 1; return true; }
    return false;
}

// 每组选项数量（决定"全选"掩码的位数）。
int resolutionOptionCount(int group) {
    switch (group) {
    case WorkshopResolutionWidescreen: return 7;
    case WorkshopResolutionUltraWidescreen: return 3;
    case WorkshopResolutionDualscreen: return 4;
    case WorkshopResolutionTriplescreen: return 5;
    case WorkshopResolutionPortrait: return 5;
    case WorkshopResolutionMisc: return 2;
    }
    return 0;
}

} // namespace

WorkshopResolutionOptions workshopResolutionOptions() {
    return {
        {QStringLiteral("标清"), QStringLiteral("1280 x 720"), QStringLiteral("1366 x 768"),
         QStringLiteral("1920 x 1080 - 全高清"), QStringLiteral("2560 x 1440"),
         QStringLiteral("3840 x 2160 - 4K"), QStringLiteral("7680 x 4320 - 8K")},
        {QStringLiteral("超宽（标准）"), QStringLiteral("2560 x 1080"), QStringLiteral("3440 x 1440")},
        {QStringLiteral("双显示器（标准）"), QStringLiteral("3840 x 1080"),
         QStringLiteral("5120 x 1440"), QStringLiteral("7680 x 2160")},
        {QStringLiteral("三显示器（标准）"), QStringLiteral("4096 x 768"), QStringLiteral("5760 x 1080"),
         QStringLiteral("7680 x 1440"), QStringLiteral("11520 x 2160")},
        {QStringLiteral("纵向（标准）"), QStringLiteral("720 x 1280"), QStringLiteral("1080 x 1920"),
         QStringLiteral("1440 x 2560"), QStringLiteral("2160 x 3840")},
        {QStringLiteral("其他分辨率"), QStringLiteral("动态分辨率")},
    };
}

int workshopResolutionAllMask(int group) {
    const int count = resolutionOptionCount(group);
    return count > 0 ? (1 << count) - 1 : 0;
}

bool workshopResolutionMatches(const QStringList& tags,
                               int widescreen, int ultraWidescreen,
                               int dualscreen, int triplescreen,
                               int portrait, int misc) {
    const int masks[WorkshopResolutionGroupCount] = {
        widescreen, ultraWidescreen, dualscreen, triplescreen, portrait, misc,
    };
    // 六组全选时直接通过（对齐 allResolutionsSelected）。
    bool allSelected = true;
    for (int group = 0; group < WorkshopResolutionGroupCount; ++group) {
        if (masks[group] != workshopResolutionAllMask(group)) {
            allSelected = false;
            break;
        }
    }
    if (allSelected) return true;

    // 将每个 tag 归一化后映射到分辨率 kind，未命中的丢弃。
    bool anyKind = false;
    for (const QString& tag : tags) {
        int group = 0;
        int bit = 0;
        if (!resolutionKindForTag(normalizeResolutionTag(tag), group, bit)) continue;
        anyKind = true;
        if ((masks[group] & (1 << bit)) != 0) return true;
    }
    // 无任何分辨率 tag 时，等价于仅选中 misc.otherResolution（bit 0）。
    if (!anyKind) return (misc & (1 << 0)) != 0;
    return false;
}

} // namespace Mirage

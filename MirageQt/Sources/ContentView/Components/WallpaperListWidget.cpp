#include "ContentView/Components/WallpaperListWidget.h"

#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QVBoxLayout>

namespace Mirage {
namespace {

constexpr int kCardSize = 160;
constexpr int kGridSize = 168;

enum WallpaperItemRole {
    PreviewRole = Qt::UserRole + 1,
    PresetRole,
    PresetStatusRole,
};

QString normalizedTag(QString tag) {
    tag = tag.toLower();
    tag.remove(' ');
    tag.remove('-');
    if (tag == QStringLiteral("pixelart")) return QStringLiteral("pixelart");
    if (tag == QStringLiteral("scifi")) return QStringLiteral("scifi");
    return tag;
}

class WallpaperCardDelegate final : public QStyledItemDelegate {
public:
    explicit WallpaperCardDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(kGridSize, kGridSize);
    }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);

        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const QRectF card = QRectF(option.rect.adjusted(4, 4, -4, -4));

        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(QStringLiteral("#27231f")));
        painter->drawRect(card);
        painter->setClipRect(card);

        const QPixmap preview = index.data(PreviewRole).value<QPixmap>();
        if (!preview.isNull()) {
            const qreal zoom = hovered ? 1.2 : 1.0;
            const qreal sourceSide = qMin(preview.width(), preview.height()) / zoom;
            const QRectF source((preview.width() - sourceSide) / 2.0,
                                (preview.height() - sourceSide) / 2.0,
                                sourceSide,
                                sourceSide);
            painter->drawPixmap(card, preview, source);
        } else {
            const QPixmap fallback = QIcon::fromTheme(QStringLiteral("preferences-desktop-wallpaper")).pixmap(72, 72);
            painter->drawPixmap(QRectF(card.center().x() - 36, card.center().y() - 36, 72, 72), fallback, fallback.rect());
        }

        const QRectF titleBackground(card.left(), card.bottom() - 44, card.width(), 44);
        painter->fillRect(titleBackground, QColor(0, 0, 0, hovered ? 102 : 52));
        painter->setClipping(false);

        QFont titleFont = option.font;
        titleFont.setPixelSize(11);
        painter->setFont(titleFont);
        painter->setPen(QColor(255, 255, 255, hovered ? 230 : 184));
        painter->drawText(titleBackground.adjusted(7, 3, -7, -3),
                          Qt::AlignCenter | Qt::TextWordWrap,
                          index.data(Qt::DisplayRole).toString());

        if (index.data(PresetRole).toBool()) {
            QFont badgeFont = option.font;
            badgeFont.setPixelSize(11);
            badgeFont.setBold(true);
            painter->setFont(badgeFont);
            const QFontMetrics metrics(badgeFont);
            const QString presetText = QStringLiteral("预设");
            const QRectF badge(card.left() + 7, card.top() + 7, metrics.horizontalAdvance(presetText) + 18, 23);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#c72fe4")));
            painter->drawRoundedRect(badge, 4, 4);
            painter->setPen(Qt::white);
            painter->drawText(badge, Qt::AlignCenter, presetText);

            const QString status = index.data(PresetStatusRole).toString();
            if (!status.isEmpty()) {
                const QRectF warning(card.left() + 7,
                                     badge.bottom() + 4,
                                     qMin<qreal>(card.width() - 14, metrics.horizontalAdvance(status) + 18),
                                     23);
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(QStringLiteral("#dd7b24")));
                painter->drawRoundedRect(warning, 4, 4);
                painter->setPen(Qt::white);
                painter->drawText(warning, Qt::AlignCenter, status);
            }
        }

        if (selected || hovered) {
            const qreal width = selected ? 3.0 : 1.0;
            painter->setPen(QPen(QColor(QStringLiteral("#0a84ff")), width));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(card.adjusted(width / 2.0, width / 2.0, -width / 2.0, -width / 2.0));
        }
        painter->restore();
    }
};

class WallpaperListView final : public QListWidget {
public:
    explicit WallpaperListView(QWidget* parent = nullptr)
        : QListWidget(parent) {
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QListWidget::paintEvent(event);
        if (count() != 0) return;
        QPainter painter(viewport());
        painter.setPen(QColor(QStringLiteral("#aaa59f")));
        QFont emptyFont = font();
        emptyFont.setPixelSize(17);
        painter.setFont(emptyFont);
        painter.drawText(viewport()->rect().adjusted(40, 40, -40, -40),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                         QStringLiteral("没有找到匹配的壁纸。\n请调整或重置左侧筛选条件，或更换搜索关键词。\n也可以点击底部“导入壁纸”添加新壁纸。"));
    }
};

} // namespace

WallpaperListWidget::WallpaperListWidget(WallpaperLibrary* library,
                                         FavoritesManager* favorites,
                                         QWidget* parent)
    : QWidget(parent)
    , m_library(library)
    , m_favorites(favorites) {
    m_list = new WallpaperListView(this);
    m_list->setItemDelegate(new WallpaperCardDelegate(m_list));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setViewMode(QListView::IconMode);
    m_list->setFlow(QListView::LeftToRight);
    m_list->setWrapping(true);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setGridSize(QSize(kGridSize, kGridSize));
    m_list->setSpacing(0);
    m_list->setUniformItemSizes(true);
    m_list->setMouseTracking(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget { border: 0; outline: 0; background: transparent; }"
        "QListWidget::item { border: 0; background: transparent; }"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_list);

    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        const Wallpaper wallpaper = currentWallpaper();
        if (!wallpaper.wallpaperDirectory.isEmpty()) emit wallpaperSelected(wallpaper);
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        const Wallpaper wallpaper = currentWallpaper();
        if (!wallpaper.wallpaperDirectory.isEmpty()) emit applyRequested(wallpaper, false);
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        QListWidgetItem* item = m_list->itemAt(point);
        if (item) m_list->setCurrentItem(item);

        QMenu menu(m_list);
        if (item) {
            const Wallpaper wallpaper = currentWallpaper();
            menu.addAction(QIcon::fromTheme(QStringLiteral("media-playback-start")), QStringLiteral("应用"), this,
                           [this, wallpaper] { emit applyRequested(wallpaper, false); });
            menu.addAction(QIcon::fromTheme(QStringLiteral("video-display")), QStringLiteral("覆盖到所有显示器"), this,
                           [this, wallpaper] { emit applyRequested(wallpaper, true); });
            menu.addAction(QIcon::fromTheme(QStringLiteral("emblem-favorite")),
                           m_favorites->contains(wallpaper.id()) ? QStringLiteral("取消收藏") : QStringLiteral("收藏"),
                           this,
                           [this, wallpaper] {
                               m_favorites->toggle(wallpaper.id());
                               emit favoriteToggled(wallpaper);
                               rebuildList();
                           });
            menu.addSeparator();
        }
        menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("刷新"), this, &WallpaperListWidget::reload);
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-import")), QStringLiteral("导入壁纸"), this, &WallpaperListWidget::importRequested);
        menu.exec(m_list->viewport()->mapToGlobal(point));
    });
    connect(m_favorites, &FavoritesManager::changed, this, &WallpaperListWidget::rebuildList);

    reload();
}

Wallpaper WallpaperListWidget::currentWallpaper() const {
    const QListWidgetItem* item = m_list->currentItem();
    if (!item) return {};
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= m_wallpapers.size()) return {};
    return m_wallpapers.at(index);
}

void WallpaperListWidget::reload() {
    m_wallpapers = m_library->loadAll();
    rebuildList();
}

void WallpaperListWidget::setSearchText(const QString& text) {
    m_searchText = text;
    rebuildList();
}

void WallpaperListWidget::setSortText(const QString& text) {
    m_sortText = text;
    rebuildList();
}

void WallpaperListWidget::setSortDescending(bool descending) {
    m_sortDescending = descending;
    rebuildList();
}

void WallpaperListWidget::setFilterState(const WallpaperFilterState& state) {
    m_filter = state;
    rebuildList();
}

void WallpaperListWidget::rebuildList() {
    QVector<int> indices;
    indices.reserve(m_wallpapers.size());
    for (int i = 0; i < m_wallpapers.size(); ++i) {
        if (matchesFilter(m_wallpapers.at(i))) indices.push_back(i);
    }

    std::sort(indices.begin(), indices.end(), [this](int lhs, int rhs) {
        const Wallpaper& a = m_wallpapers.at(lhs);
        const Wallpaper& b = m_wallpapers.at(rhs);
        int comparison = 0;
        if (m_sortText == QStringLiteral("评分")) {
            comparison = a.project.contentRating.localeAwareCompare(b.project.contentRating);
        } else if (m_sortText == QStringLiteral("文件大小")) {
            const qint64 aSize = QFileInfo(a.previewPath()).size() + QFileInfo(a.entryPath()).size();
            const qint64 bSize = QFileInfo(b.previewPath()).size() + QFileInfo(b.entryPath()).size();
            comparison = aSize == bSize ? 0 : (aSize < bSize ? -1 : 1);
        }
        if (comparison == 0) comparison = a.project.title.localeAwareCompare(b.project.title);
        return m_sortDescending ? comparison > 0 : comparison < 0;
    });

    const QString previousId = currentWallpaper().id();
    m_list->clear();
    int rowToSelect = -1;
    for (int row = 0; row < indices.size(); ++row) {
        const int index = indices.at(row);
        const Wallpaper& wallpaper = m_wallpapers.at(index);
        auto* item = new QListWidgetItem(wallpaper.project.title, m_list);
        item->setData(Qt::UserRole, index);
        item->setData(PresetRole, wallpaper.isPreset());
        item->setData(PresetStatusRole, wallpaper.presetStatusDescription());
        item->setToolTip(wallpaper.wallpaperDirectory);

        QPixmap preview(wallpaper.previewPath());
        if (!preview.isNull() && (preview.width() > 500 || preview.height() > 500)) {
            preview = preview.scaled(QSize(500, 500), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        item->setData(PreviewRole, preview);
        if (wallpaper.id() == previousId) rowToSelect = row;
    }
    if (rowToSelect >= 0) m_list->setCurrentRow(rowToSelect);
    else if (m_list->count() > 0) m_list->setCurrentRow(0);
    m_list->viewport()->update();
}

bool WallpaperListWidget::matchesFilter(const Wallpaper& wallpaper) const {
    if (m_filter.approvedOnly && !wallpaper.project.approved) return false;
    if (m_filter.favoritesOnly && !m_favorites->contains(wallpaper.id())) return false;
    if (m_filter.customizableOnly && wallpaper.project.properties.isEmpty()) return false;

    const QStringList normalizedTags = [&wallpaper] {
        QStringList result;
        for (const QString& tag : wallpaper.project.tags) result.push_back(normalizedTag(tag));
        return result;
    }();
    if (m_filter.mobileOnly && !normalizedTags.contains(QStringLiteral("mobile"))) return false;
    if (m_filter.audioOnly && !normalizedTags.contains(QStringLiteral("audioresponsive")) &&
        !normalizedTags.contains(QStringLiteral("audio"))) return false;

    QString type;
    if (wallpaper.isPreset()) {
        type = QStringLiteral("preset");
    } else {
        type = wallpaperKindKey(wallpaper.kind());
        if (wallpaper.kind() == WallpaperKind::Unsupported && wallpaper.project.type.compare(QStringLiteral("application"), Qt::CaseInsensitive) == 0) {
            type = QStringLiteral("application");
        }
    }
    if (m_filter.types.size() < 5 && !m_filter.types.contains(type)) return false;
    if (m_filter.ratings.size() < 3 && !m_filter.ratings.contains(wallpaper.project.contentRating)) return false;

    const QString importedRoot = QDir::cleanPath(m_library->importedDirectory()) + QDir::separator();
    const QString wallpaperPath = QDir::cleanPath(wallpaper.wallpaperDirectory) + QDir::separator();
    const QString source = wallpaperPath.startsWith(importedRoot) ? QStringLiteral("imported") : QStringLiteral("workshop");
    if (m_filter.sources.size() < 2 && !m_filter.sources.contains(source)) return false;

    if (m_filter.tags.size() < 25) {
        if (normalizedTags.isEmpty()) {
            if (!m_filter.tags.contains(QStringLiteral("unspecified"))) return false;
        } else {
            bool matched = false;
            for (const QString& tag : normalizedTags) {
                if (m_filter.tags.contains(tag)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) return false;
        }
    }

    const QString query = m_searchText.trimmed().toLower();
    if (query.isEmpty()) return true;
    QStringList haystack;
    haystack << wallpaper.project.title
             << wallpaper.project.resolvedAuthor()
             << wallpaper.project.type
             << wallpaper.project.description
             << wallpaper.project.tags
             << wallpaper.project.workshopId
             << QFileInfo(wallpaper.wallpaperDirectory).fileName();
    return haystack.join(' ').toLower().contains(query);
}

} // namespace Mirage

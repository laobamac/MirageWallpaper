#include "ContentView/Components/WallpaperListWidget.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

namespace Mirage {

WallpaperListWidget::WallpaperListWidget(WallpaperLibrary* library,
                                         FavoritesManager* favorites,
                                         QWidget* parent)
    : QWidget(parent)
    , m_library(library)
    , m_favorites(favorites) {
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索标题、作者、标签"));

    m_typeFilter = new QComboBox(this);
    m_typeFilter->addItems({QStringLiteral("全部"), QStringLiteral("场景"), QStringLiteral("网页"),
                            QStringLiteral("视频"), QStringLiteral("收藏"), QStringLiteral("预设")});

    m_sort = new QComboBox(this);
    m_sort->addItems({QStringLiteral("名称"), QStringLiteral("类型"), QStringLiteral("文件夹")});

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setViewMode(QListView::IconMode);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setIconSize(QSize(192, 108));
    m_list->setGridSize(QSize(230, 178));
    m_list->setWordWrap(true);

    auto* top = new QHBoxLayout;
    top->addWidget(m_search, 1);
    top->addWidget(m_typeFilter);
    top->addWidget(m_sort);

    auto* buttons = new QHBoxLayout;
    auto* reloadButton = new QPushButton(QIcon::fromTheme("view-refresh"), QStringLiteral("刷新"), this);
    auto* importButton = new QPushButton(QIcon::fromTheme("document-import"), QStringLiteral("导入"), this);
    m_favorite = new QPushButton(QIcon::fromTheme("emblem-favorite"), QStringLiteral("收藏"), this);
    m_apply = new QPushButton(QIcon::fromTheme("media-playback-start"), QStringLiteral("应用"), this);
    m_applyAll = new QPushButton(QIcon::fromTheme("video-display"), QStringLiteral("应用到所有屏幕"), this);
    buttons->addWidget(reloadButton);
    buttons->addWidget(importButton);
    buttons->addStretch(1);
    buttons->addWidget(m_favorite);
    buttons->addWidget(m_apply);
    buttons->addWidget(m_applyAll);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_list, 1);
    layout->addLayout(buttons);

    connect(reloadButton, &QPushButton::clicked, this, &WallpaperListWidget::reload);
    connect(importButton, &QPushButton::clicked, this, &WallpaperListWidget::importRequested);
    connect(m_search, &QLineEdit::textChanged, this, &WallpaperListWidget::rebuildList);
    connect(m_typeFilter, &QComboBox::currentIndexChanged, this, &WallpaperListWidget::rebuildList);
    connect(m_sort, &QComboBox::currentIndexChanged, this, &WallpaperListWidget::rebuildList);
    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        const Wallpaper wallpaper = currentWallpaper();
        if (!wallpaper.wallpaperDirectory.isEmpty()) emit wallpaperSelected(wallpaper);
    });
    connect(m_apply, &QPushButton::clicked, this, [this] {
        const Wallpaper wallpaper = currentWallpaper();
        if (!wallpaper.wallpaperDirectory.isEmpty()) emit applyRequested(wallpaper, false);
    });
    connect(m_applyAll, &QPushButton::clicked, this, [this] {
        const Wallpaper wallpaper = currentWallpaper();
        if (!wallpaper.wallpaperDirectory.isEmpty()) emit applyRequested(wallpaper, true);
    });
    connect(m_favorite, &QPushButton::clicked, this, [this] {
        const Wallpaper wallpaper = currentWallpaper();
        if (wallpaper.wallpaperDirectory.isEmpty()) return;
        m_favorites->toggle(wallpaper.id());
        emit favoriteToggled(wallpaper);
        rebuildList();
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
    m_externalSearch = text;
    m_search->setText(text);
    rebuildList();
}

void WallpaperListWidget::setTypeFilterText(const QString& text) {
    m_externalTypeFilter = text;
    const int index = m_typeFilter->findText(text);
    if (index >= 0) m_typeFilter->setCurrentIndex(index);
    rebuildList();
}

void WallpaperListWidget::setSortText(const QString& text) {
    m_externalSort = text;
    const int index = m_sort->findText(text);
    if (index >= 0) m_sort->setCurrentIndex(index);
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
        switch (m_sort->currentIndex()) {
        case 1:
            if (a.project.type != b.project.type) return a.project.type < b.project.type;
            break;
        case 2:
            if (a.wallpaperDirectory != b.wallpaperDirectory) return a.wallpaperDirectory < b.wallpaperDirectory;
            break;
        default:
            break;
        }
        return a.project.title.localeAwareCompare(b.project.title) < 0;
    });

    const QString previousId = currentWallpaper().id();
    m_list->clear();
    int rowToSelect = -1;
    for (int row = 0; row < indices.size(); ++row) {
        const int index = indices.at(row);
        const Wallpaper& wallpaper = m_wallpapers.at(index);
        auto* item = new QListWidgetItem(itemText(wallpaper), m_list);
        item->setData(Qt::UserRole, index);
        item->setToolTip(wallpaper.wallpaperDirectory);
        QPixmap preview(wallpaper.previewPath());
        if (!preview.isNull()) {
            item->setIcon(QIcon(preview.scaled(QSize(192, 108), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)));
        } else if (wallpaper.isPreset()) {
            item->setIcon(QIcon::fromTheme("emblem-symbolic-link"));
        } else {
            item->setIcon(QIcon::fromTheme("preferences-desktop-wallpaper"));
        }
        if (wallpaper.id() == previousId) rowToSelect = row;
    }
    if (rowToSelect >= 0) m_list->setCurrentRow(rowToSelect);
    else if (m_list->count() > 0) m_list->setCurrentRow(0);
}

bool WallpaperListWidget::matchesFilter(const Wallpaper& wallpaper) const {
    const QString type = m_typeFilter->currentText();
    if (type == "场景" && wallpaper.kind() != WallpaperKind::Scene) return false;
    if (type == "网页" && wallpaper.kind() != WallpaperKind::Web) return false;
    if (type == "视频" && wallpaper.kind() != WallpaperKind::Video) return false;
    if (type == "收藏" && !m_favorites->contains(wallpaper.id())) return false;
    if (type == "预设" && !wallpaper.isPreset()) return false;

    const QString q = (m_externalSearch.isEmpty() ? m_search->text() : m_externalSearch).trimmed().toLower();
    if (q.isEmpty()) return true;

    QStringList haystack;
    haystack << wallpaper.project.title
             << wallpaper.project.resolvedAuthor()
             << wallpaper.project.tags
             << QFileInfo(wallpaper.wallpaperDirectory).fileName();
    return haystack.join(' ').toLower().contains(q);
}

QString WallpaperListWidget::itemText(const Wallpaper& wallpaper) const {
    const QString favorite = m_favorites->contains(wallpaper.id()) ? QStringLiteral("  ★") : QString();
    const QString status = wallpaper.presetStatusDescription();
    const QString subtitle = QStringLiteral("%1%2%3")
                                 .arg(wallpaperKindName(wallpaper.kind()),
                                      wallpaper.isPreset() ? QStringLiteral(" · 预设") : QString(),
                                      status.isEmpty() ? QString() : QStringLiteral(" · %1").arg(status));
    return QStringLiteral("%1%2\n%3")
        .arg(wallpaper.project.title, favorite, subtitle);
}

} // namespace Mirage

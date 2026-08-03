#include "ContentView/Components/ExplorerBottomBarWidget.h"
#include "App/MirageWidgets.h"

#include "ContentView/Components/Playlist/PlaylistOpenDialog.h"
#include "ContentView/Components/Playlist/PlaylistSaveDialog.h"
#include "ContentView/Components/Playlist/PlaylistSettingsDialog.h"

#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>

namespace Mirage {
namespace {

QIcon thumbnailFor(const Wallpaper& wallpaper) {
    if (!wallpaper.previewPath().isEmpty()) {
        QPixmap pixmap(wallpaper.previewPath());
        if (!pixmap.isNull()) {
            return QIcon(pixmap.scaled(72, 72, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        }
    }
    return QIcon::fromTheme(QStringLiteral("image-x-generic"));
}

} // namespace

ExplorerBottomBarWidget::ExplorerBottomBarWidget(PlaylistManager* playlistManager,
                                                 WallpaperLibrary* library,
                                                 QWidget* parent)
    : QWidget(parent)
    , m_manager(playlistManager)
    , m_library(library) {
    m_collapsed = QSettings().value(QStringLiteral("PlaylistCollapsed"), false).toBool();

    m_collapse = new QPushButton(this);
    m_collapse->setFlat(true);
    m_collapse->setFixedWidth(28);
    m_title = new QLabel(QStringLiteral("播放列表"), this);
    QFont font = m_title->font();
    font.setPointSize(18);
    font.setWeight(QFont::Medium);
    m_title->setFont(font);

    m_screen = new MirageComboBox(this);
    m_screen->setFixedWidth(120);
    const int screenCount = qMax(1, QGuiApplication::screens().size());
    for (int i = 0; i < screenCount; ++i) {
        m_screen->addItem(QStringLiteral("屏幕 %1").arg(i + 1), i);
    }
    m_screen->setVisible(screenCount > 1);

    m_load = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")), QStringLiteral("载入"), this);
    m_save = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), QStringLiteral("保存"), this);
    m_configure = new QPushButton(QIcon::fromTheme(QStringLiteral("settings-configure")), QStringLiteral("配置"), this);
    m_add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("添加壁纸"), this);
    m_add->setProperty("accent", true);

    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(6);
    top->addWidget(m_collapse);
    top->addWidget(m_title);
    top->addWidget(m_screen);
    top->addStretch(1);
    top->addWidget(m_load);
    top->addWidget(m_save);
    top->addWidget(m_configure);
    top->addWidget(m_add);

    m_strip = new QListWidget(this);
    m_strip->setViewMode(QListView::IconMode);
    m_strip->setFlow(QListView::LeftToRight);
    m_strip->setWrapping(false);
    m_strip->setResizeMode(QListView::Adjust);
    m_strip->setMovement(QListView::Static);
    m_strip->setIconSize(QSize(72, 72));
    m_strip->setSpacing(8);
    m_strip->setFixedHeight(118);
    m_strip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_strip->setSelectionMode(QAbstractItemView::SingleSelection);

    m_empty = new QLabel(QStringLiteral("尚未添加壁纸，右键壁纸选择“加入播放列表”，或点击右上角“添加壁纸”。"), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setStyleSheet(QStringLiteral("color: #8b8680;"));
    m_empty->setFixedHeight(118);

    auto* import = new QPushButton(QIcon::fromTheme(QStringLiteral("document-import")), QStringLiteral("导入壁纸"), this);
    import->setFixedWidth(236);
    import->setMinimumHeight(34);
    import->setProperty("accent", true);
    m_remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), QStringLiteral("移除壁纸"), this);
    m_clear = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("清理"), this);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->addWidget(import);
    footer->addStretch(1);
    footer->addWidget(m_remove);
    footer->addWidget(m_clear);

    m_body = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(6);
    bodyLayout->addWidget(m_strip);
    bodyLayout->addWidget(m_empty);
    bodyLayout->addLayout(footer);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addLayout(top);
    layout->addWidget(m_body);

    connect(m_collapse, &QPushButton::clicked, this, [this] { setCollapsed(!m_collapsed); });
    connect(m_screen, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_manager->ensureScreen(targetScreen());
        m_selectedItemID.clear();
        rebuildStrip();
        updateHeader();
    });
    connect(m_load, &QPushButton::clicked, this, &ExplorerBottomBarWidget::openLoadDialog);
    connect(m_save, &QPushButton::clicked, this, &ExplorerBottomBarWidget::openSaveDialog);
    connect(m_configure, &QPushButton::clicked, this, &ExplorerBottomBarWidget::openSettingsDialog);
    connect(m_add, &QPushButton::clicked, this, &ExplorerBottomBarWidget::addCurrentSelection);
    connect(import, &QPushButton::clicked, this, &ExplorerBottomBarWidget::importRequested);
    connect(m_remove, &QPushButton::clicked, this, &ExplorerBottomBarWidget::removeSelected);
    connect(m_clear, &QPushButton::clicked, this, &ExplorerBottomBarWidget::clearPlaylist);
    connect(m_strip, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        m_selectedItemID = item->data(Qt::UserRole).toString();
        updateHeader();
    });
    connect(m_strip, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        const QString id = item->data(Qt::UserRole).toString();
        const Wallpaper wallpaper = m_manager->resolveWallpaper(id);
        if (!wallpaper.isValid()) return;
        m_selectedItemID = id;
        m_manager->setCurrentWallpaper(targetScreen(), wallpaper);
        m_manager->kickRotator(targetScreen());
        emit playItemRequested(wallpaper, targetScreen());
        rebuildStrip();
    });
    connect(m_manager, &PlaylistManager::currentChanged, this, [this](int screen) {
        if (screen == targetScreen()) {
            rebuildStrip();
            updateHeader();
        }
    });
    connect(m_manager, &PlaylistManager::savedChanged, this, &ExplorerBottomBarWidget::updateHeader);

    m_manager->ensureScreen(0);
    setCollapsed(m_collapsed);
    rebuildStrip();
    updateHeader();
}

void ExplorerBottomBarWidget::setSelectedWallpaper(const Wallpaper& wallpaper) {
    m_selectedWallpaper = wallpaper;
    updateHeader();
}

void ExplorerBottomBarWidget::setPlayingWallpaper(int screen, const Wallpaper& wallpaper) {
    if (wallpaper.isValid()) m_playingByScreen.insert(screen, wallpaper.id());
    else m_playingByScreen.remove(screen);
    if (screen == targetScreen()) rebuildStrip();
}

void ExplorerBottomBarWidget::rebuildStrip() {
    m_strip->clear();
    const Playlist playlist = m_manager->current(targetScreen());
    const QString playingID = m_playingByScreen.value(targetScreen());
    bool hasItems = false;
    for (const PlaylistItem& item : playlist.items) {
        const Wallpaper wallpaper = m_manager->resolveWallpaper(item.wallpaperID);
        auto* listItem = new QListWidgetItem(m_strip);
        listItem->setData(Qt::UserRole, item.wallpaperID);
        listItem->setIcon(thumbnailFor(wallpaper));
        const QString title = wallpaper.isValid() ? wallpaper.project.title : item.wallpaperID;
        listItem->setText(title);
        listItem->setToolTip(title);
        listItem->setSizeHint(QSize(88, 100));
        if (item.wallpaperID == playingID) {
            listItem->setBackground(QColor(QStringLiteral("#2f6fed")));
        }
        if (item.wallpaperID == m_selectedItemID) {
            listItem->setSelected(true);
        }
        hasItems = true;
    }
    m_strip->setVisible(hasItems);
    m_empty->setVisible(!hasItems);
    updateHeader();
}

void ExplorerBottomBarWidget::updateHeader() {
    const int count = m_manager->current(targetScreen()).items.size();
    m_title->setText(count > 0 ? QStringLiteral("播放列表 (%1)").arg(count) : QStringLiteral("播放列表"));
    m_collapse->setText(m_collapsed ? QStringLiteral("▴") : QStringLiteral("▾"));
    m_collapse->setToolTip(m_collapsed ? QStringLiteral("展开播放列表") : QStringLiteral("收起播放列表"));
    m_load->setEnabled(!m_manager->saved().isEmpty());
    m_save->setEnabled(count > 0);
    m_configure->setEnabled(true);
    m_add->setEnabled(m_selectedWallpaper.isValid());
    m_remove->setEnabled(!m_selectedItemID.isEmpty());
    m_clear->setEnabled(count > 0);
}

void ExplorerBottomBarWidget::setCollapsed(bool collapsed) {
    m_collapsed = collapsed;
    m_body->setVisible(!collapsed);
    QSettings().setValue(QStringLiteral("PlaylistCollapsed"), collapsed);
    updateHeader();
}

int ExplorerBottomBarWidget::targetScreen() const {
    return m_screen->currentData().toInt();
}

void ExplorerBottomBarWidget::openLoadDialog() {
    PlaylistOpenDialog dialog(m_manager, targetScreen(), this);
    dialog.exec();
}

void ExplorerBottomBarWidget::openSaveDialog() {
    PlaylistSaveDialog dialog(m_manager, targetScreen(), this);
    dialog.exec();
}

void ExplorerBottomBarWidget::openSettingsDialog() {
    PlaylistSettingsDialog dialog(m_manager, targetScreen(), this);
    dialog.exec();
}

void ExplorerBottomBarWidget::clearPlaylist() {
    if (QMessageBox::question(this,
                              QStringLiteral("清空播放列表"),
                              QStringLiteral("确定要清空当前播放列表中的全部壁纸吗？")) != QMessageBox::Yes) {
        return;
    }
    m_manager->clear(targetScreen());
    m_selectedItemID.clear();
}

void ExplorerBottomBarWidget::removeSelected() {
    if (m_selectedItemID.isEmpty()) return;
    m_manager->remove(m_selectedItemID, targetScreen());
    m_selectedItemID.clear();
}

void ExplorerBottomBarWidget::addCurrentSelection() {
    if (!m_selectedWallpaper.isValid()) return;
    m_manager->add(m_selectedWallpaper, targetScreen());
    m_selectedItemID = m_selectedWallpaper.id();
    emit addWallpaperRequested(targetScreen());
}

} // namespace Mirage

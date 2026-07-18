#include "ContentView/MainWindow.h"

#include "ContentView/FirstLaunchView.h"
#include "ContentView/ImportPanels.h"
#include "SettingsView/SettingsWidget.h"
#include "Services/Paths.h"
#include "SteamSetup/SteamSetupDialog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QScrollArea>
#include <QScreen>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>

namespace Mirage {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("MirageQt 0.1.0"));
    resize(1100, 760);
    setMinimumSize(1000, 640);

    Paths::ensureBaseDirectories();

    m_settings = new GlobalSettingsService(this);
    m_favorites = new FavoritesManager(this);
    m_library = new WallpaperLibrary(m_settings, this);
    m_steamCMD = new SteamCMDManager(this);
    m_steamAPI = new SteamWebAPI(m_settings, this);
    m_renderer = new RendererController(m_settings, this);

    m_topTabs = new TopTabBarWidget(this);
    m_contentStack = new QStackedWidget(this);
    m_rightStack = new QStackedWidget(this);
    m_preview = new WallpaperPreviewWidget(m_favorites, this);
    m_rightStack->addWidget(m_preview);
    m_rightStack->addWidget(buildWorkshopDetailPage());

    m_contentStack->addWidget(buildInstalledPage());
    m_contentStack->addWidget(buildDiscoverPage());
    m_workshop = new WorkshopWidget(m_steamAPI, m_steamCMD, m_library, this);
    m_contentStack->addWidget(m_workshop);

    auto* left = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(12, 12, 12, 12);
    leftLayout->setSpacing(8);
    leftLayout->addWidget(m_topTabs);
    leftLayout->addWidget(m_contentStack, 1);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(left);
    splitter->addWidget(m_rightStack);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({780, 320});
    setCentralWidget(splitter);

    connect(m_topTabs, &TopTabBarWidget::tabChanged, this, [this](int index) {
        m_contentStack->setCurrentIndex(index);
        m_rightStack->setCurrentIndex(index == 2 ? 1 : 0);
        if (index == 2) m_workshop->search();
    });
    connect(m_topTabs, &TopTabBarWidget::settingsRequested, this, &MainWindow::openSettings);
    connect(m_topTabs, &TopTabBarWidget::displaySettingsRequested, this, [this] {
        QMessageBox::information(this, QStringLiteral("显示器"), QStringLiteral("X11 下按屏幕索引应用壁纸；Wayland 会提示当前会话不支持动态桌面壁纸。"));
    });
    connect(m_topTabs, &TopTabBarWidget::mobileRequested, this, [this] {
        QMessageBox::information(this, QStringLiteral("移动端"), QStringLiteral("移动端同步暂未在 LinuxQt 版本实现。"));
    });
    connect(m_preview, &WallpaperPreviewWidget::favoriteRequested, this, [this](const Wallpaper& wallpaper) {
        m_favorites->toggle(wallpaper.id());
        m_preview->setWallpaper(wallpaper);
        m_wallpaperList->reload();
    });
    connect(m_preview, &WallpaperPreviewWidget::volumeChanged, this, [this](double volume) {
        m_renderer->setVolume(volume);
    });
    connect(m_preview, &WallpaperPreviewWidget::speedChanged, this, [this](double speed) {
        m_renderer->setSpeed(speed);
    });
    connect(m_preview, &WallpaperPreviewWidget::fillModeChanged, this, [this](FillMode mode) {
        m_renderer->setFillMode(mode);
    });
    connect(m_preview, &WallpaperPreviewWidget::propertyChanged, this, [this](const QString& key, const ProjectProperty& property) {
        m_renderer->setProperty(key, property);
    });
    connect(m_preview, &WallpaperPreviewWidget::applyAllRequested, this, [this](const Wallpaper& wallpaper) {
        applyWallpaper(wallpaper, true);
    });
    connect(m_preview, &WallpaperPreviewWidget::stopRequested, m_renderer, &RendererController::stopAll);
    connect(m_preview, &WallpaperPreviewWidget::closeRequested, this, &QWidget::hide);
    connect(m_workshop, &WorkshopWidget::steamSetupRequested, this, &MainWindow::openSteamSetup);
    connect(m_workshop, &WorkshopWidget::itemSelected, this, &MainWindow::showWorkshopDetail);
    connect(m_workshop, &WorkshopWidget::downloadRequested, this, [this](const WorkshopItem& item) {
        m_steamCMD->downloadItem(item.publishedFileId, item.fileSize);
    });
    connect(m_renderer, &RendererController::rendererMessage, this, &MainWindow::showMessage);
    connect(m_steamCMD, &SteamCMDManager::downloadStateChanged, this, [this](const QString& id, const DownloadState& state) {
        statusBar()->showMessage(QStringLiteral("%1: %2").arg(id, state.message), 6000);
        if (state.kind == DownloadStateKind::Completed) m_wallpaperList->reload();
    });

    if (m_wallpaperList->currentWallpaper().isValid()) {
        m_preview->setWallpaper(m_wallpaperList->currentWallpaper());
    }
    setupTray();
}

MainWindow::~MainWindow() {
    m_renderer->stopAll();
}

void MainWindow::applyWallpaper(const Wallpaper& wallpaper, bool allScreens) {
    RenderOptions options = currentRenderOptions();
    QString error;
    bool ok = false;
    if (allScreens) {
        const QList<QScreen*> screens = QGuiApplication::screens();
        for (int i = 0; i < qMax(1, screens.size()); ++i) {
            QString screenError;
            ok = m_renderer->render(wallpaper, i, options, &screenError) || ok;
            if (!screenError.isEmpty()) error = screenError;
        }
    } else {
        ok = m_renderer->render(wallpaper, 0, options, &error);
    }
    if (!ok && !error.isEmpty()) QMessageBox::warning(this, QStringLiteral("应用壁纸"), error);
}

void MainWindow::importWallpaper() {
    const QString selected = ImportPanels::selectWallpaper(this);
    if (selected.isEmpty()) return;
    QString error;
    const QString imported = m_library->importAny(selected, &error);
    if (imported.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导入壁纸"), error);
        return;
    }
    m_wallpaperList->reload();
    statusBar()->showMessage(QStringLiteral("已导入 %1").arg(imported), 5000);
}

void MainWindow::openSettings() {
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("设置"));
    dialog->resize(640, 560);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(new SettingsWidget(m_settings, dialog));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::openSteamSetup() {
    SteamSetupDialog dialog(m_steamCMD, this);
    dialog.exec();
}

void MainWindow::showWorkshopDetail(const WorkshopItem& item) {
    m_workshopTitle->setText(item.title);
    m_workshopMeta->setText(QStringLiteral("%1 · %2 · Workshop ID %3")
                                .arg(item.displayTypeName(), QLocale().formattedDataSize(item.fileSize), item.publishedFileId));
    m_workshopDescription->setText(item.description.isEmpty() ? QStringLiteral("暂无描述") : item.description);
    m_workshopPreview->setPixmap(QIcon::fromTheme("preferences-desktop-wallpaper").pixmap(280, 158));
    if (item.previewImageUrl.isValid()) m_steamAPI->downloadPreviewImage(item.previewImageUrl);
}

QWidget* MainWindow::buildInstalledPage() {
    auto* page = new QWidget(this);
    auto* top = new ExplorerTopBarWidget(page);
    m_filter = new FilterResultsWidget(page);
    m_wallpaperList = new WallpaperListWidget(m_library, m_favorites, page);
    auto* bottom = new ExplorerBottomBarWidget(page);

    auto* middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    middle->addWidget(m_filter);
    middle->addWidget(m_wallpaperList, 1);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(top);
    layout->addLayout(middle, 1);
    layout->addWidget(bottom);

    m_filter->hide();
    connect(top, &ExplorerTopBarWidget::filterToggled, m_filter, [this] { m_filter->setVisible(!m_filter->isVisible()); });
    connect(top, &ExplorerTopBarWidget::refreshRequested, m_wallpaperList, &WallpaperListWidget::reload);
    connect(top, &ExplorerTopBarWidget::searchChanged, m_wallpaperList, &WallpaperListWidget::setSearchText);
    connect(top, &ExplorerTopBarWidget::sortChanged, this, [this, top] {
        m_wallpaperList->setSortText(top->sortText());
    });
    connect(bottom, &ExplorerBottomBarWidget::importRequested, this, &MainWindow::importWallpaper);
    connect(m_wallpaperList, &WallpaperListWidget::wallpaperSelected, m_preview, &WallpaperPreviewWidget::setWallpaper);
    connect(m_wallpaperList, &WallpaperListWidget::applyRequested, this, &MainWindow::applyWallpaper);
    connect(m_wallpaperList, &WallpaperListWidget::importRequested, this, &MainWindow::importWallpaper);
    return page;
}

QWidget* MainWindow::buildDiscoverPage() {
    auto* page = new QWidget(this);
    auto* title = new QLabel(QStringLiteral("发现"), page);
    QFont font = title->font();
    font.setPointSize(font.pointSize() + 8);
    font.setBold(true);
    title->setFont(font);
    auto* text = new QLabel(QStringLiteral("热门趋势、最新发布、订阅最多和评分最高内容使用创意工坊数据。请切换到“创意工坊”浏览和下载。"), page);
    text->setWordWrap(true);
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(title);
    layout->addWidget(text);
    layout->addStretch(1);
    return page;
}

QWidget* MainWindow::buildWorkshopDetailPage() {
    auto* page = new QWidget(this);
    auto* scroll = new QScrollArea(page);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(14);

    m_workshopPreview = new QLabel(content);
    m_workshopPreview->setFixedSize(280, 158);
    m_workshopPreview->setAlignment(Qt::AlignCenter);
    m_workshopPreview->setFrameShape(QFrame::Box);
    m_workshopTitle = new QLabel(QStringLiteral("点击壁纸查看详情"), content);
    m_workshopTitle->setWordWrap(true);
    m_workshopTitle->setAlignment(Qt::AlignCenter);
    m_workshopMeta = new QLabel(content);
    m_workshopMeta->setWordWrap(true);
    m_workshopDescription = new QLabel(QStringLiteral("暂无描述"), content);
    m_workshopDescription->setWordWrap(true);

    auto* center = new QHBoxLayout;
    center->addStretch(1);
    center->addWidget(m_workshopPreview);
    center->addStretch(1);
    layout->addLayout(center);
    layout->addWidget(m_workshopTitle);
    layout->addWidget(m_workshopMeta);
    layout->addWidget(new QLabel(QStringLiteral("标签"), content));
    layout->addWidget(new QLabel(QStringLiteral("描述"), content));
    layout->addWidget(m_workshopDescription);
    layout->addStretch(1);

    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
    return page;
}

RenderOptions MainWindow::currentRenderOptions() const {
    const GlobalSettings& s = m_settings->settings();
    RenderOptions options;
    options.fps = s.fps;
    options.volume = s.masterVolume;
    options.muted = s.globalMuted;
    options.enableSpectrum = s.enableSpectrum;
    return options;
}

void MainWindow::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    m_tray = new QSystemTrayIcon(QIcon::fromTheme("preferences-desktop-wallpaper"), this);
    auto* menu = new QMenu(this);
    menu->addAction(QStringLiteral("暂停"), m_renderer, [this] { m_renderer->pause(); });
    menu->addAction(QStringLiteral("继续"), m_renderer, [this] { m_renderer->resume(); });
    menu->addAction(QStringLiteral("静音"), m_renderer, [this] { m_renderer->setMuted(true); });
    menu->addAction(QStringLiteral("取消静音"), m_renderer, [this] { m_renderer->setMuted(false); });
    menu->addAction(QStringLiteral("停止壁纸"), m_renderer, &RendererController::stopAll);
    menu->addSeparator();
    menu->addAction(QStringLiteral("打开主窗口"), this, [this] { showNormal(); raise(); activateWindow(); });
    menu->addAction(QStringLiteral("退出"), qApp, &QCoreApplication::quit);
    m_tray->setContextMenu(menu);
    m_tray->show();
}

void MainWindow::showMessage(const QString& message) {
    if (message.isEmpty()) return;
    statusBar()->showMessage(message, 7000);
}

} // namespace Mirage

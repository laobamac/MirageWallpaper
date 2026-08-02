#include "ContentView/MainWindow.h"

#include "App/MirageStyle.h"
#include "ContentView/FirstLaunchView.h"
#include "ContentView/ImportPanels.h"
#include "ContentView/Components/ProjectFeedbackBannerWidget.h"
#include "ContentView/Components/ExplorerBottomBarWidget.h"
#include "SettingsView/SettingsView.h"
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
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace Mirage {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Mirage 1.0.0"));
    setMinimumSize(1000, 640);
    const QSize available = QGuiApplication::primaryScreen()->availableGeometry().size();
    resize(qMin(1616, available.width() - 80), qMin(860, available.height() - 80));

    Paths::ensureBaseDirectories();

    m_settings = new GlobalSettingsService(this);
    applyMirageStyle(*qApp, m_settings->settings().appearance);
    m_favorites = new FavoritesManager(this);
    m_library = new WallpaperLibrary(m_settings, this);
    m_steamCMD = new SteamCMDManager(this);
    m_steamAPI = new SteamWebAPI(m_settings, this);
    m_workshopViewModel = new WorkshopViewModel(m_steamAPI, m_steamCMD, m_library, this);
    m_renderer = new RendererController(m_settings, this);
    m_runtimeStore = new WallpaperRuntimeStore(this);
    m_propertyCommandTimer = new QTimer(this);
    m_propertyCommandTimer->setSingleShot(true);
    m_propertyCommandTimer->setInterval(1000 / 60);
    connect(m_propertyCommandTimer, &QTimer::timeout, this, &MainWindow::flushPendingPropertyCommands);
    m_playlist = new PlaylistManager(m_library, m_renderer, this);

    m_topTabs = new TopTabBarWidget(this);
    m_contentStack = new QStackedWidget(this);
    m_rightStack = new QStackedWidget(this);
    m_preview = new WallpaperPreviewWidget(m_favorites, this);
    m_workshopDetail = new WorkshopItemDetail(m_workshopViewModel, m_steamAPI, this);
    m_rightStack->addWidget(m_preview);
    m_rightStack->addWidget(m_workshopDetail);

    m_contentStack->addWidget(buildInstalledPage());
    m_contentStack->addWidget(buildDiscoverPage());
    m_contentStack->addWidget(buildWorkshopPage());

    auto* left = new QWidget(this);
    left->setObjectName(QStringLiteral("mainContentPane"));
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(16, 16, 16, 16);
    leftLayout->setSpacing(6);
    leftLayout->addWidget(m_topTabs);
    leftLayout->addWidget(new ProjectFeedbackBannerWidget(left));
    leftLayout->addWidget(m_contentStack, 1);
    m_bottomBar = new ExplorerBottomBarWidget(m_playlist, m_library, left);
    leftLayout->addWidget(m_bottomBar);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("mainSplitter"));
    m_splitter->setHandleWidth(1);
    m_splitter->setChildrenCollapsible(false);
    m_rightStack->setObjectName(QStringLiteral("previewRail"));
    m_rightStack->setMinimumWidth(320);
    m_rightStack->setMaximumWidth(340);
    m_splitter->addWidget(left);
    m_splitter->addWidget(m_rightStack);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setSizes({1296, 320});
    setCentralWidget(m_splitter);
    statusBar()->hide();

    QSettings uiState;
    if (const QByteArray geometry = uiState.value(QStringLiteral("MainWindow/Geometry")).toByteArray(); !geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    if (const QByteArray splitterState = uiState.value(QStringLiteral("MainWindow/Splitter")).toByteArray(); !splitterState.isEmpty()) {
        m_splitter->restoreState(splitterState);
    }
    setFilterVisible(uiState.value(QStringLiteral("MainWindow/FilterVisible"), false).toBool());

    connect(m_topTabs, &TopTabBarWidget::tabChanged, this, [this](int index) {
        m_contentStack->setCurrentIndex(index);
        m_rightStack->setCurrentIndex(index == 0 || m_showWorkshopCustomization ? 0 : 1);
        if (index == 1) m_discover->activate();
        if (index == 2) m_workshop->activate();
    });
    connect(m_topTabs, &TopTabBarWidget::settingsRequested, this, &MainWindow::openSettings);
    connect(m_topTabs, &TopTabBarWidget::displaySettingsRequested, this, [this] {
        QMessageBox::information(this, QStringLiteral("显示器"), QStringLiteral("KDE Plasma 使用稳定显示器标识路由动态壁纸；显示表面由 Plasma 壁纸插件管理。"));
    });
    connect(m_topTabs, &TopTabBarWidget::mobileRequested, this, [this] {
        QMessageBox::information(this, QStringLiteral("移动端"), QStringLiteral("移动端同步暂未在 LinuxQt 版本实现。"));
    });
    connect(m_preview, &WallpaperPreviewWidget::favoriteRequested, this, [this](const Wallpaper& wallpaper) {
        m_favorites->toggle(wallpaper.id());
        selectWallpaper(wallpaper);
        m_wallpaperList->reload();
    });
    connect(m_preview, &WallpaperPreviewWidget::volumeChanged, this, &MainWindow::handleVolumeChanged);
    connect(m_preview, &WallpaperPreviewWidget::speedChanged, this, &MainWindow::handleSpeedChanged);
    connect(m_preview, &WallpaperPreviewWidget::fillModeChanged, this, &MainWindow::handleFillModeChanged);
    connect(m_preview, &WallpaperPreviewWidget::propertyChanged, this, &MainWindow::handlePropertyChanged);
    connect(m_preview, &WallpaperPreviewWidget::resetDefaultsRequested, this, &MainWindow::handleResetDefaults);
    connect(m_preview, &WallpaperPreviewWidget::applyAllRequested, this, [this](const Wallpaper& wallpaper) {
        applyWallpaper(wallpaper, true);
    });
    connect(m_preview, &WallpaperPreviewWidget::stopRequested, m_renderer, &RendererController::stopAll);
    connect(m_preview, &WallpaperPreviewWidget::closeRequested, this, &QWidget::hide);
    connect(m_bottomBar, &ExplorerBottomBarWidget::importRequested, this, &MainWindow::importWallpaper);
    connect(m_bottomBar, &ExplorerBottomBarWidget::playItemRequested, this,
            [this](const Wallpaper& wallpaper, int screen) {
        QString error;
        if (!m_renderer->render(wallpaper, screen, renderOptionsFor(wallpaper), &error) && !error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("应用壁纸"), error);
        } else {
            m_playlist->setCurrentWallpaper(screen, wallpaper);
            m_bottomBar->setPlayingWallpaper(screen, wallpaper);
        }
    });
    connect(m_playlist, &PlaylistManager::applyWallpaperRequested, this,
            [this](const Wallpaper& wallpaper, int screen) {
        QString error;
        if (!m_renderer->render(wallpaper, screen, renderOptionsFor(wallpaper), &error) && !error.isEmpty()) {
            showMessage(error);
        } else {
            m_playlist->setCurrentWallpaper(screen, wallpaper);
            m_bottomBar->setPlayingWallpaper(screen, wallpaper);
        }
    });
    connect(m_wallpaperList, &WallpaperListWidget::wallpaperSelected, m_bottomBar,
            &ExplorerBottomBarWidget::setSelectedWallpaper);
    connect(m_wallpaperList, &WallpaperListWidget::addToPlaylistRequested, this,
            [this](const Wallpaper& wallpaper) {
        m_playlist->add(wallpaper, 0);
        m_bottomBar->setSelectedWallpaper(wallpaper);
        showMessage(QStringLiteral("已加入播放列表"));
    });
    connect(m_workshop, &WorkshopView::steamSetupRequested, this, &MainWindow::openSteamSetup);
    connect(m_workshopDetail, &WorkshopItemDetail::steamSetupRequested, this, &MainWindow::openSteamSetup);
    connect(m_workshopDetail, &WorkshopItemDetail::closeRequested, this, &QWidget::hide);
    connect(m_workshopViewModel, &WorkshopViewModel::steamSetupRequested, this, &MainWindow::openSteamSetup);
    connect(m_workshop, &WorkshopView::settingsRequested, this, [this] { openSettingsPage(1); });
    connect(m_discover, &DiscoverView::settingsRequested, this, [this] { openSettingsPage(1); });
    connect(m_workshopViewModel, &WorkshopViewModel::navigateToWorkshopRequested, this, [this] {
        m_topTabs->setCurrentIndex(2);
    });
    connect(m_workshopViewModel, &WorkshopViewModel::selectedItemChanged, this,
            [this](const WorkshopItem& item, bool hasSelection) {
        m_showWorkshopCustomization = false;
        if (hasSelection) m_workshopDetail->setItem(item);
        else m_workshopDetail->clearItem();
        if (m_topTabs->currentIndex() != 0) m_rightStack->setCurrentIndex(1);
    });
    connect(m_workshopViewModel, &WorkshopViewModel::installedWallpaperRequested, this,
            [this](const Wallpaper& wallpaper) {
        m_showWorkshopCustomization = true;
        selectWallpaper(wallpaper);
        m_rightStack->setCurrentIndex(0);
    });
    connect(m_workshopViewModel, &WorkshopViewModel::presetDependencyRequested, this,
            [this](const QString& presetId,
                   const QString& presetTitle,
                   const QString& dependencyId,
                   const WorkshopItem& dependencyItem) {
        const QString size = dependencyItem.fileSize > 0
            ? QStringLiteral("（%1）").arg(dependencyItem.formattedFileSize())
            : QString();
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("需要基础壁纸"),
            QStringLiteral("预设“%1”需要基础壁纸“%2”%3才能使用。是否一起下载？")
                .arg(presetTitle, dependencyItem.title, size),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            m_workshopViewModel->confirmPresetDependencyDownload(presetId, dependencyId, dependencyItem);
        }
    });
    connect(m_workshopViewModel, &WorkshopViewModel::workshopItemDownloaded, m_wallpaperList, &WallpaperListWidget::reload);
    connect(m_settings, &GlobalSettingsService::settingsChanged, this, [this](const GlobalSettings& settings) {
        applyMirageStyle(*qApp, settings.appearance);
        if (m_selectedWallpaper.isValid()) {
            const WallpaperRuntimeState runtime = m_runtimeStore->loadRuntime(m_selectedWallpaper);
            const double volume = runtime.volume * settings.masterVolume;
            const bool muted = runtime.muted || settings.globalMuted || runtime.volume <= 0.0;
            m_renderer->setVolume(volume);
            m_renderer->setMuted(muted);
        } else {
            m_renderer->setVolume(settings.masterVolume);
            m_renderer->setMuted(settings.globalMuted);
        }
        m_renderer->setFps(settings.fps);
        m_wallpaperList->reload();
        m_workshopViewModel->reloadOnlineContent();
    });
    connect(m_renderer, &RendererController::rendererMessage, this, &MainWindow::showMessage);
    connect(m_steamCMD, &SteamCMDManager::downloadStateChanged, this, [this](const QString& id, const DownloadState& state) {
        statusBar()->showMessage(QStringLiteral("%1: %2").arg(id, state.message), 6000);
        if (state.kind == DownloadStateKind::Completed) m_wallpaperList->reload();
    });

    if (m_wallpaperList->currentWallpaper().isValid()) {
        selectWallpaper(m_wallpaperList->currentWallpaper());
    }
    m_playlist->startRotators();
    setupTray();
}

MainWindow::~MainWindow() {
    m_renderer->stopAll();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings uiState;
    uiState.setValue(QStringLiteral("MainWindow/Geometry"), saveGeometry());
    if (m_splitter) uiState.setValue(QStringLiteral("MainWindow/Splitter"), m_splitter->saveState());
    QMainWindow::closeEvent(event);
}

void MainWindow::applyWallpaper(const Wallpaper& wallpaper, bool allScreens) {
    if (wallpaper.isValid()) {
        selectWallpaper(wallpaper);
    }
    RenderOptions options = renderOptionsFor(wallpaper);
    QString error;
    bool ok = false;
    if (allScreens) {
        const QList<QScreen*> screens = QGuiApplication::screens();
        for (int i = 0; i < qMax(1, screens.size()); ++i) {
            QString screenError;
            if (m_renderer->render(wallpaper, i, options, &screenError)) {
                ok = true;
                m_playlist->setCurrentWallpaper(i, wallpaper);
                m_bottomBar->setPlayingWallpaper(i, wallpaper);
                m_playlist->kickRotator(i);
            }
            if (!screenError.isEmpty()) error = screenError;
        }
    } else {
        ok = m_renderer->render(wallpaper, 0, options, &error);
        if (ok) {
            m_playlist->setCurrentWallpaper(0, wallpaper);
            m_bottomBar->setPlayingWallpaper(0, wallpaper);
            m_playlist->kickRotator(0);
        }
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
    openSettingsPage(-1);
}

void MainWindow::openSettingsPage(int page) {
    if (m_settingsDialog) {
        if (page >= 0) {
            if (auto* settingsView = m_settingsDialog->findChild<SettingsView*>()) settingsView->selectPage(page);
        }
        m_settingsDialog->showNormal();
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    m_settingsDialog = dialog;
    dialog->setWindowTitle(QStringLiteral("设置"));
    dialog->resize(680, 680);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* settingsView = new SettingsView(m_settings, dialog);
    if (page >= 0) settingsView->selectPage(page);
    layout->addWidget(settingsView);
    connect(settingsView, &SettingsView::accepted, dialog, &QDialog::accept);
    connect(settingsView, &SettingsView::cancelled, dialog, &QDialog::reject);
    QSettings uiState;
    if (const QByteArray geometry = uiState.value(QStringLiteral("Settings/Geometry")).toByteArray(); !geometry.isEmpty()) {
        dialog->restoreGeometry(geometry);
    }
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        QSettings().setValue(QStringLiteral("Settings/Geometry"), dialog->saveGeometry());
        if (m_settingsDialog == dialog) m_settingsDialog = nullptr;
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::openSteamSetup() {
    SteamSetupDialog dialog(m_steamCMD, this);
    dialog.exec();
}

QWidget* MainWindow::buildInstalledPage() {
    auto* page = new QWidget(this);
    m_installedTop = new ExplorerTopBarWidget(page);
    m_filter = new FilterResultsWidget(page);
    m_wallpaperList = new WallpaperListWidget(m_library, m_favorites, page);

    auto* middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    middle->setSpacing(10);
    middle->addWidget(m_filter);
    middle->addWidget(m_wallpaperList, 1);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(m_installedTop);
    layout->addLayout(middle, 1);

    m_filter->hide();
    connect(m_installedTop, &ExplorerTopBarWidget::filterToggled, this, &MainWindow::setFilterVisible);
    connect(m_installedTop, &ExplorerTopBarWidget::refreshRequested, m_wallpaperList, &WallpaperListWidget::reload);
    connect(m_installedTop, &ExplorerTopBarWidget::searchChanged, m_wallpaperList, &WallpaperListWidget::setSearchText);
    connect(m_installedTop, &ExplorerTopBarWidget::sortChanged, this, [this] {
        m_wallpaperList->setSortText(m_installedTop->sortText());
        m_wallpaperList->setSortDescending(m_installedTop->descending());
    });
    connect(m_filter, &FilterResultsWidget::filtersChanged, this, [this] {
        m_wallpaperList->setFilterState(m_filter->filterState());
    });
    connect(m_wallpaperList, &WallpaperListWidget::wallpaperSelected, this, &MainWindow::selectWallpaper);
    connect(m_wallpaperList, &WallpaperListWidget::applyRequested, this, &MainWindow::applyWallpaper);
    connect(m_wallpaperList, &WallpaperListWidget::importRequested, this, &MainWindow::importWallpaper);
    m_wallpaperList->setFilterState(m_filter->filterState());
    return page;
}

QWidget* MainWindow::buildDiscoverPage() {
    m_discover = new DiscoverView(m_workshopViewModel, m_steamAPI, m_settings, this);
    return m_discover;
}

QWidget* MainWindow::buildWorkshopPage() {
    auto* page = new QWidget(this);
    m_workshopTop = new ExplorerTopBarWidget(page);
    m_workshopFilter = new WorkshopFilterSidebar(m_workshopViewModel, page);
    m_workshop = new WorkshopView(m_workshopViewModel, m_steamAPI, m_steamCMD, m_settings, page);
    m_workshopFilter->hide();

    auto* middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    middle->setSpacing(10);
    middle->addWidget(m_workshopFilter);
    middle->addWidget(m_workshop, 1);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(m_workshopTop);
    layout->addLayout(middle, 1);

    connect(m_workshopTop, &ExplorerTopBarWidget::filterToggled, this, &MainWindow::setFilterVisible);
    connect(m_workshopTop, &ExplorerTopBarWidget::refreshRequested, m_workshopViewModel, &WorkshopViewModel::search);
    connect(m_workshopTop, &ExplorerTopBarWidget::searchChanged, m_workshopViewModel, &WorkshopViewModel::setSearchText);
    connect(m_workshopTop, &ExplorerTopBarWidget::sortChanged, this, [this] {
        if (m_workshopTop->sortText() == QStringLiteral("评分")) {
            m_workshopViewModel->setSortOrder(WorkshopSortOrder::TopRated);
        } else if (m_workshopTop->sortText() == QStringLiteral("文件大小")) {
            m_workshopViewModel->setSortOrder(WorkshopSortOrder::MostSubscribed);
        } else {
            m_workshopViewModel->setSortOrder(WorkshopSortOrder::Trending);
        }
    });
    return page;
}

void MainWindow::setFilterVisible(bool visible) {
    if (m_filter) m_filter->setVisible(visible);
    if (m_workshopFilter) m_workshopFilter->setVisible(visible);
    if (m_installedTop) m_installedTop->setFilterVisible(visible);
    if (m_workshopTop) m_workshopTop->setFilterVisible(visible);
    QSettings().setValue(QStringLiteral("MainWindow/FilterVisible"), visible);
}

RenderOptions MainWindow::renderOptionsFor(const Wallpaper& wallpaper) const {
    const GlobalSettings& s = m_settings->settings();
    const WallpaperRuntimeState runtime = wallpaper.isValid()
        ? m_runtimeStore->loadRuntime(wallpaper)
        : WallpaperRuntimeState{};

    RenderOptions options;
    options.fps = s.fps;
    options.volume = runtime.volume * s.masterVolume;
    options.muted = runtime.muted || s.globalMuted || runtime.volume <= 0.0;
    options.speed = runtime.speed;
    options.fillMode = runtime.fillMode;
    options.enableSpectrum = s.enableSpectrum;
    options.loadFromMemory = s.wallpaperLoadSource == QStringLiteral("memory");
    if (wallpaper.isValid()) {
        options.userProperties = m_runtimeStore->effectiveProperties(wallpaper, runtime);
    }
    return options;
}

void MainWindow::selectWallpaper(const Wallpaper& wallpaper) {
    if (m_selectedWallpaper.isValid()
        && m_selectedWallpaper.id() != wallpaper.id()
        && m_propertyCommandTimer
        && m_propertyCommandTimer->isActive()) {
        flushPendingPropertyCommands();
    }

    m_selectedWallpaper = wallpaper;
    m_pendingPropertyCommands.clear();
    if (m_propertyCommandTimer) m_propertyCommandTimer->stop();

    m_preview->setWallpaper(wallpaper);
    if (wallpaper.isValid()) {
        m_preview->setRuntime(m_runtimeStore->loadRuntime(wallpaper));
    } else {
        m_preview->setRuntime(WallpaperRuntimeState{});
    }
}

void MainWindow::handleVolumeChanged(double volume) {
    if (!m_selectedWallpaper.isValid()) return;
    m_runtimeStore->setVolume(m_selectedWallpaper, volume);
    const GlobalSettings& settings = m_settings->settings();
    const WallpaperRuntimeState runtime = m_runtimeStore->loadRuntime(m_selectedWallpaper);
    m_renderer->setVolume(runtime.volume * settings.masterVolume);
    m_renderer->setMuted(runtime.muted || settings.globalMuted || runtime.volume <= 0.0);
}

void MainWindow::handleSpeedChanged(double speed) {
    if (!m_selectedWallpaper.isValid()) return;
    m_runtimeStore->setSpeed(m_selectedWallpaper, speed);
    m_renderer->setSpeed(speed);
}

void MainWindow::handleFillModeChanged(FillMode mode) {
    if (!m_selectedWallpaper.isValid()) return;
    m_runtimeStore->setFillMode(m_selectedWallpaper, mode);
    m_renderer->setFillMode(mode);
}

void MainWindow::handlePropertyChanged(const QString& key, const ProjectProperty& property) {
    if (!m_selectedWallpaper.isValid() || key.isEmpty()) return;

    const ProjectProperty normalized = m_runtimeStore->setProperty(m_selectedWallpaper, key, property.value);
    if (!m_selectedWallpaper.project.properties.contains(key)) return;

    switch (m_selectedWallpaper.kind()) {
    case WallpaperKind::Scene:
    case WallpaperKind::Web:
        m_pendingPropertyCommands.insert(key, normalized);
        if (m_propertyCommandTimer && !m_propertyCommandTimer->isActive()) {
            m_propertyCommandTimer->start();
        }
        break;
    case WallpaperKind::Video:
    case WallpaperKind::Unsupported:
        break;
    }
}

void MainWindow::handleResetDefaults() {
    if (!m_selectedWallpaper.isValid()) return;

    m_pendingPropertyCommands.clear();
    if (m_propertyCommandTimer) m_propertyCommandTimer->stop();

    m_runtimeStore->resetRuntime(m_selectedWallpaper);
    const Wallpaper wallpaper = m_selectedWallpaper;
    m_preview->setRuntime(m_runtimeStore->loadRuntime(wallpaper));
    reapplyPlayingWallpaper(wallpaper);
}

void MainWindow::flushPendingPropertyCommands() {
    if (m_pendingPropertyCommands.isEmpty()) return;
    if (!m_selectedWallpaper.isValid()) {
        m_pendingPropertyCommands.clear();
        return;
    }

    const auto commands = m_pendingPropertyCommands;
    m_pendingPropertyCommands.clear();
    for (auto it = commands.constBegin(); it != commands.constEnd(); ++it) {
        m_renderer->setProperty(it.key(), it.value());
    }
}

void MainWindow::reapplyPlayingWallpaper(const Wallpaper& wallpaper) {
    if (!wallpaper.isValid()) return;

    const RenderOptions options = renderOptionsFor(wallpaper);
    for (int screen : m_renderer->activeScreens()) {
        QString error;
        if (m_renderer->render(wallpaper, screen, options, &error)) {
            m_playlist->setCurrentWallpaper(screen, wallpaper);
            m_bottomBar->setPlayingWallpaper(screen, wallpaper);
        } else if (!error.isEmpty()) {
            showMessage(error);
        }
    }
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

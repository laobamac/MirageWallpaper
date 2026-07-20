#pragma once

#include "ContentView/Components/ExplorerBottomBarWidget.h"
#include "ContentView/Components/ExplorerTopBarWidget.h"
#include "ContentView/Components/FilterResultsWidget.h"
#include "ContentView/Components/TopTabBarWidget.h"
#include "ContentView/Components/WallpaperListWidget.h"
#include "ContentView/Components/WallpaperPreviewWidget.h"
#include "ContentView/Components/Discover/DiscoverView.h"
#include "ContentView/Components/Workshop/WorkshopFilterSidebar.h"
#include "ContentView/Components/Workshop/WorkshopItemDetail.h"
#include "ContentView/Components/Workshop/WorkshopView.h"
#include "Services/FavoritesManager.h"
#include "Services/GlobalSettingsService.h"
#include "Services/RendererController.h"
#include "Services/SteamCMDManager.h"
#include "Services/SteamWebAPI.h"
#include "Services/WallpaperLibrary.h"

#include <QLabel>
#include <QDialog>
#include <QMainWindow>
#include <QPointer>
#include <QStackedWidget>
#include <QSplitter>
#include <QSystemTrayIcon>

namespace Mirage {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void applyWallpaper(const Mirage::Wallpaper& wallpaper, bool allScreens);
    void importWallpaper();
    void openSettings();
    void openSteamSetup();

private:
    QWidget* buildInstalledPage();
    QWidget* buildDiscoverPage();
    QWidget* buildWorkshopPage();
    void openSettingsPage(int page);
    void setFilterVisible(bool visible);
    RenderOptions currentRenderOptions() const;
    void setupTray();
    void showMessage(const QString& message);

    GlobalSettingsService* m_settings = nullptr;
    FavoritesManager* m_favorites = nullptr;
    WallpaperLibrary* m_library = nullptr;
    SteamCMDManager* m_steamCMD = nullptr;
    SteamWebAPI* m_steamAPI = nullptr;
    WorkshopViewModel* m_workshopViewModel = nullptr;
    RendererController* m_renderer = nullptr;

    TopTabBarWidget* m_topTabs = nullptr;
    ExplorerTopBarWidget* m_installedTop = nullptr;
    ExplorerTopBarWidget* m_workshopTop = nullptr;
    QStackedWidget* m_contentStack = nullptr;
    QStackedWidget* m_rightStack = nullptr;
    QSplitter* m_splitter = nullptr;
    WallpaperListWidget* m_wallpaperList = nullptr;
    FilterResultsWidget* m_filter = nullptr;
    WallpaperPreviewWidget* m_preview = nullptr;
    DiscoverView* m_discover = nullptr;
    WorkshopView* m_workshop = nullptr;
    WorkshopFilterSidebar* m_workshopFilter = nullptr;
    WorkshopItemDetail* m_workshopDetail = nullptr;
    bool m_showWorkshopCustomization = false;
    QSystemTrayIcon* m_tray = nullptr;
    QPointer<QDialog> m_settingsDialog;
};

} // namespace Mirage

#pragma once

#include "ContentView/Components/ExplorerBottomBarWidget.h"
#include "ContentView/Components/ExplorerTopBarWidget.h"
#include "ContentView/Components/FilterResultsWidget.h"
#include "ContentView/Components/TopTabBarWidget.h"
#include "ContentView/Components/WallpaperListWidget.h"
#include "ContentView/Components/WallpaperPreviewWidget.h"
#include "ContentView/Components/WorkshopWidget.h"
#include "Services/FavoritesManager.h"
#include "Services/GlobalSettingsService.h"
#include "Services/RendererController.h"
#include "Services/SteamCMDManager.h"
#include "Services/SteamWebAPI.h"
#include "Services/WallpaperLibrary.h"

#include <QLabel>
#include <QMainWindow>
#include <QStackedWidget>
#include <QSystemTrayIcon>

namespace Mirage {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void applyWallpaper(const Mirage::Wallpaper& wallpaper, bool allScreens);
    void importWallpaper();
    void openSettings();
    void openSteamSetup();
    void showWorkshopDetail(const Mirage::WorkshopItem& item);

private:
    QWidget* buildInstalledPage();
    QWidget* buildDiscoverPage();
    QWidget* buildWorkshopDetailPage();
    RenderOptions currentRenderOptions() const;
    void setupTray();
    void showMessage(const QString& message);

    GlobalSettingsService* m_settings = nullptr;
    FavoritesManager* m_favorites = nullptr;
    WallpaperLibrary* m_library = nullptr;
    SteamCMDManager* m_steamCMD = nullptr;
    SteamWebAPI* m_steamAPI = nullptr;
    RendererController* m_renderer = nullptr;

    TopTabBarWidget* m_topTabs = nullptr;
    QStackedWidget* m_contentStack = nullptr;
    QStackedWidget* m_rightStack = nullptr;
    WallpaperListWidget* m_wallpaperList = nullptr;
    FilterResultsWidget* m_filter = nullptr;
    WallpaperPreviewWidget* m_preview = nullptr;
    WorkshopWidget* m_workshop = nullptr;
    QWidget* m_workshopDetail = nullptr;
    QLabel* m_workshopPreview = nullptr;
    QLabel* m_workshopTitle = nullptr;
    QLabel* m_workshopMeta = nullptr;
    QLabel* m_workshopDescription = nullptr;
    QSystemTrayIcon* m_tray = nullptr;
};

} // namespace Mirage

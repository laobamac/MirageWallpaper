#pragma once

#include "Services/GlobalSettingsService.h"
#include "Services/WorkshopViewModel.h"

#include <QWidget>

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QToolButton;

namespace Mirage {

class DownloadPopover;
class WorkshopSearchBar;

class WorkshopView final : public QWidget {
    Q_OBJECT

public:
    explicit WorkshopView(WorkshopViewModel* viewModel,
                          SteamWebAPI* api,
                          SteamCMDManager* steamCMD,
                          GlobalSettingsService* settings,
                          QWidget* parent = nullptr);

public slots:
    void activate();

signals:
    void steamSetupRequested();
    void settingsRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildList();
    void updateBrowseState();
    void updateSteamState();
    void updateAPIKeyBanner();
    void updateGridMetrics();
    void selectCurrentModelItem();

    WorkshopViewModel* m_viewModel = nullptr;
    SteamWebAPI* m_api = nullptr;
    SteamCMDManager* m_steamCMD = nullptr;
    GlobalSettingsService* m_settings = nullptr;

    QWidget* m_apiKeyBanner = nullptr;
    WorkshopSearchBar* m_searchBar = nullptr;
    QToolButton* m_downloadButton = nullptr;
    QLabel* m_downloadBadge = nullptr;
    QWidget* m_account = nullptr;
    QWidget* m_setupBanner = nullptr;
    QLabel* m_setupSummary = nullptr;
    QStackedWidget* m_stack = nullptr;
    QListWidget* m_list = nullptr;
    QProgressBar* m_busy = nullptr;
    QLabel* m_emptyTitle = nullptr;
    QLabel* m_emptyDetail = nullptr;
    QPushButton* m_retry = nullptr;
    QPushButton* m_previous = nullptr;
    QPushButton* m_next = nullptr;
    QLabel* m_page = nullptr;
    DownloadPopover* m_downloadPopover = nullptr;
};

} // namespace Mirage

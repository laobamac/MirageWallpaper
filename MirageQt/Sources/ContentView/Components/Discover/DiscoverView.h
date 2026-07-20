#pragma once

#include "Services/GlobalSettingsService.h"
#include "Services/WorkshopViewModel.h"

#include <QWidget>

class QStackedWidget;

namespace Mirage {

class DiscoverView final : public QWidget {
    Q_OBJECT

public:
    explicit DiscoverView(WorkshopViewModel* viewModel,
                          SteamWebAPI* api,
                          GlobalSettingsService* settings,
                          QWidget* parent = nullptr);

public slots:
    void activate();

signals:
    void settingsRequested();

private:
    void updateState();
    void updateAPIKeyBanner();

    WorkshopViewModel* m_viewModel = nullptr;
    GlobalSettingsService* m_settings = nullptr;
    QWidget* m_apiKeyBanner = nullptr;
    QStackedWidget* m_stack = nullptr;
};

} // namespace Mirage

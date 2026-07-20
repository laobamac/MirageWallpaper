#pragma once

#include "Services/WorkshopViewModel.h"

#include <QTimer>
#include <QWidget>

class QLabel;

namespace Mirage {

class DiscoverBannerView final : public QWidget {
    Q_OBJECT

public:
    explicit DiscoverBannerView(WorkshopViewModel* viewModel,
                                SteamWebAPI* api,
                                QWidget* parent = nullptr);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void rebuild();
    void showCurrent();

    WorkshopViewModel* m_viewModel = nullptr;
    SteamWebAPI* m_api = nullptr;
    QLabel* m_image = nullptr;
    QLabel* m_caption = nullptr;
    QLabel* m_dots = nullptr;
    QTimer m_timer;
    int m_index = 0;
};

} // namespace Mirage

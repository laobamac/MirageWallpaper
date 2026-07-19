#pragma once

#include "Services/WorkshopViewModel.h"

#include <QWidget>

class QLabel;
class QStackedWidget;
class QVBoxLayout;

namespace Mirage {

class WorkshopItemDetail final : public QWidget {
    Q_OBJECT

public:
    explicit WorkshopItemDetail(WorkshopViewModel* viewModel,
                                SteamWebAPI* api,
                                QWidget* parent = nullptr);

public slots:
    void setItem(const Mirage::WorkshopItem& item);
    void clearItem();

signals:
    void steamSetupRequested();
    void closeRequested();

private:
    void updateContent();
    void rebuildDownloadSection();
    QLabel* sectionHeader(const QString& text, QWidget* parent);

    WorkshopViewModel* m_viewModel = nullptr;
    SteamWebAPI* m_api = nullptr;
    std::optional<WorkshopItem> m_item;

    QStackedWidget* m_stack = nullptr;
    QLabel* m_preview = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_presetNotice = nullptr;
    QLabel* m_subscriptions = nullptr;
    QLabel* m_favorites = nullptr;
    QLabel* m_views = nullptr;
    QLabel* m_meta = nullptr;
    QLabel* m_tags = nullptr;
    QLabel* m_description = nullptr;
    QLabel* m_workshopId = nullptr;
    QLabel* m_updatedAt = nullptr;
    QWidget* m_downloadSection = nullptr;
    QVBoxLayout* m_downloadLayout = nullptr;
};

} // namespace Mirage

#pragma once

#include "Services/WorkshopViewModel.h"

#include <QColor>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;

namespace Mirage {

class DiscoverSectionView final : public QWidget {
    Q_OBJECT

public:
    explicit DiscoverSectionView(const QString& title,
                                 const QString& iconName,
                                 const QColor& iconColor,
                                 DiscoverCollection collection,
                                 WorkshopViewModel* viewModel,
                                 SteamWebAPI* api,
                                 QWidget* parent = nullptr);

private:
    void rebuild();
    void showAll();
    void selectCurrentModelItem();

    DiscoverCollection m_collection;
    WorkshopViewModel* m_viewModel = nullptr;
    SteamWebAPI* m_api = nullptr;
    QListWidget* m_list = nullptr;
};

} // namespace Mirage

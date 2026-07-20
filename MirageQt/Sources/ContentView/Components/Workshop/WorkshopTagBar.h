#pragma once

#include "Services/WorkshopViewModel.h"

#include <QHash>
#include <QWidget>

class QToolButton;

namespace Mirage {

class WorkshopTagBar final : public QWidget {
    Q_OBJECT

public:
    explicit WorkshopTagBar(WorkshopViewModel* viewModel, QWidget* parent = nullptr);

private:
    void syncFromViewModel();

    WorkshopViewModel* m_viewModel = nullptr;
    QHash<QString, QToolButton*> m_buttons;
    QToolButton* m_clear = nullptr;
};

} // namespace Mirage

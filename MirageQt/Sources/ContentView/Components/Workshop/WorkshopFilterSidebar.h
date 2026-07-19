#pragma once

#include "Services/WorkshopViewModel.h"

#include <QHash>
#include <QWidget>

class QCheckBox;

namespace Mirage {

class WorkshopFilterSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit WorkshopFilterSidebar(WorkshopViewModel* viewModel, QWidget* parent = nullptr);

private:
    void syncFromViewModel();

    WorkshopViewModel* m_viewModel = nullptr;
    QHash<WorkshopTypeFilter, QCheckBox*> m_typeChecks;
    QHash<QString, QCheckBox*> m_tagChecks;
};

} // namespace Mirage

#pragma once

#include "Services/WorkshopViewModel.h"

#include <QComboBox>
#include <QLineEdit>
#include <QWidget>

namespace Mirage {

class WorkshopSearchBar final : public QWidget {
    Q_OBJECT

public:
    explicit WorkshopSearchBar(WorkshopViewModel* viewModel, QWidget* parent = nullptr);

private:
    void syncFromViewModel();

    WorkshopViewModel* m_viewModel = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_sort = nullptr;
};

} // namespace Mirage

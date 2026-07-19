#include "ContentView/Components/Workshop/WorkshopFilterSidebar.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace Mirage {

WorkshopFilterSidebar::WorkshopFilterSidebar(WorkshopViewModel* viewModel, QWidget* parent)
    : QWidget(parent)
    , m_viewModel(viewModel) {
    setFixedWidth(225);

    auto* content = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(10, 10, 10, 10);
    contentLayout->setSpacing(18);

    auto* reset = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("重置筛选"), content);
    reset->setProperty("accent", true);
    contentLayout->addWidget(reset);

    auto* types = new QGroupBox(QStringLiteral("类型"), content);
    auto* typeLayout = new QVBoxLayout(types);
    for (WorkshopTypeFilter type : {WorkshopTypeFilter::All,
                                    WorkshopTypeFilter::Scene,
                                    WorkshopTypeFilter::Web,
                                    WorkshopTypeFilter::Video,
                                    WorkshopTypeFilter::Preset}) {
        auto* check = new QCheckBox(workshopTypeLabel(type), types);
        m_typeChecks.insert(type, check);
        typeLayout->addWidget(check);
        connect(check, &QCheckBox::toggled, this, [this, type](bool checked) {
            if (checked) m_viewModel->setTypeFilter(type);
        });
    }
    contentLayout->addWidget(types);

    auto* tags = new QGroupBox(QStringLiteral("标签"), content);
    auto* tagLayout = new QVBoxLayout(tags);
    auto* tagActions = new QHBoxLayout;
    auto* selectAll = new QPushButton(QStringLiteral("全选"), tags);
    auto* clear = new QPushButton(QStringLiteral("清空"), tags);
    selectAll->setProperty("flatAction", true);
    clear->setProperty("flatAction", true);
    tagActions->addWidget(selectAll);
    tagActions->addWidget(clear);
    tagActions->addStretch(1);
    tagLayout->addLayout(tagActions);

    for (const WorkshopTag& tag : workshopTags()) {
        auto* check = new QCheckBox(tag.displayName, tags);
        m_tagChecks.insert(tag.value, check);
        tagLayout->addWidget(check);
        connect(check, &QCheckBox::toggled, this, [this, value = tag.value] {
            m_viewModel->toggleTag(value);
        });
    }
    contentLayout->addWidget(tags);
    contentLayout->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);

    connect(reset, &QPushButton::clicked, m_viewModel, &WorkshopViewModel::clearFilters);
    connect(selectAll, &QPushButton::clicked, m_viewModel, &WorkshopViewModel::selectAllTags);
    connect(clear, &QPushButton::clicked, m_viewModel, &WorkshopViewModel::clearTags);
    connect(m_viewModel, &WorkshopViewModel::filtersChanged, this, &WorkshopFilterSidebar::syncFromViewModel);
    syncFromViewModel();
}

void WorkshopFilterSidebar::syncFromViewModel() {
    for (auto it = m_typeChecks.begin(); it != m_typeChecks.end(); ++it) {
        const QSignalBlocker blocker(it.value());
        it.value()->setChecked(it.key() == m_viewModel->typeFilter());
    }
    for (auto it = m_tagChecks.begin(); it != m_tagChecks.end(); ++it) {
        const QSignalBlocker blocker(it.value());
        it.value()->setChecked(m_viewModel->selectedTags().contains(it.key()));
    }
}

} // namespace Mirage

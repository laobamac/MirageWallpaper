#include "ContentView/Components/Workshop/WorkshopTagBar.h"

#include <QHBoxLayout>
#include <QScrollArea>
#include <QToolButton>

namespace Mirage {

WorkshopTagBar::WorkshopTagBar(WorkshopViewModel* viewModel, QWidget* parent)
    : QWidget(parent)
    , m_viewModel(viewModel) {
    auto* content = new QWidget(this);
    auto* row = new QHBoxLayout(content);
    row->setContentsMargins(0, 2, 0, 2);
    row->setSpacing(6);

    const QSet<QString> visible = {
        QStringLiteral("Anime"), QStringLiteral("Nature"), QStringLiteral("Abstract"),
        QStringLiteral("Landscape"), QStringLiteral("Sci-Fi"), QStringLiteral("Cartoon"),
        QStringLiteral("Cyberpunk"), QStringLiteral("Fantasy"), QStringLiteral("Girl"),
        QStringLiteral("Game"), QStringLiteral("Animal"), QStringLiteral("Music"),
        QStringLiteral("Vehicle"), QStringLiteral("Technology"), QStringLiteral("City"),
        QStringLiteral("Space"), QStringLiteral("Dark"), QStringLiteral("Minimalist"),
        QStringLiteral("Relaxing")};
    for (const WorkshopTag& tag : workshopTags()) {
        if (!visible.contains(tag.value)) continue;
        auto* button = new QToolButton(content);
        button->setText(tag.displayName);
        button->setIcon(QIcon::fromTheme(tag.iconName));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setCheckable(true);
        button->setProperty("tagChip", true);
        m_buttons.insert(tag.value, button);
        row->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, value = tag.value] {
            m_viewModel->toggleTag(value);
        });
    }
    m_clear = new QToolButton(content);
    m_clear->setText(QStringLiteral("清除"));
    m_clear->setIcon(QIcon::fromTheme(QStringLiteral("edit-clear")));
    m_clear->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_clear->setProperty("dangerChip", true);
    row->addWidget(m_clear);
    row->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    scroll->setFixedHeight(38);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);

    connect(m_clear, &QToolButton::clicked, m_viewModel, &WorkshopViewModel::clearTags);
    connect(m_viewModel, &WorkshopViewModel::filtersChanged, this, &WorkshopTagBar::syncFromViewModel);
    syncFromViewModel();
}

void WorkshopTagBar::syncFromViewModel() {
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) {
        it.value()->setChecked(m_viewModel->selectedTags().contains(it.key()));
    }
    m_clear->setVisible(!m_viewModel->selectedTags().isEmpty());
}

} // namespace Mirage

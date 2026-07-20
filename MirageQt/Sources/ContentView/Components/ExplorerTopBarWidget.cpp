#include "ContentView/Components/ExplorerTopBarWidget.h"

#include <QHBoxLayout>
#include <QPushButton>

namespace Mirage {

ExplorerTopBarWidget::ExplorerTopBarWidget(QWidget* parent)
    : QWidget(parent) {
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索"));
    m_search->setFixedWidth(160);

    m_filter = new QPushButton(QIcon::fromTheme("view-filter"), QStringLiteral("筛选"), this);
    m_filter->setCheckable(true);
    m_filter->setChecked(false);
    m_filter->setProperty("accent", true);
    auto* refresh = new QToolButton(this);
    refresh->setIcon(QIcon::fromTheme("view-refresh"));
    refresh->setToolTip(QStringLiteral("刷新"));
    refresh->setProperty("flatButton", true);
    refresh->setFixedWidth(34);

    m_direction = new QToolButton(this);
    m_direction->setIcon(QIcon::fromTheme("go-up"));
    m_direction->setToolTip(QStringLiteral("切换排序方向"));
    m_direction->setProperty("flatButton", true);
    m_direction->setFixedWidth(34);

    m_sort = new QComboBox(this);
    m_sort->addItems({QStringLiteral("名称"), QStringLiteral("评分"), QStringLiteral("文件大小")});
    m_sort->setFixedWidth(120);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(m_search);
    layout->addWidget(m_filter);
    layout->addWidget(refresh);
    layout->addStretch(1);
    layout->addWidget(m_direction);
    layout->addWidget(m_sort);

    connect(m_search, &QLineEdit::textChanged, this, &ExplorerTopBarWidget::searchChanged);
    connect(m_filter, &QPushButton::toggled, this, &ExplorerTopBarWidget::filterToggled);
    connect(refresh, &QToolButton::clicked, this, &ExplorerTopBarWidget::refreshRequested);
    connect(m_sort, &QComboBox::currentIndexChanged, this, &ExplorerTopBarWidget::sortChanged);
    connect(m_direction, &QToolButton::clicked, this, [this] {
        m_descending = !m_descending;
        m_direction->setIcon(QIcon::fromTheme(m_descending ? "go-up" : "go-down"));
        emit sortChanged();
    });
}

QString ExplorerTopBarWidget::searchText() const {
    return m_search->text();
}

QString ExplorerTopBarWidget::sortText() const {
    return m_sort->currentText();
}

bool ExplorerTopBarWidget::descending() const {
    return m_descending;
}

void ExplorerTopBarWidget::setFilterVisible(bool visible) {
    m_filter->setChecked(visible);
}

} // namespace Mirage

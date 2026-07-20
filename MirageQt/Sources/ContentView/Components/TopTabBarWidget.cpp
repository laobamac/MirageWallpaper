#include "ContentView/Components/TopTabBarWidget.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace Mirage {
namespace {

QToolButton* tabButton(const QIcon& icon, const QString& text, int id, QButtonGroup* group, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setIcon(icon);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setCheckable(true);
    button->setAutoRaise(false);
    button->setProperty("tabButton", true);
    button->setMinimumHeight(32);
    group->addButton(button, id);
    return button;
}

QToolButton* plainButton(const QIcon& icon, const QString& text, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setIcon(icon);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setProperty("flatButton", true);
    button->setMinimumHeight(32);
    return button;
}

QFrame* divider(QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setFrameShape(QFrame::VLine);
    frame->setFixedHeight(24);
    frame->setStyleSheet(QStringLiteral("color: #625d56;"));
    return frame;
}

} // namespace

TopTabBarWidget::TopTabBarWidget(QWidget* parent)
    : QWidget(parent) {
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    auto* installed = tabButton(QIcon::fromTheme("folder-download"), QStringLiteral("已安装"), 0, m_group, this);
    auto* discover = tabButton(QIcon::fromTheme("edit-find"), QStringLiteral("发现"), 1, m_group, this);
    auto* workshop = tabButton(QIcon::fromTheme("folder-cloud"), QStringLiteral("创意工坊"), 2, m_group, this);

    auto* mobile = plainButton(QIcon::fromTheme("smartphone"), QStringLiteral("移动端"), this);
    auto* display = plainButton(QIcon::fromTheme("video-display"), QStringLiteral("显示器"), this);
    auto* settings = plainButton(QIcon::fromTheme("settings-configure"), QStringLiteral("设置"), this);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(installed);
    row->addWidget(discover);
    row->addWidget(workshop);
    row->addSpacing(10);
    row->addStretch(1);
    row->addWidget(divider(this));
    row->addWidget(mobile);
    row->addWidget(divider(this));
    row->addWidget(display);
    row->addWidget(divider(this));
    row->addWidget(settings);
    row->addWidget(divider(this));

    auto* accent = new QFrame(this);
    accent->setFixedHeight(4);
    accent->setStyleSheet(QStringLiteral("background: #0a84ff; border: 0;"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(row);
    layout->addWidget(accent);

    connect(m_group, &QButtonGroup::idClicked, this, &TopTabBarWidget::setCurrentIndex);
    connect(mobile, &QToolButton::clicked, this, &TopTabBarWidget::mobileRequested);
    connect(display, &QToolButton::clicked, this, &TopTabBarWidget::displaySettingsRequested);
    connect(settings, &QToolButton::clicked, this, &TopTabBarWidget::settingsRequested);
    setCurrentIndex(0);
}

int TopTabBarWidget::currentIndex() const {
    return m_group->checkedId();
}

void TopTabBarWidget::setCurrentIndex(int index) {
    if (auto* button = m_group->button(index)) button->setChecked(true);
    emit tabChanged(index);
}

void TopTabBarWidget::setActiveDownloadCount(int count) {
    Q_UNUSED(count)
}

} // namespace Mirage

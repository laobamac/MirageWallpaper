#include "SettingsView/SettingsView.h"

#include "SettingsView/AboutUsView.h"
#include "SettingsView/GeneralPage.h"
#include "SettingsView/PerformancePage.h"
#include "SettingsView/PluginsPage.h"
#include "SettingsView/ScreenSaverPage.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace Mirage {
namespace {

QToolButton* toolbarButton(const QString& iconName,
                           const QString& text,
                           int index,
                           QButtonGroup* group,
                           QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setIcon(QIcon::fromTheme(iconName));
    button->setIconSize(QSize(24, 24));
    button->setText(text);
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setCheckable(true);
    button->setProperty("settingsToolbar", true);
    button->setFixedSize(72, 58);
    group->addButton(button, index);
    return button;
}

QScrollArea* scrollPage(QWidget* page, QWidget* parent) {
    auto* scroll = new QScrollArea(parent);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    return scroll;
}

} // namespace

SettingsView::SettingsView(GlobalSettingsService* settings, QWidget* parent)
    : QWidget(parent)
    , m_settings(settings)
    , m_original(settings->settings())
    , m_draft(m_original) {
    setMinimumWidth(500);

    m_toolbar = new QButtonGroup(this);
    m_toolbar->setExclusive(true);
    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(16, 6, 16, 6);
    toolbar->setSpacing(6);
    toolbar->addStretch(1);
    toolbar->addWidget(toolbarButton(QStringLiteral("speedometer"), QStringLiteral("性能"), 0, m_toolbar, this));
    toolbar->addWidget(toolbarButton(QStringLiteral("settings-configure"), QStringLiteral("通用"), 1, m_toolbar, this));
    toolbar->addWidget(toolbarButton(QStringLiteral("applications-system"), QStringLiteral("插件"), 2, m_toolbar, this));
    toolbar->addWidget(toolbarButton(QStringLiteral("preferences-desktop-screensaver"), QStringLiteral("屏保"), 3, m_toolbar, this));
    toolbar->addWidget(toolbarButton(QStringLiteral("help-about"), QStringLiteral("关于"), 4, m_toolbar, this));
    toolbar->addStretch(1);

    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    m_stack = new QStackedWidget(this);
    m_performance = new PerformancePage(&m_draft, m_stack);
    m_general = new GeneralPage(&m_draft, m_stack);
    m_stack->addWidget(scrollPage(m_performance, m_stack));
    m_stack->addWidget(scrollPage(m_general, m_stack));
    m_stack->addWidget(scrollPage(new PluginsPage(m_stack), m_stack));
    m_stack->addWidget(scrollPage(new ScreenSaverPage(m_stack), m_stack));
    m_stack->addWidget(scrollPage(new AboutUsView(m_stack), m_stack));
    m_stack->setMinimumHeight(400);

    auto* bottomSeparator = new QFrame(this);
    bottomSeparator->setFrameShape(QFrame::HLine);
    m_modified = new QLabel(QStringLiteral("⚠  已修改"), this);
    m_modified->setProperty("warning", true);
    auto* confirm = new QPushButton(QStringLiteral("确认"), this);
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    confirm->setProperty("accent", true);
    confirm->setFixedWidth(70);
    cancel->setFixedWidth(70);
    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(16, 16, 16, 16);
    actions->setSpacing(8);
    actions->addWidget(m_modified);
    actions->addStretch(1);
    actions->addWidget(confirm);
    actions->addWidget(cancel);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(toolbar);
    layout->addWidget(separator);
    layout->addWidget(m_stack, 1);
    layout->addWidget(bottomSeparator);
    layout->addLayout(actions);

    connect(m_toolbar, &QButtonGroup::idClicked, this, &SettingsView::selectPage);
    connect(m_performance, &PerformancePage::changed, this, &SettingsView::updateModifiedState);
    connect(m_general, &GeneralPage::changed, this, &SettingsView::updateModifiedState);
    connect(m_general, &GeneralPage::resetRequested, this, &SettingsView::resetDraft);
    connect(confirm, &QPushButton::clicked, this, &SettingsView::commit);
    connect(cancel, &QPushButton::clicked, this, &SettingsView::cancelled);

    QSettings uiState;
    selectPage(uiState.value(QStringLiteral("Settings/SelectedPage"), 0).toInt());
    updateModifiedState();
}

void SettingsView::selectPage(int index) {
    index = qBound(0, index, m_stack->count() - 1);
    m_stack->setCurrentIndex(index);
    if (QAbstractButton* button = m_toolbar->button(index)) button->setChecked(true);
    QSettings().setValue(QStringLiteral("Settings/SelectedPage"), index);
}

void SettingsView::updateModifiedState() {
    m_modified->setVisible(m_draft != m_original);
}

void SettingsView::resetDraft() {
    m_draft = GlobalSettings();
    m_performance->load();
    m_general->load();
    updateModifiedState();
}

void SettingsView::commit() {
    QString error;
    if (!m_settings->setSettings(m_draft, &error)) {
        QMessageBox::critical(this,
                              QStringLiteral("无法保存设置"),
                              error.isEmpty() ? QStringLiteral("设置文件写入失败。") : error);
        return;
    }
    m_original = m_settings->settings();
    m_draft = m_original;
    updateModifiedState();
    emit accepted();
}

} // namespace Mirage

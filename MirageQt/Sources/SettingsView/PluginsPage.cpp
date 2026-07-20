#include "SettingsView/PluginsPage.h"

#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace Mirage {

PluginsPage::PluginsPage(QWidget* parent)
    : QWidget(parent) {
    auto* builtIn = new QGroupBox(QStringLiteral("内置"), this);
    auto* builtInLayout = new QVBoxLayout(builtIn);
    auto* builtInText = new QLabel(QStringLiteral("暂无内置插件。"), builtIn);
    builtInText->setProperty("secondary", true);
    builtInLayout->addWidget(builtInText);

    auto* thirdParty = new QGroupBox(QStringLiteral("第三方"), this);
    auto* thirdPartyLayout = new QVBoxLayout(thirdParty);
    auto* thirdPartyText = new QLabel(QStringLiteral("无"), thirdParty);
    thirdPartyText->setProperty("secondary", true);
    thirdPartyLayout->addWidget(thirdPartyText);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(14);
    layout->addWidget(builtIn);
    layout->addWidget(thirdParty);
    layout->addStretch(1);
}

} // namespace Mirage

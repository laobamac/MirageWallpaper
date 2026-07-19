#include "SettingsView/AboutUsView.h"

#include "ContentView/Components/ProjectFeedbackBannerWidget.h"
#include "Services/Paths.h"

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

namespace Mirage {

AboutUsView::AboutUsView(QWidget* parent)
    : QWidget(parent) {
    auto* icon = new QLabel(this);
    icon->setFixedSize(88, 88);
    icon->setAlignment(Qt::AlignCenter);
    const QString iconPath = Paths::repoRoot() + QStringLiteral("/Mirage/Mirage Wallpaper/Resources/Assets.xcassets/AppIcon.appiconset/icon_128.png");
    QPixmap appIcon(iconPath);
    if (!appIcon.isNull()) icon->setPixmap(appIcon.scaled(88, 88, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else icon->setPixmap(QIcon::fromTheme(QStringLiteral("preferences-desktop-wallpaper")).pixmap(72, 72));

    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedHeight(90);
    auto* name = new QLabel(QStringLiteral("Mirage"), this);
    QFont nameFont = name->font();
    nameFont.setPointSize(25);
    nameFont.setBold(true);
    name->setFont(nameFont);
    auto* description = new QLabel(QStringLiteral("Linux 动态壁纸引擎"), this);
    description->setProperty("secondary", true);
    auto* formats = new QLabel(QStringLiteral("场景 · 网页 · 视频"), this);
    formats->setProperty("tertiary", true);
    auto* identity = new QVBoxLayout;
    identity->setSpacing(6);
    identity->addWidget(name);
    identity->addWidget(description);
    identity->addWidget(formats);
    auto* hero = new QHBoxLayout;
    hero->setSpacing(20);
    hero->addStretch(1);
    hero->addWidget(icon);
    hero->addWidget(divider);
    hero->addLayout(identity);
    hero->addStretch(1);

    auto* version = new QLabel(QStringLiteral("版本 %1").arg(QCoreApplication::applicationVersion()), this);
    version->setAlignment(Qt::AlignCenter);
    version->setProperty("secondary", true);
    auto* author = new QLabel(QStringLiteral("作者  <b>王孝慈 (laobamac)</b>"), this);
    author->setAlignment(Qt::AlignCenter);
    auto* link = new QLabel(QStringLiteral("<a href='https://github.com/laobamac/MirageWallpaper'>github.com/laobamac/MirageWallpaper</a>"), this);
    link->setAlignment(Qt::AlignCenter);
    link->setOpenExternalLinks(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 30, 28, 24);
    layout->setSpacing(14);
    layout->addLayout(hero);
    layout->addSpacing(8);
    layout->addWidget(version);
    layout->addWidget(author);
    layout->addWidget(link);
    layout->addStretch(1);
    layout->addWidget(new ProjectFeedbackBannerWidget(this, false));
}

} // namespace Mirage

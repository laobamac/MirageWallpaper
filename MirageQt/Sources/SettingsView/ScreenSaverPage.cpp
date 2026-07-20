#include "SettingsView/ScreenSaverPage.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace Mirage {

ScreenSaverPage::ScreenSaverPage(QWidget* parent)
    : QWidget(parent) {
    auto* component = new QGroupBox(QStringLiteral("屏保组件"), this);
    auto* componentLayout = new QVBoxLayout(component);
    auto* statusRow = new QHBoxLayout;
    auto* status = new QLabel(QStringLiteral("◇  Mirage 动态屏保尚未安装"), component);
    status->setProperty("secondary", true);
    auto* install = new QPushButton(QStringLiteral("安装"), component);
    install->setEnabled(false);
    statusRow->addWidget(status);
    statusRow->addStretch(1);
    statusRow->addWidget(install);
    componentLayout->addLayout(statusRow);
    auto* systemSettings = new QPushButton(QStringLiteral("打开系统屏保设置"), component);
    systemSettings->setEnabled(false);
    componentLayout->addWidget(systemSettings, 0, Qt::AlignLeft);
    auto* componentHint = new QLabel(
        QStringLiteral("当前 Linux Qt 构建尚未提供独立的动态屏保宿主。此页面保留与 macOS 相同的信息结构，功能可用后将在这里安装。"),
        component);
    componentHint->setWordWrap(true);
    componentHint->setProperty("secondary", true);
    componentLayout->addWidget(componentHint);

    auto* wallpaper = new QGroupBox(QStringLiteral("屏保壁纸"), this);
    auto* wallpaperLayout = new QVBoxLayout(wallpaper);
    wallpaperLayout->addWidget(new QLabel(QStringLiteral("当前屏保壁纸                                  尚未选择"), wallpaper));
    wallpaperLayout->addWidget(new QLabel(QStringLiteral("正在播放                                      无"), wallpaper));
    auto* useCurrent = new QPushButton(QStringLiteral("将正在播放的壁纸设为屏保"), wallpaper);
    useCurrent->setEnabled(false);
    wallpaperLayout->addWidget(useCurrent, 0, Qt::AlignLeft);
    auto* wallpaperHint = new QLabel(
        QStringLiteral("屏保会始终静音，并保留当前预设、自定义属性、填充方式和最高 60 FPS 的帧率设置。"),
        wallpaper);
    wallpaperHint->setWordWrap(true);
    wallpaperHint->setProperty("secondary", true);
    wallpaperLayout->addWidget(wallpaperHint);

    auto* runtime = new QGroupBox(QStringLiteral("运行方式"), this);
    auto* runtimeLayout = new QVBoxLayout(runtime);
    auto* runtimeText = new QLabel(
        QStringLiteral("视频、网页和场景壁纸将由 Mirage 的独立屏保宿主加载，不要求主程序保持运行。网页屏保不会获得网络导航权限，音频响应保持静音。"),
        runtime);
    runtimeText->setWordWrap(true);
    runtimeText->setProperty("secondary", true);
    runtimeLayout->addWidget(runtimeText);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(14);
    layout->addWidget(component);
    layout->addWidget(wallpaper);
    layout->addWidget(runtime);
    layout->addStretch(1);
}

} // namespace Mirage

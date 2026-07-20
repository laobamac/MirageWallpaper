#include "ContentView/FirstLaunchView.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>

namespace Mirage {

FirstLaunchView::FirstLaunchView(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("欢迎使用 MirageQt"));
    auto* label = new QLabel(QStringLiteral("MirageQt 会扫描本机 Wallpaper Engine 创意工坊目录、MirageQt 导入目录和 SteamCMD 下载目录。X11 支持动态桌面壁纸；Wayland 会保留主界面和预览，但不能应用动态桌面壁纸。"), this);
    label->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(label);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
}

} // namespace Mirage

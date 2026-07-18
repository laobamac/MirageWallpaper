#include "ContentView/Components/ExplorerBottomBarWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace Mirage {

ExplorerBottomBarWidget::ExplorerBottomBarWidget(QWidget* parent)
    : QWidget(parent) {
    auto* playlist = new QLabel(QStringLiteral("播放列表"), this);
    QFont font = playlist->font();
    font.setPointSize(font.pointSize() + 8);
    playlist->setFont(font);

    auto* load = new QPushButton(QIcon::fromTheme("document-open"), QStringLiteral("载入"), this);
    auto* save = new QPushButton(QIcon::fromTheme("document-save"), QStringLiteral("保存"), this);
    auto* configure = new QPushButton(QIcon::fromTheme("settings-configure"), QStringLiteral("配置"), this);
    auto* add = new QPushButton(QIcon::fromTheme("list-add"), QStringLiteral("添加壁纸"), this);
    load->setEnabled(false);
    save->setEnabled(false);
    configure->setEnabled(false);
    add->setEnabled(false);

    auto* top = new QHBoxLayout;
    top->addWidget(playlist);
    top->addWidget(load);
    top->addWidget(save);
    top->addWidget(configure);
    top->addWidget(add);
    top->addStretch(1);

    auto* import = new QPushButton(QIcon::fromTheme("document-import"), QStringLiteral("导入壁纸"), this);
    import->setFixedWidth(220);

    auto* bottom = new QHBoxLayout;
    bottom->addWidget(import);
    bottom->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(top);
    layout->addLayout(bottom);

    connect(import, &QPushButton::clicked, this, &ExplorerBottomBarWidget::importRequested);
}

} // namespace Mirage

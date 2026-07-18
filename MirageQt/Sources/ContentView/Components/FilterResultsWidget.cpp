#include "ContentView/Components/FilterResultsWidget.h"

#include <QGroupBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace Mirage {

FilterResultsWidget::FilterResultsWidget(QWidget* parent)
    : QWidget(parent) {
    auto* content = new QWidget(this);
    auto* list = new QVBoxLayout(content);

    auto* reset = new QPushButton(QIcon::fromTheme("edit-undo"), QStringLiteral("重置筛选"), content);
    list->addWidget(reset);

    auto addGroup = [&](const QString& title, const QStringList& values) {
        auto* box = new QGroupBox(title, content);
        auto* layout = new QVBoxLayout(box);
        for (const QString& value : values) layout->addWidget(addCheck(value, box));
        list->addWidget(box);
    };

    addGroup(QStringLiteral("仅显示："), {QStringLiteral("广受好评"), QStringLiteral("我的收藏"),
                                           QStringLiteral("已安装"), QStringLiteral("未安装")});
    addGroup(QStringLiteral("类型"), {QStringLiteral("场景"), QStringLiteral("网页"), QStringLiteral("视频")});
    addGroup(QStringLiteral("分级"), {QStringLiteral("Everyone"), QStringLiteral("Questionable"), QStringLiteral("Mature")});
    addGroup(QStringLiteral("来源"), {QStringLiteral("创意工坊"), QStringLiteral("我的壁纸")});
    addGroup(QStringLiteral("标签"), {QStringLiteral("Anime"), QStringLiteral("Nature"), QStringLiteral("Abstract"),
                                      QStringLiteral("Landscape"), QStringLiteral("Sci-Fi"), QStringLiteral("Game"),
                                      QStringLiteral("Music"), QStringLiteral("Technology")});
    list->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 0);
    layout->addWidget(scroll);
    setFixedWidth(225);

    connect(reset, &QPushButton::clicked, this, [this] {
        const auto checks = findChildren<QCheckBox*>();
        for (QCheckBox* box : checks) box->setChecked(false);
        emit filtersChanged();
    });
}

QCheckBox* FilterResultsWidget::addCheck(const QString& text, QWidget* parent) {
    auto* check = new QCheckBox(text, parent);
    connect(check, &QCheckBox::toggled, this, &FilterResultsWidget::filtersChanged);
    return check;
}

} // namespace Mirage

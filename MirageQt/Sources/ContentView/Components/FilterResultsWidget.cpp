#include "ContentView/Components/FilterResultsWidget.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace Mirage {
namespace {

QPixmap tintedIcon(const QString& name, const QColor& color) {
    QPixmap pixmap = QIcon::fromTheme(name).pixmap(17, 17);
    if (pixmap.isNull()) return pixmap;
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), color);
    return pixmap;
}

QPushButton* linkButton(const QString& text, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { min-height: 20px; padding: 0; color: #2997ff; background: transparent; border: 0; }"
        "QPushButton:hover { color: #69b7ff; text-decoration: underline; }"));
    return button;
}

} // namespace

FilterResultsWidget::FilterResultsWidget(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("filterSidebar"));
    setFixedWidth(225);
    setStyleSheet(QStringLiteral("QWidget#filterSidebar { border-right: 1px solid #5b5650; }"));

    auto* content = new QWidget(this);
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 0, 12, 16);
    list->setSpacing(0);

    auto* reset = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("重置筛选"), content);
    reset->setProperty("accent", true);
    reset->setMinimumHeight(30);
    list->addWidget(reset);
    list->addSpacing(24);

    auto* only = new QGroupBox(QStringLiteral("仅显示："), content);
    only->setFixedWidth(145);
    auto* onlyLayout = new QVBoxLayout(only);
    onlyLayout->setContentsMargins(14, 15, 10, 10);
    onlyLayout->setSpacing(7);

    const auto addOnly = [&](const QString& iconName,
                             const QColor& iconColor,
                             const QString& text,
                             const QString& value) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        auto* icon = new QLabel(only);
        icon->setFixedSize(18, 18);
        icon->setAlignment(Qt::AlignCenter);
        const QPixmap pixmap = tintedIcon(iconName, iconColor);
        if (!pixmap.isNull()) icon->setPixmap(pixmap);
        auto* check = addCheck(text, value, false, m_showOnly, only);
        row->addWidget(icon);
        row->addWidget(check, 1);
        onlyLayout->addLayout(row);
    };

    addOnly(QStringLiteral("rating"), QColor(QStringLiteral("#35d06f")), QStringLiteral("广受好评"), QStringLiteral("approved"));
    addOnly(QStringLiteral("emblem-favorite"), QColor(QStringLiteral("#ff416c")), QStringLiteral("我的收藏"), QStringLiteral("favorite"));
    addOnly(QStringLiteral("smartphone"), QColor(QStringLiteral("#ff9f2f")), QStringLiteral("移动端兼容"), QStringLiteral("mobile"));
    addOnly(QStringLiteral("audio-volume-high"), QColor(QStringLiteral("#2997ff")), QStringLiteral("音频响应"), QStringLiteral("audio"));
    addOnly(QStringLiteral("settings-configure"), QColor(QStringLiteral("#2997ff")), QStringLiteral("可自定义"), QStringLiteral("customizable"));
    auto* onlyRow = new QHBoxLayout;
    onlyRow->setContentsMargins(0, 0, 0, 0);
    onlyRow->addStretch(1);
    onlyRow->addWidget(only);
    onlyRow->addStretch(1);
    list->addLayout(onlyRow);
    list->addSpacing(22);

    const auto addSection = [&](const QString& title,
                                const QList<QPair<QString, QString>>& options,
                                QVector<QCheckBox*>& destination,
                                bool withTagLinks = false) {
        auto* section = new QWidget(content);
        auto* sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(2);

        auto* header = new QToolButton(section);
        header->setText(title);
        header->setArrowType(Qt::DownArrow);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setCheckable(true);
        header->setChecked(true);
        header->setProperty("flatButton", true);
        header->setStyleSheet(QStringLiteral(
            "QToolButton { min-height: 28px; padding: 2px 0; border: 0; background: transparent;"
            " text-align: left; font-size: 15px; font-weight: 600; }"
            "QToolButton:hover { color: white; background: transparent; }"));

        auto* body = new QWidget(section);
        auto* bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(20, 3, 0, 0);
        bodyLayout->setSpacing(6);

        if (withTagLinks) {
            auto* destinationPtr = &destination;
            auto* links = new QHBoxLayout;
            links->setContentsMargins(0, 0, 0, 2);
            links->setSpacing(12);
            auto* selectAll = linkButton(QStringLiteral("全选"), body);
            auto* clearAll = linkButton(QStringLiteral("清空"), body);
            links->addWidget(selectAll);
            links->addWidget(clearAll);
            links->addStretch(1);
            bodyLayout->addLayout(links);
            connect(selectAll, &QPushButton::clicked, this, [destinationPtr] {
                for (QCheckBox* check : *destinationPtr) check->setChecked(true);
            });
            connect(clearAll, &QPushButton::clicked, this, [destinationPtr] {
                for (QCheckBox* check : *destinationPtr) check->setChecked(false);
            });
        }

        for (const auto& option : options) {
            bodyLayout->addWidget(addCheck(option.first, option.second, true, destination, body));
        }

        sectionLayout->addWidget(header);
        sectionLayout->addWidget(body);
        connect(header, &QToolButton::toggled, body, &QWidget::setVisible);
        connect(header, &QToolButton::toggled, header, [header](bool expanded) {
            header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        });
        list->addWidget(section);
        list->addSpacing(16);
    };

    addSection(QStringLiteral("类型"),
               {{QStringLiteral("场景"), QStringLiteral("scene")},
                {QStringLiteral("视频"), QStringLiteral("video")},
                {QStringLiteral("网页"), QStringLiteral("web")},
                {QStringLiteral("应用程序"), QStringLiteral("application")},
                {QStringLiteral("预设"), QStringLiteral("preset")}},
               m_types);
    addSection(QStringLiteral("分级"),
               {{QStringLiteral("所有人"), QStringLiteral("Everyone")},
                {QStringLiteral("轻度裸露"), QStringLiteral("Questionable")},
                {QStringLiteral("成人"), QStringLiteral("Mature")}},
               m_ratings);
    addSection(QStringLiteral("来源"),
               {{QStringLiteral("创意工坊"), QStringLiteral("workshop")},
                {QStringLiteral("我的壁纸"), QStringLiteral("imported")}},
               m_sources);
    addSection(QStringLiteral("标签"),
               {{QStringLiteral("抽象"), QStringLiteral("abstract")},
                {QStringLiteral("动物"), QStringLiteral("animal")},
                {QStringLiteral("动漫"), QStringLiteral("anime")},
                {QStringLiteral("卡通"), QStringLiteral("cartoon")},
                {QStringLiteral("CGI"), QStringLiteral("cgi")},
                {QStringLiteral("赛博朋克"), QStringLiteral("cyberpunk")},
                {QStringLiteral("奇幻"), QStringLiteral("fantasy")},
                {QStringLiteral("游戏"), QStringLiteral("game")},
                {QStringLiteral("女孩"), QStringLiteral("girls")},
                {QStringLiteral("男孩"), QStringLiteral("guys")},
                {QStringLiteral("风景"), QStringLiteral("landscape")},
                {QStringLiteral("中世纪"), QStringLiteral("medieval")},
                {QStringLiteral("表情包"), QStringLiteral("memes")},
                {QStringLiteral("MMD"), QStringLiteral("mmd")},
                {QStringLiteral("音乐"), QStringLiteral("music")},
                {QStringLiteral("自然"), QStringLiteral("nature")},
                {QStringLiteral("像素艺术"), QStringLiteral("pixelart")},
                {QStringLiteral("治愈"), QStringLiteral("relaxing")},
                {QStringLiteral("复古"), QStringLiteral("retro")},
                {QStringLiteral("科幻"), QStringLiteral("scifi")},
                {QStringLiteral("运动"), QStringLiteral("sports")},
                {QStringLiteral("科技"), QStringLiteral("technology")},
                {QStringLiteral("影视"), QStringLiteral("television")},
                {QStringLiteral("载具"), QStringLiteral("vehicle")},
                {QStringLiteral("未分类"), QStringLiteral("unspecified")}},
               m_tags,
               true);
    list->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 0);
    layout->addWidget(scroll);

    connect(reset, &QPushButton::clicked, this, [this] {
        const QList<QCheckBox*> checks = findChildren<QCheckBox*>();
        for (QCheckBox* check : checks) check->blockSignals(true);
        for (QCheckBox* check : m_showOnly) check->setChecked(false);
        for (QCheckBox* check : m_types) check->setChecked(true);
        for (QCheckBox* check : m_ratings) check->setChecked(true);
        for (QCheckBox* check : m_sources) check->setChecked(true);
        for (QCheckBox* check : m_tags) check->setChecked(true);
        for (QCheckBox* check : checks) check->blockSignals(false);
        emit filtersChanged();
    });
}

WallpaperFilterState FilterResultsWidget::filterState() const {
    WallpaperFilterState state;
    state.approvedOnly = m_showOnly.value(0) && m_showOnly.at(0)->isChecked();
    state.favoritesOnly = m_showOnly.value(1) && m_showOnly.at(1)->isChecked();
    state.mobileOnly = m_showOnly.value(2) && m_showOnly.at(2)->isChecked();
    state.audioOnly = m_showOnly.value(3) && m_showOnly.at(3)->isChecked();
    state.customizableOnly = m_showOnly.value(4) && m_showOnly.at(4)->isChecked();

    const auto collect = [](const QVector<QCheckBox*>& checks) {
        QSet<QString> values;
        for (const QCheckBox* check : checks) {
            if (check->isChecked()) values.insert(check->property("filterValue").toString());
        }
        return values;
    };
    state.types = collect(m_types);
    state.ratings = collect(m_ratings);
    state.sources = collect(m_sources);
    state.tags = collect(m_tags);
    return state;
}

QCheckBox* FilterResultsWidget::addCheck(const QString& text,
                                         const QString& value,
                                         bool checked,
                                         QVector<QCheckBox*>& destination,
                                         QWidget* parent) {
    auto* check = new QCheckBox(text, parent);
    check->setProperty("filterValue", value);
    check->setChecked(checked);
    destination.push_back(check);
    connect(check, &QCheckBox::toggled, this, &FilterResultsWidget::filtersChanged);
    return check;
}

} // namespace Mirage

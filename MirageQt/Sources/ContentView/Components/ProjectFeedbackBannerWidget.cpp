#include "ContentView/Components/ProjectFeedbackBannerWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace Mirage {

ProjectFeedbackBannerWidget::ProjectFeedbackBannerWidget(QWidget* parent, bool showsActions)
    : QWidget(parent) {
    setObjectName(QStringLiteral("projectFeedbackBanner"));
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(52);
    setStyleSheet(QStringLiteral(
        "QWidget#projectFeedbackBanner {"
        " background-color: #584432;"
        " border: 1px solid #a66524;"
        " border-radius: 8px;"
        "}"
        "QLabel#feedbackTitle { font-weight: 700; font-size: 15px; }"
        "QLabel#feedbackDetail { color: #b9b3ac; font-size: 12px; }"
        "QLabel#feedbackIcon { color: #3d3832; background-color: #ff9f2f;"
        " border-radius: 5px; font-size: 16px; font-weight: 800; }"));

    auto* icon = new QLabel(QStringLiteral("!"), this);
    icon->setObjectName(QStringLiteral("feedbackIcon"));
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(22, 22);

    auto* title = new QLabel(QStringLiteral("Mirage 仍处于早期阶段"), this);
    title->setObjectName(QStringLiteral("feedbackTitle"));
    auto* detail = new QLabel(QStringLiteral("遇到问题请认真撰写 Issue，或加入 QQ 交流群 2160040437 反馈。"), this);
    detail->setObjectName(QStringLiteral("feedbackDetail"));

    auto* text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(2);
    text->addWidget(title);
    text->addWidget(detail);

    auto* issue = new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), QStringLiteral("提交 Issue"), this);
    m_copyButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("复制群号"), this);
    m_copyButton->setProperty("accent", true);
    issue->setVisible(showsActions);
    m_copyButton->setVisible(showsActions);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 7, 12, 7);
    layout->setSpacing(12);
    layout->addWidget(icon);
    layout->addLayout(text);
    layout->addStretch(1);
    layout->addWidget(issue);
    layout->addWidget(m_copyButton);

    connect(issue, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/laobamac/MirageWallpaper/issues/new/choose")));
    });
    connect(m_copyButton, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(QStringLiteral("2160040437"));
        m_copyButton->setText(QStringLiteral("群号已复制"));
        QTimer::singleShot(1800, this, [this] { m_copyButton->setText(QStringLiteral("复制群号")); });
    });
}

} // namespace Mirage

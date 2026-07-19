#include "ContentView/Components/Workshop/WorkshopItemDetail.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace Mirage {
namespace {

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
}

QLabel* centeredStat(const QString& icon, const QString& value, const QString& label, QWidget* parent) {
    auto* stat = new QLabel(QStringLiteral("%1\n<b>%2</b>\n%3").arg(icon, value, label), parent);
    stat->setAlignment(Qt::AlignCenter);
    stat->setTextFormat(Qt::RichText);
    stat->setProperty("secondary", true);
    return stat;
}

} // namespace

WorkshopItemDetail::WorkshopItemDetail(WorkshopViewModel* viewModel,
                                       SteamWebAPI* api,
                                       QWidget* parent)
    : QWidget(parent)
    , m_viewModel(viewModel)
    , m_api(api) {
    m_stack = new QStackedWidget(this);

    auto* empty = new QWidget(m_stack);
    auto* emptyText = new QLabel(QStringLiteral("▥\n\n点击壁纸查看详情"), empty);
    emptyText->setAlignment(Qt::AlignCenter);
    emptyText->setProperty("secondary", true);
    QFont emptyFont = emptyText->font();
    emptyFont.setPixelSize(16);
    emptyText->setFont(emptyFont);
    auto* emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch(1);
    emptyLayout->addWidget(emptyText);
    emptyLayout->addStretch(1);

    auto* page = new QWidget(m_stack);
    auto* scroll = new QScrollArea(page);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(16, 16, 16, 12);
    contentLayout->setSpacing(13);

    m_preview = new QLabel(content);
    m_preview->setFixedSize(280, 158);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setObjectName(QStringLiteral("workshopDetailPreview"));
    m_preview->setPixmap(QIcon::fromTheme(QStringLiteral("image-x-generic")).pixmap(52, 52));
    auto* previewRow = new QHBoxLayout;
    previewRow->addStretch(1);
    previewRow->addWidget(m_preview);
    previewRow->addStretch(1);
    contentLayout->addLayout(previewRow);

    m_title = new QLabel(content);
    m_title->setWordWrap(true);
    m_title->setAlignment(Qt::AlignCenter);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPixelSize(16);
    m_title->setFont(titleFont);
    contentLayout->addWidget(m_title);

    m_presetNotice = new QLabel(QStringLiteral("☷  创意工坊预设：需要对应的基础壁纸"), content);
    m_presetNotice->setWordWrap(true);
    m_presetNotice->setAlignment(Qt::AlignCenter);
    m_presetNotice->setObjectName(QStringLiteral("presetNotice"));
    contentLayout->addWidget(m_presetNotice);

    auto* stats = new QHBoxLayout;
    m_subscriptions = centeredStat(QStringLiteral("↓"), QStringLiteral("0"), QStringLiteral("订阅"), content);
    m_favorites = centeredStat(QStringLiteral("♥"), QStringLiteral("0"), QStringLiteral("收藏"), content);
    m_views = centeredStat(QStringLiteral("◉"), QStringLiteral("0"), QStringLiteral("浏览"), content);
    stats->addWidget(m_subscriptions, 1);
    stats->addWidget(m_favorites, 1);
    stats->addWidget(m_views, 1);
    contentLayout->addLayout(stats);

    m_meta = new QLabel(content);
    m_meta->setAlignment(Qt::AlignCenter);
    m_meta->setProperty("secondary", true);
    contentLayout->addWidget(m_meta);

    contentLayout->addWidget(sectionHeader(QStringLiteral("标签"), content));
    m_tags = new QLabel(content);
    m_tags->setWordWrap(true);
    m_tags->setProperty("secondary", true);
    contentLayout->addWidget(m_tags);

    contentLayout->addWidget(sectionHeader(QStringLiteral("描述"), content));
    m_description = new QLabel(content);
    m_description->setWordWrap(true);
    m_description->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_description->setProperty("secondary", true);
    contentLayout->addWidget(m_description);

    contentLayout->addWidget(sectionHeader(QStringLiteral("操作"), content));
    m_downloadSection = new QWidget(content);
    m_downloadLayout = new QVBoxLayout(m_downloadSection);
    m_downloadLayout->setContentsMargins(0, 0, 0, 0);
    m_downloadLayout->setSpacing(6);
    contentLayout->addWidget(m_downloadSection);

    auto* openSteam = new QPushButton(QIcon::fromTheme(QStringLiteral("internet-web-browser")), QStringLiteral("在 Steam 中查看"), content);
    contentLayout->addWidget(openSteam);

    contentLayout->addWidget(sectionHeader(QStringLiteral("信息"), content));
    m_workshopId = new QLabel(content);
    m_workshopId->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_updatedAt = new QLabel(content);
    auto* info = new QVBoxLayout;
    info->addWidget(m_workshopId);
    info->addWidget(m_updatedAt);
    contentLayout->addLayout(info);
    contentLayout->addStretch(1);

    scroll->setWidget(content);
    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(16, 16, 16, 16);
    buttons->setSpacing(8);
    buttons->addStretch(1);
    auto* confirm = new QPushButton(QStringLiteral("确认"), page);
    auto* cancel = new QPushButton(QStringLiteral("取消"), page);
    confirm->setProperty("accent", true);
    confirm->setFixedWidth(70);
    cancel->setFixedWidth(70);
    buttons->addWidget(confirm);
    buttons->addWidget(cancel);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageLayout->addWidget(scroll, 1);
    pageLayout->addLayout(buttons);

    m_stack->addWidget(empty);
    m_stack->addWidget(page);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);

    connect(openSteam, &QPushButton::clicked, this, [this] {
        if (!m_item) return;
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://steamcommunity.com/sharedfiles/filedetails/?id=%1")
                                           .arg(m_item->publishedFileId)));
    });
    connect(confirm, &QPushButton::clicked, this, &WorkshopItemDetail::closeRequested);
    connect(cancel, &QPushButton::clicked, this, &WorkshopItemDetail::closeRequested);
    connect(m_viewModel, &WorkshopViewModel::downloadQueueChanged, this, &WorkshopItemDetail::rebuildDownloadSection);
    connect(m_viewModel, &WorkshopViewModel::steamSetupChanged, this, &WorkshopItemDetail::rebuildDownloadSection);
    connect(m_api, &SteamWebAPI::previewImageFinished, this,
            [this](const QUrl& url, const QByteArray& bytes, const QString&) {
        if (!m_item || m_item->previewImageUrl != url || bytes.isEmpty()) return;
        QPixmap pixmap;
        pixmap.loadFromData(bytes);
        if (!pixmap.isNull()) {
            m_preview->setPixmap(pixmap.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    });
    clearItem();
}

QLabel* WorkshopItemDetail::sectionHeader(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sectionHeader"));
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

void WorkshopItemDetail::setItem(const WorkshopItem& item) {
    m_item = item;
    updateContent();
    m_stack->setCurrentIndex(1);
}

void WorkshopItemDetail::clearItem() {
    m_item.reset();
    m_stack->setCurrentIndex(0);
}

void WorkshopItemDetail::updateContent() {
    if (!m_item) return;
    const WorkshopItem& item = *m_item;
    m_preview->setPixmap(QIcon::fromTheme(QStringLiteral("image-x-generic")).pixmap(52, 52));
    m_title->setText(item.title);
    m_presetNotice->setVisible(item.isPreset());
    m_subscriptions->setText(QStringLiteral("↓\n<b>%1</b>\n订阅").arg(item.formattedSubscriptions()));
    m_favorites->setText(QStringLiteral("♥\n<b>%1</b>\n收藏").arg(item.formattedFavorited()));
    m_views->setText(QStringLiteral("◉\n<b>%1</b>\n浏览").arg(item.formattedViews()));
    m_meta->setText(QStringLiteral("◆ %1    ▣ %2").arg(item.displayTypeName(), item.formattedFileSize()));
    m_tags->setText(item.tags.isEmpty() ? QStringLiteral("暂无标签") : item.tags.mid(0, 6).join(QStringLiteral("   ·   ")));
    m_description->setText(item.description.trimmed().isEmpty() ? QStringLiteral("暂无描述") : item.description);
    m_workshopId->setText(QStringLiteral("Workshop ID                                  %1").arg(item.publishedFileId));
    m_updatedAt->setText(QStringLiteral("更新时间                                      %1")
                             .arg(QLocale().toString(item.timeUpdated.date(), QLocale::ShortFormat)));
    rebuildDownloadSection();
    if (item.previewImageUrl.isValid()) m_api->downloadPreviewImage(item.previewImageUrl);
}

void WorkshopItemDetail::rebuildDownloadSection() {
    clearLayout(m_downloadLayout);
    if (!m_item) return;

    const WorkshopItem item = *m_item;
    const auto installed = m_viewModel->installedItem(item.publishedFileId);
    const auto state = m_viewModel->downloadStateFor(item.publishedFileId);

    auto addMessage = [this](const QString& text, const char* property) {
        auto* label = new QLabel(text, m_downloadSection);
        label->setWordWrap(true);
        label->setProperty(property, true);
        m_downloadLayout->addWidget(label);
    };
    auto addButton = [this](const QIcon& icon, const QString& text, const char* property = nullptr) {
        auto* button = new QPushButton(icon, text, m_downloadSection);
        if (property) button->setProperty(property, true);
        m_downloadLayout->addWidget(button);
        return button;
    };

    if (installed && installed->isPreset() && !installed->isValid()) {
        addMessage(QStringLiteral("预设已下载，但缺少基础壁纸 %1").arg(installed->presetDependency), "warning");
        auto* button = addButton(QIcon::fromTheme(QStringLiteral("folder-download")), QStringLiteral("下载基础壁纸"), "accent");
        connect(button, &QPushButton::clicked, this, [this, id = item.publishedFileId] {
            m_viewModel->requestPresetDependency(id);
        });
        return;
    }

    if (!state && installed && installed->isValid()) {
        auto* button = addButton(QIcon::fromTheme(QStringLiteral("emblem-default")),
                                 item.isPreset() ? QStringLiteral("预设已安装") : QStringLiteral("已下载"),
                                 "success");
        button->setEnabled(false);
        return;
    }

    if (m_viewModel->steamSetupState() != SteamSetupState::Ready) {
        addMessage(m_viewModel->steamSetupSummary(), "warning");
        auto* setup = addButton(QIcon::fromTheme(QStringLiteral("dialog-password")),
                                m_viewModel->steamSetupState() == SteamSetupState::SteamCMDMissing
                                    ? QStringLiteral("安装 SteamCMD")
                                    : QStringLiteral("登录全球 Steam"),
                                "accent");
        connect(setup, &QPushButton::clicked, this, &WorkshopItemDetail::steamSetupRequested);
        return;
    }

    if (state) {
        switch (state->kind) {
        case DownloadStateKind::Downloading: {
            auto* progress = new QProgressBar(m_downloadSection);
            progress->setRange(state->percent >= 0.0 ? 0 : 0, state->percent >= 0.0 ? 100 : 0);
            if (state->percent >= 0.0) progress->setValue(qRound(state->percent * 100.0));
            m_downloadLayout->addWidget(progress);
            addMessage(state->percent >= 0.0
                           ? QStringLiteral("%1% 下载中…").arg(qRound(state->percent * 100.0))
                           : QStringLiteral("正在连接 Steam…"),
                       "secondary");
            auto* cancel = addButton(QIcon::fromTheme(QStringLiteral("process-stop")), QStringLiteral("取消下载"), "danger");
            connect(cancel, &QPushButton::clicked, this, [this, id = item.publishedFileId] { m_viewModel->cancelDownload(id); });
            return;
        }
        case DownloadStateKind::Queued: {
            auto* button = addButton(QIcon::fromTheme(QStringLiteral("appointment-soon")), QStringLiteral("排队中..."));
            button->setEnabled(false);
            return;
        }
        case DownloadStateKind::Starting:
        case DownloadStateKind::Validating: {
            auto* progress = new QProgressBar(m_downloadSection);
            progress->setRange(0, 0);
            m_downloadLayout->addWidget(progress);
            addMessage(state->kind == DownloadStateKind::Starting
                           ? QStringLiteral("正在启动 SteamCMD…")
                           : QStringLiteral("正在验证下载文件…"),
                       "secondary");
            return;
        }
        case DownloadStateKind::Failed: {
            addMessage(state->message, "error");
            auto* retry = addButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("重试"), "accent");
            connect(retry, &QPushButton::clicked, this, [this, id = item.publishedFileId] { m_viewModel->retryDownload(id); });
            return;
        }
        case DownloadStateKind::Completed: {
            auto* button = addButton(QIcon::fromTheme(QStringLiteral("emblem-default")), QStringLiteral("已完成"), "success");
            button->setEnabled(false);
            return;
        }
        case DownloadStateKind::Cancelled:
            break;
        }
    }

    auto* download = addButton(QIcon::fromTheme(QStringLiteral("folder-download")),
                               item.isPreset() ? QStringLiteral("下载预设") : QStringLiteral("下载壁纸"),
                               "accent");
    connect(download, &QPushButton::clicked, this, [this, item] { m_viewModel->downloadItem(item); });
}

} // namespace Mirage

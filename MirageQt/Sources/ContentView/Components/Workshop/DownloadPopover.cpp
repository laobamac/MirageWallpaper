#include "ContentView/Components/Workshop/DownloadPopover.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace Mirage {
namespace {

QString stateText(const WorkshopDownloadTask& task) {
    const DownloadState& state = task.state;
    switch (state.kind) {
    case DownloadStateKind::Queued: return QStringLiteral("等待 SteamCMD 按顺序下载…");
    case DownloadStateKind::Starting: return QStringLiteral("正在启动 SteamCMD…");
    case DownloadStateKind::Downloading:
        return state.percent >= 0.0
            ? QStringLiteral("%1% 下载中…").arg(qRound(state.percent * 100.0))
            : QStringLiteral("正在连接 Steam…");
    case DownloadStateKind::Validating: return QStringLiteral("正在验证下载文件…");
    case DownloadStateKind::Completed: return QStringLiteral("已完成");
    case DownloadStateKind::Failed: return QStringLiteral("失败: %1").arg(state.message);
    case DownloadStateKind::Cancelled: return QStringLiteral("已取消");
    }
    return {};
}

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
}

} // namespace

DownloadPopover::DownloadPopover(WorkshopViewModel* viewModel,
                                 SteamWebAPI* api,
                                 QWidget* parent)
    : QFrame(parent, Qt::Popup)
    , m_viewModel(viewModel)
    , m_api(api) {
    setObjectName(QStringLiteral("downloadPopover"));
    setFrameShape(QFrame::StyledPanel);
    setFixedWidth(420);

    auto* title = new QLabel(QStringLiteral("下载管理"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPixelSize(15);
    title->setFont(titleFont);
    m_clear = new QPushButton(QStringLiteral("清除记录"), this);
    m_clear->setProperty("flatAction", true);

    auto* header = new QHBoxLayout;
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(m_clear);

    m_rows = new QWidget(this);
    m_rowsLayout = new QVBoxLayout(m_rows);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(1);
    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setWidget(m_rows);
    m_scroll->setMaximumHeight(400);

    m_empty = new QLabel(QStringLiteral("暂无下载任务\n在创意工坊中浏览并下载壁纸"), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setMinimumHeight(130);
    m_empty->setProperty("secondary", true);

    m_footer = new QLabel(this);
    m_footer->setProperty("secondary", true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 10, 16, 10);
    layout->setSpacing(8);
    layout->addLayout(header);
    layout->addWidget(m_empty);
    layout->addWidget(m_scroll);
    layout->addWidget(m_footer);

    connect(m_clear, &QPushButton::clicked, m_viewModel, &WorkshopViewModel::clearCompletedDownloads);
    connect(m_viewModel, &WorkshopViewModel::downloadQueueChanged, this, &DownloadPopover::rebuild);
    connect(m_api, &SteamWebAPI::previewImageFinished, this,
            [this](const QUrl& url, const QByteArray& bytes, const QString&) {
        if (bytes.isEmpty()) return;
        QPixmap preview;
        preview.loadFromData(bytes);
        if (preview.isNull()) return;
        for (const QPointer<QLabel>& label : m_previewLabels.values(url)) {
            if (label) label->setPixmap(preview.scaled(label->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        }
    });
    rebuild();
}

void DownloadPopover::showBelow(QWidget* anchor) {
    adjustSize();
    const QPoint bottomRight = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 6));
    move(bottomRight.x() - width(), bottomRight.y());
    show();
    raise();
}

void DownloadPopover::rebuild() {
    m_previewLabels.clear();
    clearLayout(m_rowsLayout);

    bool hasCompleted = false;
    int completed = 0;
    for (const WorkshopDownloadTask& task : m_viewModel->downloadQueue()) {
        m_rowsLayout->addWidget(buildRow(task));
        if (task.state.kind == DownloadStateKind::Completed || task.state.kind == DownloadStateKind::Failed) {
            hasCompleted = true;
        }
        if (task.state.kind == DownloadStateKind::Completed) ++completed;
    }
    m_rowsLayout->addStretch(1);

    const bool empty = m_viewModel->downloadQueue().isEmpty();
    m_empty->setVisible(empty);
    m_scroll->setVisible(!empty);
    m_clear->setVisible(hasCompleted);
    m_footer->setText(QStringLiteral("↓ %1 下载中                                      ✓ %2 已完成")
                          .arg(m_viewModel->activeDownloadCount())
                          .arg(completed));
    adjustSize();
}

QWidget* DownloadPopover::buildRow(const WorkshopDownloadTask& task) {
    auto* row = new QWidget(m_rows);
    row->setObjectName(QStringLiteral("downloadRow"));

    auto* preview = new QLabel(row);
    preview->setFixedSize(64, 36);
    preview->setAlignment(Qt::AlignCenter);
    preview->setPixmap(QIcon::fromTheme(QStringLiteral("image-x-generic")).pixmap(28, 28));
    if (task.workshopItem.previewImageUrl.isValid()) {
        m_previewLabels.insert(task.workshopItem.previewImageUrl, preview);
        m_api->downloadPreviewImage(task.workshopItem.previewImageUrl);
    }

    auto* title = new QLabel(task.workshopItem.title, row);
    title->setTextInteractionFlags(Qt::NoTextInteraction);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setToolTip(task.workshopItem.title);

    QString suffix;
    if (task.workshopItem.isPreset()) suffix = QStringLiteral("  ·  预设");
    else if (task.purpose == DownloadPurpose::PresetDependency) suffix = QStringLiteral("  ·  基础壁纸");
    auto* state = new QLabel(stateText(task) + suffix, row);
    state->setProperty(task.state.kind == DownloadStateKind::Failed ? "error" : "secondary", true);

    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    text->addWidget(title);
    if (task.state.kind == DownloadStateKind::Downloading && task.state.percent >= 0.0) {
        auto* progress = new QProgressBar(row);
        progress->setRange(0, 100);
        progress->setValue(qRound(task.state.percent * 100.0));
        progress->setTextVisible(false);
        progress->setFixedHeight(5);
        text->addWidget(progress);
    }
    text->addWidget(state);

    auto* action = new QToolButton(row);
    action->setProperty("flatButton", true);
    action->setFixedSize(30, 30);
    const QString id = task.workshopItem.publishedFileId;
    switch (task.state.kind) {
    case DownloadStateKind::Queued:
    case DownloadStateKind::Starting:
    case DownloadStateKind::Downloading:
        action->setIcon(QIcon::fromTheme(QStringLiteral("process-stop")));
        action->setToolTip(QStringLiteral("取消下载"));
        connect(action, &QToolButton::clicked, this, [this, id] { m_viewModel->cancelDownload(id); });
        break;
    case DownloadStateKind::Failed:
        action->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
        action->setToolTip(QStringLiteral("重试"));
        connect(action, &QToolButton::clicked, this, [this, id] { m_viewModel->retryDownload(id); });
        break;
    case DownloadStateKind::Completed:
        action->setIcon(QIcon::fromTheme(QStringLiteral("folder-open")));
        action->setToolTip(QStringLiteral("打开下载目录"));
        connect(action, &QToolButton::clicked, this, [this, id] { revealDownload(id); });
        break;
    case DownloadStateKind::Validating:
    case DownloadStateKind::Cancelled:
        action->setVisible(false);
        break;
    }

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 7, 8, 7);
    layout->setSpacing(10);
    layout->addWidget(preview);
    layout->addLayout(text, 1);
    layout->addWidget(action);
    return row;
}

void DownloadPopover::revealDownload(const QString& workshopId) {
    const auto wallpaper = m_viewModel->installedItem(workshopId);
    if (!wallpaper || !QFileInfo::exists(wallpaper->wallpaperDirectory)) {
        QMessageBox::warning(this, QStringLiteral("无法打开下载目录"), QStringLiteral("未找到该壁纸的本地下载目录。"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(wallpaper->wallpaperDirectory));
}

} // namespace Mirage

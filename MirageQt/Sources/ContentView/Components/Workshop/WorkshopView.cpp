#include "ContentView/Components/Workshop/WorkshopView.h"

#include "ContentView/Components/Workshop/DownloadPopover.h"
#include "ContentView/Components/Workshop/WorkshopItemCard.h"
#include "ContentView/Components/Workshop/WorkshopSearchBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace Mirage {
namespace {

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
}

QWidget* centeredMessage(const QString& icon, QLabel** title, QLabel** detail, QPushButton** action, QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* symbol = new QLabel(icon, page);
    symbol->setAlignment(Qt::AlignCenter);
    QFont symbolFont = symbol->font();
    symbolFont.setPointSize(32);
    symbol->setFont(symbolFont);
    symbol->setProperty("tertiary", true);
    *title = new QLabel(page);
    (*title)->setAlignment(Qt::AlignCenter);
    QFont titleFont = (*title)->font();
    titleFont.setPixelSize(17);
    (*title)->setFont(titleFont);
    (*title)->setProperty("secondary", true);
    *detail = new QLabel(page);
    (*detail)->setAlignment(Qt::AlignCenter);
    (*detail)->setWordWrap(true);
    (*detail)->setProperty("tertiary", true);
    *action = new QPushButton(QStringLiteral("重试"), page);
    (*action)->setProperty("accent", true);
    auto* layout = new QVBoxLayout(page);
    layout->addStretch(1);
    layout->addWidget(symbol);
    layout->addWidget(*title);
    layout->addWidget(*detail);
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    buttonRow->addWidget(*action);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);
    layout->addStretch(1);
    return page;
}

} // namespace

WorkshopView::WorkshopView(WorkshopViewModel* viewModel,
                           SteamWebAPI* api,
                           SteamCMDManager* steamCMD,
                           GlobalSettingsService* settings,
                           QWidget* parent)
    : QWidget(parent)
    , m_viewModel(viewModel)
    , m_api(api)
    , m_steamCMD(steamCMD)
    , m_settings(settings) {
    m_apiKeyBanner = new QWidget(this);
    m_apiKeyBanner->setObjectName(QStringLiteral("apiKeyBanner"));
    auto* keyIcon = new QLabel(QStringLiteral("⚿"), m_apiKeyBanner);
    QFont keyIconFont = keyIcon->font();
    keyIconFont.setPointSize(20);
    keyIcon->setFont(keyIconFont);
    auto* keyText = new QLabel(QStringLiteral("<b>建议设置专属 Steam Web API Key</b><br>"
                                                  "<span style='color:#aaa59f'>内置 Key 由所有用户共享，可能因请求过多影响浏览；专属 Key 不影响 SteamCMD 登录和下载。</span>"),
                               m_apiKeyBanner);
    keyText->setWordWrap(true);
    auto* keySettings = new QPushButton(QStringLiteral("立即设置"), m_apiKeyBanner);
    keySettings->setProperty("warningAccent", true);
    auto* keyLayout = new QHBoxLayout(m_apiKeyBanner);
    keyLayout->setContentsMargins(12, 9, 12, 9);
    keyLayout->setSpacing(12);
    keyLayout->addWidget(keyIcon);
    keyLayout->addWidget(keyText, 1);
    keyLayout->addWidget(keySettings);

    m_searchBar = new WorkshopSearchBar(m_viewModel, this);
    m_downloadButton = new QToolButton(this);
    m_downloadButton->setIcon(QIcon::fromTheme(QStringLiteral("folder-download")));
    m_downloadButton->setToolTip(QStringLiteral("下载管理"));
    m_downloadButton->setProperty("flatButton", true);
    m_downloadButton->setFixedSize(34, 34);
    m_downloadBadge = new QLabel(m_downloadButton);
    m_downloadBadge->setObjectName(QStringLiteral("downloadBadge"));
    m_downloadBadge->setAlignment(Qt::AlignCenter);
    m_downloadBadge->setFixedSize(17, 17);
    m_downloadBadge->move(20, -1);

    m_account = new QWidget(this);
    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(8);
    toolbar->addWidget(m_searchBar, 1);
    toolbar->addWidget(m_downloadButton);
    toolbar->addWidget(m_account);

    m_setupBanner = new QWidget(this);
    m_setupBanner->setObjectName(QStringLiteral("steamSetupBanner"));
    auto* cloud = new QLabel(QStringLiteral("☁"), m_setupBanner);
    QFont cloudFont = cloud->font();
    cloudFont.setPointSize(21);
    cloud->setFont(cloudFont);
    m_setupSummary = new QLabel(m_setupBanner);
    m_setupSummary->setWordWrap(true);
    auto* setup = new QPushButton(QStringLiteral("立即设置"), m_setupBanner);
    setup->setProperty("accent", true);
    auto* setupLayout = new QHBoxLayout(m_setupBanner);
    setupLayout->setContentsMargins(12, 9, 12, 9);
    setupLayout->setSpacing(12);
    setupLayout->addWidget(cloud);
    setupLayout->addWidget(m_setupSummary, 1);
    setupLayout->addWidget(setup);

    m_stack = new QStackedWidget(this);
    auto* loading = new QWidget(m_stack);
    auto* loadingProgress = new QProgressBar(loading);
    loadingProgress->setRange(0, 0);
    loadingProgress->setFixedWidth(180);
    auto* loadingText = new QLabel(QStringLiteral("正在搜索创意工坊..."), loading);
    loadingText->setAlignment(Qt::AlignCenter);
    loadingText->setProperty("secondary", true);
    auto* loadingLayout = new QVBoxLayout(loading);
    loadingLayout->addStretch(1);
    loadingLayout->addWidget(loadingProgress, 0, Qt::AlignHCenter);
    loadingLayout->addWidget(loadingText);
    loadingLayout->addStretch(1);

    QWidget* empty = centeredMessage(QStringLiteral("⌕"), &m_emptyTitle, &m_emptyDetail, &m_retry, m_stack);

    auto* results = new QWidget(m_stack);
    m_list = new QListWidget(results);
    m_list->setItemDelegate(new WorkshopItemCard(m_list));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setViewMode(QListView::IconMode);
    m_list->setFlow(QListView::LeftToRight);
    m_list->setWrapping(true);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setUniformItemSizes(true);
    m_list->setMouseTracking(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setStyleSheet(QStringLiteral("QListWidget { border: 0; outline: 0; } QListWidget::item { background: transparent; border: 0; }"));
    m_busy = new QProgressBar(results);
    m_busy->setRange(0, 0);
    m_busy->setFixedHeight(5);
    m_busy->setTextVisible(false);
    m_previous = new QPushButton(QIcon::fromTheme(QStringLiteral("go-previous")), QString(), results);
    m_previous->setToolTip(QStringLiteral("上一页"));
    m_previous->setFixedWidth(34);
    m_next = new QPushButton(QIcon::fromTheme(QStringLiteral("go-next")), QString(), results);
    m_next->setToolTip(QStringLiteral("下一页"));
    m_next->setFixedWidth(34);
    m_page = new QLabel(results);
    m_page->setAlignment(Qt::AlignCenter);
    m_page->setMinimumWidth(65);
    m_page->setProperty("secondary", true);
    auto* pages = new QHBoxLayout;
    pages->addStretch(1);
    pages->addWidget(m_previous);
    pages->addWidget(m_page);
    pages->addWidget(m_next);
    pages->addStretch(1);
    auto* resultsLayout = new QVBoxLayout(results);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->setSpacing(4);
    resultsLayout->addWidget(m_list, 1);
    resultsLayout->addWidget(m_busy);
    resultsLayout->addLayout(pages);

    m_stack->addWidget(loading);
    m_stack->addWidget(empty);
    m_stack->addWidget(results);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(m_apiKeyBanner);
    layout->addLayout(toolbar);
    layout->addWidget(m_setupBanner);
    layout->addWidget(m_stack, 1);

    m_downloadPopover = new DownloadPopover(m_viewModel, m_api, this);

    connect(keySettings, &QPushButton::clicked, this, &WorkshopView::settingsRequested);
    connect(setup, &QPushButton::clicked, this, &WorkshopView::steamSetupRequested);
    connect(m_downloadButton, &QToolButton::clicked, this, [this] { m_downloadPopover->showBelow(m_downloadButton); });
    connect(m_retry, &QPushButton::clicked, m_viewModel, &WorkshopViewModel::search);
    connect(m_previous, &QPushButton::clicked, m_viewModel, &WorkshopViewModel::loadPreviousPage);
    connect(m_next, &QPushButton::clicked, m_viewModel, &WorkshopViewModel::loadNextPage);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* row) {
        m_viewModel->selectWorkshopItem(row->data(WorkshopItemRole).value<WorkshopItem>());
    });
    connect(m_viewModel, &WorkshopViewModel::browseChanged, this, &WorkshopView::updateBrowseState);
    connect(m_viewModel, &WorkshopViewModel::downloadQueueChanged, this, [this] {
        rebuildList();
        const int active = m_viewModel->activeDownloadCount();
        m_downloadBadge->setText(QString::number(active));
        m_downloadBadge->setVisible(active > 0);
    });
    connect(m_viewModel, &WorkshopViewModel::steamSetupChanged, this, &WorkshopView::updateSteamState);
    connect(m_viewModel, &WorkshopViewModel::selectedItemChanged, this, &WorkshopView::selectCurrentModelItem);
    connect(m_settings, &GlobalSettingsService::settingsChanged, this, &WorkshopView::updateAPIKeyBanner);
    connect(m_api, &SteamWebAPI::previewImageFinished, this,
            [this](const QUrl& url, const QByteArray& bytes, const QString&) {
        if (bytes.isEmpty()) return;
        QPixmap pixmap;
        pixmap.loadFromData(bytes);
        if (pixmap.isNull()) return;
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* row = m_list->item(i);
            if (row->data(WorkshopItemRole).value<WorkshopItem>().previewImageUrl == url) {
                row->setData(WorkshopPreviewRole, pixmap);
            }
        }
        m_list->viewport()->update();
    });

    updateAPIKeyBanner();
    updateSteamState();
    updateBrowseState();
}

void WorkshopView::activate() {
    m_viewModel->checkSteamSetup();
    if (m_viewModel->items().isEmpty() && !m_viewModel->isLoading()) m_viewModel->search();
}

void WorkshopView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateGridMetrics();
}

void WorkshopView::updateGridMetrics() {
    if (!m_list || m_list->viewport()->width() <= 0) return;
    const int available = m_list->viewport()->width() - 4;
    const int columns = qMax(1, (available + 12) / (220 + 12));
    const int cardWidth = qBound(220, (available - (columns - 1) * 8) / columns, 440);
    m_list->setGridSize(QSize(cardWidth, qRound(cardWidth * 9.0 / 16.0)));
}

void WorkshopView::rebuildList() {
    QString selectedId;
    if (m_list->currentItem()) {
        selectedId = m_list->currentItem()->data(WorkshopItemRole).value<WorkshopItem>().publishedFileId;
    }
    m_list->clear();
    int selectedRow = -1;
    for (int i = 0; i < m_viewModel->items().size(); ++i) {
        const WorkshopItem& item = m_viewModel->items().at(i);
        auto* row = new QListWidgetItem(m_list);
        setWorkshopCardData(row,
                            item,
                            m_viewModel->isItemDownloaded(item.publishedFileId),
                            m_viewModel->presetNeedsDependency(item.publishedFileId),
                            m_viewModel->downloadStateFor(item.publishedFileId));
        if (item.publishedFileId == selectedId ||
            (m_viewModel->selectedItem() && m_viewModel->selectedItem()->publishedFileId == item.publishedFileId)) {
            selectedRow = i;
        }
        if (item.previewImageUrl.isValid()) m_api->downloadPreviewImage(item.previewImageUrl);
    }
    if (selectedRow >= 0) m_list->setCurrentRow(selectedRow);
    updateGridMetrics();
}

void WorkshopView::updateBrowseState() {
    rebuildList();
    if (m_viewModel->isLoading() && m_viewModel->items().isEmpty()) {
        m_stack->setCurrentIndex(0);
    } else if (m_viewModel->items().isEmpty()) {
        m_stack->setCurrentIndex(1);
        if (m_viewModel->error().isEmpty()) {
            m_emptyTitle->setText(QStringLiteral("没有找到壁纸"));
            m_emptyDetail->setText(QStringLiteral("试试调整搜索条件或筛选标签"));
            m_emptyDetail->setProperty("error", false);
            m_retry->hide();
        } else {
            m_emptyTitle->setText(QStringLiteral("加载失败"));
            m_emptyDetail->setText(m_viewModel->error());
            m_emptyDetail->setProperty("error", true);
            m_retry->show();
        }
        m_emptyDetail->style()->unpolish(m_emptyDetail);
        m_emptyDetail->style()->polish(m_emptyDetail);
    } else {
        m_stack->setCurrentIndex(2);
    }
    m_busy->setVisible(m_viewModel->isLoading() && !m_viewModel->items().isEmpty());
    m_page->setText(QStringLiteral("%1 / %2").arg(m_viewModel->currentPage()).arg(m_viewModel->totalPages()));
    m_previous->setEnabled(m_viewModel->currentPage() > 1 && !m_viewModel->isLoading());
    m_next->setEnabled(m_viewModel->currentPage() < m_viewModel->totalPages() && !m_viewModel->isLoading());
}

void WorkshopView::updateSteamState() {
    const bool ready = m_viewModel->steamSetupState() == SteamSetupState::Ready;
    m_setupBanner->setVisible(!ready);
    m_setupSummary->setText(QStringLiteral("<b>连接 Steam 以下载壁纸</b><br>"
                                                "<span style='color:#aaa59f'>设置 SteamCMD 后可直接从创意工坊下载壁纸到本地（需拥有 Wallpaper Engine）</span>"));

    QLayout* layout = m_account->layout();
    if (!layout) layout = new QHBoxLayout(m_account);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    clearLayout(layout);
    if (ready) {
        auto* user = new QLabel(QStringLiteral("●  %1").arg(m_steamCMD->savedUsername()), m_account);
        user->setObjectName(QStringLiteral("steamAccount"));
        layout->addWidget(user);
        auto* logout = new QToolButton(m_account);
        logout->setIcon(QIcon::fromTheme(QStringLiteral("system-log-out")));
        logout->setToolTip(QStringLiteral("退出 Steam"));
        logout->setProperty("flatButton", true);
        layout->addWidget(logout);
        connect(logout, &QToolButton::clicked, m_viewModel, &WorkshopViewModel::logout);
    } else {
        auto* button = new QPushButton(QIcon::fromTheme(QStringLiteral("settings-configure")), QStringLiteral("设置 Steam"), m_account);
        button->setProperty("accent", true);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, &WorkshopView::steamSetupRequested);
    }
}

void WorkshopView::updateAPIKeyBanner() {
    m_apiKeyBanner->setVisible(!m_settings->hasValidCustomSteamAPIKey());
}

void WorkshopView::selectCurrentModelItem() {
    if (!m_viewModel->selectedItem()) {
        m_list->clearSelection();
        return;
    }
    const QString id = m_viewModel->selectedItem()->publishedFileId;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(WorkshopItemRole).value<WorkshopItem>().publishedFileId == id) {
            m_list->setCurrentRow(i);
            return;
        }
    }
}

} // namespace Mirage

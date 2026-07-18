#include "ContentView/Components/WorkshopWidget.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QUrl>
#include <QVBoxLayout>

namespace Mirage {
namespace {

QString sortLabel(WorkshopSortOrder order) {
    switch (order) {
    case WorkshopSortOrder::Trending: return QStringLiteral("热门趋势");
    case WorkshopSortOrder::MostRecent: return QStringLiteral("最新发布");
    case WorkshopSortOrder::MostSubscribed: return QStringLiteral("订阅最多");
    case WorkshopSortOrder::TopRated: return QStringLiteral("评分最高");
    }
    return {};
}

QString sizeText(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("未知大小");
    return QLocale().formattedDataSize(bytes);
}

} // namespace

WorkshopWidget::WorkshopWidget(SteamWebAPI* api,
                               SteamCMDManager* steamCMD,
                               WallpaperLibrary* library,
                               QWidget* parent)
    : QWidget(parent)
    , m_api(api)
    , m_steamCMD(steamCMD)
    , m_library(library) {
    m_apiKeyBanner = new QLabel(QStringLiteral("建议设置专属 Steam API Key，内置 Key 繁忙时可能导致创意工坊无法加载。"), this);
    m_apiKeyBanner->setStyleSheet(QStringLiteral("padding: 8px; background: rgba(255, 190, 0, 35); border-radius: 6px;"));

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索创意工坊"));
    m_sort = new QComboBox(this);
    m_sort->addItems({sortLabel(WorkshopSortOrder::Trending), sortLabel(WorkshopSortOrder::MostRecent),
                      sortLabel(WorkshopSortOrder::MostSubscribed), sortLabel(WorkshopSortOrder::TopRated)});
    m_type = new QComboBox(this);
    m_type->addItems({QStringLiteral("全部"), QStringLiteral("场景"), QStringLiteral("网页"), QStringLiteral("视频"), QStringLiteral("预设")});
    auto* searchButton = new QPushButton(QIcon::fromTheme("edit-find"), QStringLiteral("搜索"), this);
    auto* downloads = new QPushButton(QIcon::fromTheme("go-down"), QStringLiteral("下载队列"), this);
    auto* steamSetup = new QPushButton(QIcon::fromTheme("settings-configure"), QStringLiteral("设置 Steam"), this);

    auto* searchRow = new QHBoxLayout;
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(m_sort);
    searchRow->addWidget(m_type);
    searchRow->addWidget(searchButton);
    searchRow->addWidget(downloads);
    searchRow->addWidget(steamSetup);

    m_setupBanner = new QLabel(this);
    m_setupBanner->setStyleSheet(QStringLiteral("padding: 10px; background: rgba(60, 130, 230, 30); border-radius: 8px;"));

    m_list = new QListWidget(this);
    m_list->setViewMode(QListView::IconMode);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setIconSize(QSize(220, 124));
    m_list->setGridSize(QSize(260, 208));
    m_list->setWordWrap(true);

    m_download = new QPushButton(QIcon::fromTheme("go-down"), QStringLiteral("下载选中项目"), this);
    auto* openSteam = new QPushButton(QIcon::fromTheme("internet-web-browser"), QStringLiteral("在 Steam 中查看"), this);
    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    actions->addWidget(m_download);
    actions->addWidget(openSteam);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(m_apiKeyBanner);
    layout->addLayout(searchRow);
    layout->addWidget(m_setupBanner);
    layout->addWidget(m_list, 1);
    layout->addLayout(actions);

    connect(searchButton, &QPushButton::clicked, this, &WorkshopWidget::search);
    connect(m_search, &QLineEdit::returnPressed, this, &WorkshopWidget::search);
    connect(steamSetup, &QPushButton::clicked, this, &WorkshopWidget::steamSetupRequested);
    connect(m_api, &SteamWebAPI::queryFinished, this, [this](const WorkshopQueryResult& result) {
        if (!result.error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("创意工坊"), result.error);
            return;
        }
        rebuildList(result.items);
    });
    connect(m_api, &SteamWebAPI::previewImageFinished, this, [this](const QUrl& url, const QByteArray& bytes, const QString&) {
        if (bytes.isEmpty()) return;
        QPixmap pixmap;
        pixmap.loadFromData(bytes);
        if (pixmap.isNull()) return;
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items.at(i).previewImageUrl == url && i < m_list->count()) {
                m_list->item(i)->setIcon(QIcon(pixmap.scaled(QSize(220, 124), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)));
            }
        }
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < m_items.size()) emit itemSelected(m_items.at(row));
    });
    connect(m_download, &QPushButton::clicked, this, [this] {
        const WorkshopItem item = currentItem();
        if (!item.publishedFileId.isEmpty()) emit downloadRequested(item);
    });
    connect(openSteam, &QPushButton::clicked, this, [this] {
        const WorkshopItem item = currentItem();
        if (!item.publishedFileId.isEmpty()) {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://steamcommunity.com/sharedfiles/filedetails/?id=%1")
                                               .arg(item.publishedFileId)));
        }
    });
    connect(m_steamCMD, &SteamCMDManager::authenticationChanged, this, &WorkshopWidget::updateSteamBanner);
    connect(m_steamCMD, &SteamCMDManager::steamCMDPathChanged, this, &WorkshopWidget::updateSteamBanner);

    updateSteamBanner();
}

WorkshopItem WorkshopWidget::currentItem() const {
    const int row = m_list->currentRow();
    return row >= 0 && row < m_items.size() ? m_items.at(row) : WorkshopItem();
}

void WorkshopWidget::search() {
    WorkshopQuery query;
    query.searchText = m_search->text();
    switch (m_sort->currentIndex()) {
    case 1: query.sortOrder = WorkshopSortOrder::MostRecent; break;
    case 2: query.sortOrder = WorkshopSortOrder::MostSubscribed; break;
    case 3: query.sortOrder = WorkshopSortOrder::TopRated; break;
    default: query.sortOrder = WorkshopSortOrder::Trending; break;
    }
    switch (m_type->currentIndex()) {
    case 1: query.typeFilter = WorkshopTypeFilter::Scene; break;
    case 2: query.typeFilter = WorkshopTypeFilter::Web; break;
    case 3: query.typeFilter = WorkshopTypeFilter::Video; break;
    case 4: query.typeFilter = WorkshopTypeFilter::Preset; break;
    default: query.typeFilter = WorkshopTypeFilter::All; break;
    }
    m_api->queryFiles(query);
}

void WorkshopWidget::rebuildList(const QVector<WorkshopItem>& items) {
    m_items = items;
    m_list->clear();
    for (int i = 0; i < m_items.size(); ++i) {
        const WorkshopItem& item = m_items.at(i);
        auto* row = new QListWidgetItem(QStringLiteral("%1\n%2 · %3")
                                            .arg(item.title, item.displayTypeName(), sizeText(item.fileSize)),
                                        m_list);
        row->setData(Qt::UserRole, i);
        row->setIcon(QIcon::fromTheme("preferences-desktop-wallpaper"));
        if (item.previewImageUrl.isValid()) m_api->downloadPreviewImage(item.previewImageUrl);
    }
    if (m_list->count() > 0) m_list->setCurrentRow(0);
}

void WorkshopWidget::updateSteamBanner() {
    if (!m_steamCMD->steamCMDPath().isEmpty() && m_steamCMD->isLoggedIn()) {
        m_setupBanner->setText(QStringLiteral("已登录 %1，可从创意工坊下载壁纸。").arg(m_steamCMD->savedUsername()));
    } else {
        m_setupBanner->setText(QStringLiteral("连接 Steam 以下载壁纸：设置 SteamCMD 后可直接从创意工坊下载壁纸到本地。"));
    }
}

} // namespace Mirage

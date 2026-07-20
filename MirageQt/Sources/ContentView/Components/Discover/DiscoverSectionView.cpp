#include "ContentView/Components/Discover/DiscoverSectionView.h"

#include "ContentView/Components/Workshop/WorkshopItemCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace Mirage {
namespace {

class DiscoverCardDelegate final : public QStyledItemDelegate {
public:
    explicit DiscoverCardDelegate(QObject* parent)
        : QStyledItemDelegate(parent) {
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(230, 170);
    }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        const WorkshopItem item = index.data(WorkshopItemRole).value<WorkshopItem>();
        const QRectF card = option.rect.adjusted(5, 4, -5, -4);
        const QRectF previewRect(card.left(), card.top(), card.width(), 124);

        QPainterPath clip;
        clip.addRoundedRect(card, 8, 8);
        painter->setClipPath(clip);
        painter->fillPath(clip, option.palette.color(QPalette::Base));
        const QPixmap preview = index.data(WorkshopPreviewRole).value<QPixmap>();
        if (!preview.isNull()) {
            const qreal targetRatio = previewRect.width() / previewRect.height();
            QRectF source = preview.rect();
            const qreal sourceRatio = qreal(preview.width()) / qMax(1, preview.height());
            if (sourceRatio > targetRatio) {
                const qreal width = preview.height() * targetRatio;
                source.setLeft((preview.width() - width) / 2.0);
                source.setWidth(width);
            } else {
                const qreal height = preview.width() / targetRatio;
                source.setTop((preview.height() - height) / 2.0);
                source.setHeight(height);
            }
            painter->drawPixmap(previewRect, preview, source);
        } else {
            painter->fillRect(previewRect, option.palette.color(QPalette::AlternateBase));
            const QPixmap fallback = QIcon::fromTheme(QStringLiteral("image-x-generic")).pixmap(36, 36);
            painter->drawPixmap(QRectF(previewRect.center().x() - 18, previewRect.center().y() - 18, 36, 36), fallback, fallback.rect());
        }
        painter->setClipping(false);

        if (item.isPreset()) {
            const QRectF badge(card.left() + 7, card.top() + 7, 38, 20);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#a747d1")));
            painter->drawRoundedRect(badge, 4, 4);
            painter->setPen(Qt::white);
            QFont badgeFont = option.font;
            badgeFont.setPixelSize(10);
            badgeFont.setBold(true);
            painter->setFont(badgeFont);
            painter->drawText(badge, Qt::AlignCenter, QStringLiteral("预设"));
        }

        if (index.data(WorkshopDownloadedRole).toBool()) {
            const bool warning = index.data(WorkshopPresetNeedsDependencyRole).toBool();
            const QRectF badge(card.right() - 27, card.top() + 7, 20, 20);
            painter->setPen(Qt::NoPen);
            painter->setBrush(warning ? QColor(QStringLiteral("#d98222")) : QColor(QStringLiteral("#29964a")));
            painter->drawEllipse(badge);
            painter->setPen(Qt::white);
            painter->drawText(badge, Qt::AlignCenter, warning ? QStringLiteral("!") : QStringLiteral("✓"));
        }

        QFont titleFont = option.font;
        titleFont.setPixelSize(11);
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(option.palette.color(QPalette::Text));
        const QRectF titleRect(card.left() + 8, previewRect.bottom() + 4, card.width() - 16, 17);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(titleFont).elidedText(item.title, Qt::ElideRight, qRound(titleRect.width())));

        QFont metaFont = option.font;
        metaFont.setPixelSize(9);
        painter->setFont(metaFont);
        painter->setPen(option.palette.color(QPalette::PlaceholderText));
        painter->drawText(QRectF(card.left() + 8, previewRect.bottom() + 22, card.width() - 16, 15),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QStringLiteral("↓ %1     ◆ %2").arg(item.formattedSubscriptions(), item.displayTypeName()));

        if (option.state.testFlag(QStyle::State_Selected) || option.state.testFlag(QStyle::State_MouseOver)) {
            painter->setPen(QPen(QColor(QStringLiteral("#0a84ff")),
                                 option.state.testFlag(QStyle::State_Selected) ? 2.5 : 1.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(card.adjusted(1, 1, -1, -1), 8, 8);
        }
        painter->restore();
    }
};

} // namespace

DiscoverSectionView::DiscoverSectionView(const QString& title,
                                         const QString& iconName,
                                         const QColor& iconColor,
                                         DiscoverCollection collection,
                                         WorkshopViewModel* viewModel,
                                         SteamWebAPI* api,
                                         QWidget* parent)
    : QWidget(parent)
    , m_collection(collection)
    , m_viewModel(viewModel)
    , m_api(api) {
    auto* icon = new QLabel(this);
    icon->setPixmap(QIcon::fromTheme(iconName).pixmap(20, 20));
    icon->setStyleSheet(QStringLiteral("color: %1;").arg(iconColor.name()));
    auto* heading = new QLabel(title, this);
    QFont headingFont = heading->font();
    headingFont.setPixelSize(17);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    auto* seeAll = new QPushButton(QStringLiteral("查看全部  ›"), this);
    seeAll->setProperty("flatAction", true);
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(icon);
    header->addWidget(heading);
    header->addStretch(1);
    header->addWidget(seeAll);

    m_list = new QListWidget(this);
    m_list->setItemDelegate(new DiscoverCardDelegate(m_list));
    m_list->setViewMode(QListView::IconMode);
    m_list->setFlow(QListView::LeftToRight);
    m_list->setWrapping(false);
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Fixed);
    m_list->setGridSize(QSize(230, 170));
    m_list->setUniformItemSizes(true);
    m_list->setMouseTracking(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setFixedHeight(174);
    m_list->setStyleSheet(QStringLiteral("QListWidget { border: 0; outline: 0; } QListWidget::item { background: transparent; border: 0; }"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);
    layout->addLayout(header);
    layout->addWidget(m_list);

    connect(seeAll, &QPushButton::clicked, this, &DiscoverSectionView::showAll);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* row) {
        m_viewModel->selectWorkshopItem(row->data(WorkshopItemRole).value<WorkshopItem>());
    });
    connect(m_viewModel, &WorkshopViewModel::discoverChanged, this, &DiscoverSectionView::rebuild);
    connect(m_viewModel, &WorkshopViewModel::downloadQueueChanged, this, &DiscoverSectionView::rebuild);
    connect(m_viewModel, &WorkshopViewModel::selectedItemChanged, this, &DiscoverSectionView::selectCurrentModelItem);
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
    rebuild();
}

void DiscoverSectionView::rebuild() {
    const QVector<WorkshopItem>& items = m_viewModel->discoverItems(m_collection);
    setVisible(!items.isEmpty());
    m_list->clear();
    for (const WorkshopItem& item : items) {
        auto* row = new QListWidgetItem(m_list);
        setWorkshopCardData(row,
                            item,
                            m_viewModel->isItemDownloaded(item.publishedFileId),
                            m_viewModel->presetNeedsDependency(item.publishedFileId),
                            m_viewModel->downloadStateFor(item.publishedFileId));
        if (item.previewImageUrl.isValid()) m_api->downloadPreviewImage(item.previewImageUrl);
    }
    selectCurrentModelItem();
}

void DiscoverSectionView::showAll() {
    switch (m_collection) {
    case DiscoverCollection::Trending: m_viewModel->navigateToWorkshopWithSort(WorkshopSortOrder::Trending); break;
    case DiscoverCollection::MostRecent: m_viewModel->navigateToWorkshopWithSort(WorkshopSortOrder::MostRecent); break;
    case DiscoverCollection::MostSubscribed: m_viewModel->navigateToWorkshopWithSort(WorkshopSortOrder::MostSubscribed); break;
    case DiscoverCollection::TopRated: m_viewModel->navigateToWorkshopWithSort(WorkshopSortOrder::TopRated); break;
    case DiscoverCollection::Anime: m_viewModel->navigateToWorkshopWithTag(QStringLiteral("Anime")); break;
    case DiscoverCollection::Nature: m_viewModel->navigateToWorkshopWithTag(QStringLiteral("Nature")); break;
    case DiscoverCollection::Abstract: m_viewModel->navigateToWorkshopWithTag(QStringLiteral("Abstract")); break;
    case DiscoverCollection::Landscape: m_viewModel->navigateToWorkshopWithTag(QStringLiteral("Landscape")); break;
    }
}

void DiscoverSectionView::selectCurrentModelItem() {
    m_list->clearSelection();
    if (!m_viewModel->selectedItem()) return;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(WorkshopItemRole).value<WorkshopItem>().publishedFileId ==
            m_viewModel->selectedItem()->publishedFileId) {
            m_list->setCurrentRow(i);
            return;
        }
    }
}

} // namespace Mirage

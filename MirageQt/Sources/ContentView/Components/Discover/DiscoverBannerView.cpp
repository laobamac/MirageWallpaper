#include "ContentView/Components/Discover/DiscoverBannerView.h"

#include <QEnterEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>

namespace Mirage {

DiscoverBannerView::DiscoverBannerView(WorkshopViewModel* viewModel,
                                       SteamWebAPI* api,
                                       QWidget* parent)
    : QWidget(parent)
    , m_viewModel(viewModel)
    , m_api(api) {
    setFixedHeight(200);
    setObjectName(QStringLiteral("discoverBanner"));
    setCursor(Qt::PointingHandCursor);
    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setScaledContents(false);
    m_caption = new QLabel(this);
    m_caption->setObjectName(QStringLiteral("bannerCaption"));
    m_caption->setWordWrap(true);
    m_dots = new QLabel(this);
    m_dots->setAlignment(Qt::AlignCenter);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 8);
    layout->setSpacing(2);
    layout->addWidget(m_image, 1);
    layout->addWidget(m_caption);
    layout->addWidget(m_dots);

    m_timer.setInterval(5000);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (m_viewModel->bannerItems().size() < 2) return;
        m_index = (m_index + 1) % m_viewModel->bannerItems().size();
        showCurrent();
    });
    connect(m_viewModel, &WorkshopViewModel::discoverChanged, this, &DiscoverBannerView::rebuild);
    connect(m_api, &SteamWebAPI::previewImageFinished, this,
            [this](const QUrl& url, const QByteArray& bytes, const QString&) {
        if (m_viewModel->bannerItems().isEmpty() || bytes.isEmpty()) return;
        const WorkshopItem& item = m_viewModel->bannerItems().at(qBound(0, m_index, m_viewModel->bannerItems().size() - 1));
        if (item.previewImageUrl != url) return;
        QPixmap pixmap;
        pixmap.loadFromData(bytes);
        if (!pixmap.isNull()) m_image->setPixmap(pixmap.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    });
    rebuild();
}

void DiscoverBannerView::rebuild() {
    m_index = qBound(0, m_index, qMax(0, m_viewModel->bannerItems().size() - 1));
    setVisible(!m_viewModel->bannerItems().isEmpty());
    showCurrent();
    if (m_viewModel->bannerItems().size() > 1) m_timer.start();
    else m_timer.stop();
}

void DiscoverBannerView::showCurrent() {
    if (m_viewModel->bannerItems().isEmpty()) return;
    const WorkshopItem& item = m_viewModel->bannerItems().at(m_index);
    m_image->setPixmap(QIcon::fromTheme(QStringLiteral("image-x-generic")).pixmap(52, 52));
    m_caption->setText(QStringLiteral("<b>%1</b><br>↓ %2    ◉ %3    ◆ %4")
                           .arg(item.title, item.formattedSubscriptions(), item.formattedViews(), item.displayTypeName()));
    QStringList dots;
    for (int i = 0; i < m_viewModel->bannerItems().size(); ++i) dots << (i == m_index ? QStringLiteral("━━━━") : QStringLiteral("━"));
    m_dots->setText(dots.join(QStringLiteral("  ")));
    if (item.previewImageUrl.isValid()) m_api->downloadPreviewImage(item.previewImageUrl);
}

void DiscoverBannerView::enterEvent(QEnterEvent* event) {
    m_timer.stop();
    QWidget::enterEvent(event);
}

void DiscoverBannerView::leaveEvent(QEvent* event) {
    if (m_viewModel->bannerItems().size() > 1) m_timer.start();
    QWidget::leaveEvent(event);
}

void DiscoverBannerView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !m_viewModel->bannerItems().isEmpty()) {
        m_viewModel->selectWorkshopItem(m_viewModel->bannerItems().at(m_index));
    }
    QWidget::mouseReleaseEvent(event);
}

} // namespace Mirage

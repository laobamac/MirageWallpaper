#include "ContentView/Components/WallpaperPreviewWidget.h"

#include <QComboBox>
#include <QDirIterator>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QVBoxLayout>

namespace Mirage {
namespace {

QPixmap placeholderPixmap(const QSize& size) {
    QPixmap pixmap(size);
    pixmap.fill(QColor(47, 43, 39));
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(240, 240, 240, 150));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("Wallpaper\nPreview"));
    return pixmap;
}

qint64 directorySize(const QString& path) {
    qint64 total = 0;
    QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

QString byteText(qint64 bytes) {
    const QStringList units = {QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB")};
    double value = bytes;
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', unit == 0 ? 0 : 1).arg(units.at(unit));
}

QLabel* iconLabel(const QString& iconName, int size, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setFixedSize(size, size);
    label->setAlignment(Qt::AlignCenter);
    label->setPixmap(QIcon::fromTheme(iconName).pixmap(size, size));
    return label;
}

QWidget* controlRow(const QString& iconName,
                    const QString& title,
                    QWidget* control,
                    QLabel* value,
                    QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    row->setFixedHeight(30);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);
    layout->addWidget(iconLabel(iconName, 18, row));
    layout->addWidget(new QLabel(title, row));
    layout->addStretch(1);
    layout->addWidget(control);
    if (value) {
        value->setFixedWidth(42);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(value);
    }
    return row;
}

QPushButton* iconButton(const QString& iconName, const QString& tooltip, QWidget* parent) {
    auto* button = new QPushButton(QIcon::fromTheme(iconName), QString(), parent);
    button->setToolTip(tooltip);
    button->setFixedWidth(36);
    return button;
}

} // namespace

WallpaperPreviewWidget::WallpaperPreviewWidget(FavoritesManager* favorites, QWidget* parent)
    : QWidget(parent)
    , m_favorites(favorites) {
    setObjectName(QStringLiteral("wallpaperPreviewPane"));
    setStyleSheet(QStringLiteral(
        "QWidget#wallpaperPreviewPane { background-color: #3d3832; }"
        "QLabel#previewImage { border: 4px solid #f4f2ef; background: #2f2b27; }"
        "QLabel#previewTitle { font-weight: 700; }"
        "QWidget#previewFooter { border-top: 1px solid #211f1c; }"));

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("previewImage"));
    m_preview->setFixedSize(280, 280);
    m_preview->setAlignment(Qt::AlignCenter);

    m_title = new QLabel(QStringLiteral("请选择一个有效的壁纸"), this);
    m_title->setObjectName(QStringLiteral("previewTitle"));
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(false);
    m_title->setMaximumWidth(240);
    m_title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_author = new QLabel(QStringLiteral("佚名作者"), this);
    m_author->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_author->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    m_favorite = new QPushButton(QStringLiteral("♡"), this);
    m_favorite->setToolTip(QStringLiteral("收藏"));
    m_favorite->setFixedSize(38, 32);
    m_favorite->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: 20px; color: #b4afa9; padding: 0; }"
        "QPushButton:hover { color: #ff466c; }"));

    m_typeSize = new QLabel(this);
    m_typeSize->setAlignment(Qt::AlignCenter);
    m_typeSize->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_typeSize->setFixedHeight(20);
    m_typeSize->setStyleSheet(QStringLiteral("font-size: 12px;"));
    m_dependency = new QLabel(this);
    m_dependency->setAlignment(Qt::AlignCenter);
    m_dependency->setWordWrap(true);
    m_dependency->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);
    m_dependency->setMaximumHeight(40);
    m_dependency->setStyleSheet(QStringLiteral("color: #aaa59f; font-size: 12px;"));

    m_tags = new QWidget(this);
    m_tags->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_tags->setFixedHeight(30);
    auto* tagLayout = new QHBoxLayout(m_tags);
    tagLayout->setContentsMargins(0, 0, 0, 0);
    tagLayout->setSpacing(5);

    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setValue(100);
    m_volume->setFixedWidth(100);
    m_volumeValue = new QLabel(QStringLiteral("100%"), this);

    m_speed = new QSlider(Qt::Horizontal, this);
    m_speed->setRange(0, 200);
    m_speed->setSingleStep(10);
    m_speed->setValue(100);
    m_speed->setFixedWidth(100);
    m_speedValue = new QLabel(QStringLiteral("1.0x"), this);

    m_fill = new QComboBox(this);
    m_fill->setFixedWidth(120);
    m_fill->addItem(QStringLiteral("填充"), QVariant::fromValue(int(FillMode::Cover)));
    m_fill->addItem(QStringLiteral("适应"), QVariant::fromValue(int(FillMode::Contain)));
    m_fill->addItem(QStringLiteral("拉伸"), QVariant::fromValue(int(FillMode::Stretch)));

    m_properties = new PropertyEditorWidget(this);

    auto* content = new QWidget(this);
    content->setMinimumWidth(0);
    content->setMaximumWidth(306);
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* body = new QVBoxLayout(content);
    body->setSpacing(16);
    body->setContentsMargins(12, 12, 12, 12);
    body->setAlignment(Qt::AlignTop);

    auto* centerPreviewWidget = new QWidget(content);
    centerPreviewWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    centerPreviewWidget->setFixedHeight(280);
    auto* centerPreview = new QHBoxLayout(centerPreviewWidget);
    centerPreview->setContentsMargins(0, 0, 0, 0);
    centerPreview->addStretch(1);
    centerPreview->addWidget(m_preview);
    centerPreview->addStretch(1);
    body->addWidget(centerPreviewWidget);

    auto* titleWidget = new QWidget(content);
    titleWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    titleWidget->setFixedHeight(28);
    auto* titleRow = new QHBoxLayout(titleWidget);
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->addStretch(1);
    titleRow->addWidget(m_title);
    titleRow->addWidget(iconLabel(QStringLiteral("document-edit"), 16, content));
    titleRow->addStretch(1);
    body->addWidget(titleWidget);

    auto* authorWidget = new QWidget(content);
    authorWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    authorWidget->setFixedHeight(32);
    auto* authorRow = new QHBoxLayout(authorWidget);
    authorRow->setContentsMargins(0, 0, 0, 0);
    authorRow->addStretch(1);
    authorRow->addWidget(iconLabel(QStringLiteral("user-identity"), 32, content));
    authorRow->addWidget(m_author);
    authorRow->addStretch(1);
    body->addWidget(authorWidget);

    auto* favoriteWidget = new QWidget(content);
    favoriteWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    favoriteWidget->setFixedHeight(32);
    auto* favoriteRow = new QHBoxLayout(favoriteWidget);
    favoriteRow->setContentsMargins(0, 0, 0, 0);
    favoriteRow->setSpacing(8);
    favoriteRow->addStretch(1);
    auto* stars = new QLabel(QStringLiteral("☆ ☆ ☆ ☆ ☆"), content);
    stars->setStyleSheet(QStringLiteral("font-size: 13px;"));
    favoriteRow->addWidget(stars);
    favoriteRow->addWidget(m_favorite);
    favoriteRow->addStretch(1);
    body->addWidget(favoriteWidget);
    body->addWidget(m_typeSize);
    body->addWidget(m_dependency);
    body->addWidget(m_tags);

    auto* unsubscribe = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Unsubscribe"), content);
    unsubscribe->setProperty("danger", true);
    unsubscribe->setEnabled(false);
    body->addWidget(unsubscribe);

    auto* commentWidget = new QWidget(content);
    commentWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    commentWidget->setFixedHeight(28);
    auto* commentRow = new QHBoxLayout(commentWidget);
    commentRow->setContentsMargins(0, 0, 0, 0);
    commentRow->setSpacing(3);
    auto* comment = new QPushButton(QIcon::fromTheme(QStringLiteral("mail-message-new")), QStringLiteral("Comment"), content);
    comment->setEnabled(false);
    auto* copy = iconButton(QStringLiteral("edit-copy"), QStringLiteral("复制"), content);
    auto* report = iconButton(QStringLiteral("dialog-warning"), QStringLiteral("举报"), content);
    copy->setEnabled(false);
    report->setEnabled(false);
    commentRow->addWidget(comment, 1);
    commentRow->addWidget(copy);
    commentRow->addWidget(report);
    body->addWidget(commentWidget);

    body->addWidget(sectionHeader(QStringLiteral("播放控制")));
    auto* controlsWidget = new QWidget(content);
    controlsWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);
    auto* controls = new QVBoxLayout(controlsWidget);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(16);
    controls->addWidget(controlRow(QStringLiteral("audio-volume-high"), QStringLiteral("音量"), m_volume, m_volumeValue, content));
    m_speedRow = controlRow(QStringLiteral("speedometer"), QStringLiteral("速度"), m_speed, m_speedValue, content);
    controls->addWidget(m_speedRow);
    m_fillRow = controlRow(QStringLiteral("view-fullscreen"), QStringLiteral("填充模式"), m_fill, nullptr, content);
    controls->addWidget(m_fillRow);
    body->addWidget(controlsWidget);

    body->addWidget(sectionHeader(QStringLiteral("壁纸属性")));
    m_properties->setMaximumWidth(282);
    body->addWidget(m_properties);

    body->addWidget(sectionHeader(QStringLiteral("壁纸")));
    auto* applyAll = new QPushButton(QIcon::fromTheme(QStringLiteral("view-grid")), QStringLiteral("覆盖到所有显示器"), content);
    applyAll->setProperty("accent", true);
    auto* stop = new QPushButton(QIcon::fromTheme(QStringLiteral("media-playback-stop")), QStringLiteral("停止壁纸"), content);
    body->addWidget(applyAll);
    body->addWidget(stop);

    body->addWidget(sectionHeader(QStringLiteral("预设")));
    auto* presetRowWidget = new QWidget(content);
    presetRowWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    presetRowWidget->setFixedHeight(28);
    auto* presetRow = new QHBoxLayout(presetRowWidget);
    presetRow->setContentsMargins(0, 0, 0, 0);
    presetRow->setSpacing(3);
    auto* importPreset = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")), QStringLiteral("导入"), content);
    auto* exportPreset = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), QStringLiteral("导出"), content);
    auto* resetPreset = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("重置为默认"), content);
    resetPreset->setProperty("danger", true);
    presetRow->addWidget(importPreset);
    presetRow->addWidget(exportPreset);
    body->addWidget(presetRowWidget);
    body->addWidget(resetPreset);
    body->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("previewFooter"));
    auto* closeRow = new QHBoxLayout(footer);
    closeRow->setContentsMargins(16, 16, 16, 16);
    closeRow->setSpacing(8);
    auto* confirm = new QPushButton(QStringLiteral("确认"), footer);
    auto* cancel = new QPushButton(QStringLiteral("取消"), footer);
    confirm->setProperty("accent", true);
    confirm->setFixedWidth(70);
    cancel->setFixedWidth(70);
    closeRow->addStretch(1);
    closeRow->addWidget(confirm);
    closeRow->addWidget(cancel);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(scroll, 1);
    layout->addWidget(footer);
    setMinimumWidth(320);
    setMaximumWidth(340);

    connect(m_favorite, &QPushButton::clicked, this, [this] { emit favoriteRequested(m_wallpaper); });
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_volumeValue->setText(QStringLiteral("%1%").arg(value));
        emit volumeChanged(value / 100.0);
    });
    connect(m_speed, &QSlider::valueChanged, this, [this](int value) {
        m_speedValue->setText(QStringLiteral("%1x").arg(value / 100.0, 0, 'f', 1));
        emit speedChanged(value / 100.0);
    });
    connect(m_fill, &QComboBox::currentIndexChanged, this, [this](int) {
        emit fillModeChanged(static_cast<FillMode>(m_fill->currentData().toInt()));
    });
    connect(m_properties, &PropertyEditorWidget::propertyChanged, this, &WallpaperPreviewWidget::propertyChanged);
    connect(applyAll, &QPushButton::clicked, this, [this] { emit applyAllRequested(m_wallpaper); });
    connect(stop, &QPushButton::clicked, this, &WallpaperPreviewWidget::stopRequested);
    connect(confirm, &QPushButton::clicked, this, &WallpaperPreviewWidget::closeRequested);
    connect(cancel, &QPushButton::clicked, this, &WallpaperPreviewWidget::closeRequested);
}

void WallpaperPreviewWidget::setWallpaper(const Wallpaper& wallpaper) {
    m_wallpaper = wallpaper;
    QPixmap pixmap;
    if (!wallpaper.project.preview.isEmpty()) pixmap.load(wallpaper.previewPath());
    if (pixmap.isNull()) pixmap = placeholderPixmap(QSize(272, 272));
    m_preview->setPixmap(pixmap.scaled(QSize(272, 272), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    m_title->setText(wallpaper.project.title.isEmpty() ? QStringLiteral("未命名") : wallpaper.project.title);
    const QString author = wallpaper.project.resolvedAuthor();
    m_author->setText(author.isEmpty() ? QStringLiteral("佚名作者") : author);
    m_typeSize->setText(QStringLiteral("%1  %2")
                            .arg(wallpaper.isPreset()
                                     ? QStringLiteral("预设 · %1").arg(wallpaperKindName(wallpaper.kind()))
                                     : wallpaper.project.type,
                                 byteText(directorySize(wallpaper.wallpaperDirectory))));
    m_dependency->setText(wallpaper.isPreset()
                              ? QStringLiteral("基础壁纸：%1").arg(wallpaper.presetDependency)
                              : QString());
    m_dependency->setVisible(wallpaper.isPreset());

    const bool favorite = m_favorites->contains(wallpaper.id());
    m_favorite->setText(favorite ? QStringLiteral("♥") : QStringLiteral("♡"));
    m_favorite->setToolTip(favorite ? QStringLiteral("取消收藏") : QStringLiteral("收藏"));
    m_favorite->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: 20px; color: %1; padding: 0; }"
        "QPushButton:hover { color: #ff466c; }").arg(favorite ? QStringLiteral("#ff466c") : QStringLiteral("#b4afa9")));

    m_speedRow->setVisible(wallpaper.kind() == WallpaperKind::Scene);
    m_fillRow->setVisible(wallpaper.kind() == WallpaperKind::Video);
    m_properties->setWallpaper(wallpaper);
    updateTags();
}

QWidget* WallpaperPreviewWidget::sectionHeader(const QString& title) {
    auto* row = new QWidget(this);
    row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    row->setFixedHeight(20);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addWidget(new QLabel(title, row));
    auto* line = new QFrame(row);
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background: #0a84ff; border: 0;"));
    layout->addWidget(line, 1);
    return row;
}

void WallpaperPreviewWidget::updateTags() {
    auto* layout = qobject_cast<QHBoxLayout*>(m_tags->layout());
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (m_wallpaper.project.tags.isEmpty()) {
        auto* label = new QLabel(QStringLiteral("暂无标签"), m_tags);
        label->setStyleSheet(QStringLiteral("color: #aaa59f;"));
        layout->addWidget(label);
    } else {
        const int visibleTagCount = qMin(3, m_wallpaper.project.tags.size());
        for (int i = 0; i < visibleTagCount; ++i) {
            auto* label = new QLabel(m_wallpaper.project.tags.at(i), m_tags);
            label->setStyleSheet(QStringLiteral(
                "padding: 4px 8px; background: #292622; border: 1px solid #88827a; border-radius: 12px;"));
            layout->addWidget(label);
        }
    }
    layout->addStretch(1);
}

} // namespace Mirage

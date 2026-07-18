#include "ContentView/Components/WallpaperPreviewWidget.h"

#include <QComboBox>
#include <QDirIterator>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QVBoxLayout>

namespace Mirage {
namespace {

QPixmap placeholderPixmap(const QSize& size) {
    QPixmap pixmap(size);
    pixmap.fill(QColor(43, 47, 54));
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(240, 240, 240, 180));
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
    const QStringList units = {"B", "KB", "MB", "GB"};
    double value = bytes;
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, unit == 0 ? 'f' : 'f', unit == 0 ? 0 : 1).arg(units.at(unit));
}

} // namespace

WallpaperPreviewWidget::WallpaperPreviewWidget(FavoritesManager* favorites, QWidget* parent)
    : QWidget(parent)
    , m_favorites(favorites) {
    m_preview = new QLabel(this);
    m_preview->setFixedSize(280, 280);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFrameShape(QFrame::Box);
    m_preview->setStyleSheet(QStringLiteral("border: 4px solid white; background: palette(base);"));

    m_title = new QLabel(QStringLiteral("请选择一个有效的壁纸"), this);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(true);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);

    m_author = new QLabel(QStringLiteral("佚名作者"), this);
    m_author->setAlignment(Qt::AlignCenter);

    m_favorite = new QPushButton(QIcon::fromTheme("emblem-favorite"), QStringLiteral("收藏"), this);
    m_typeSize = new QLabel(this);
    m_typeSize->setAlignment(Qt::AlignCenter);
    m_dependency = new QLabel(this);
    m_dependency->setAlignment(Qt::AlignCenter);
    m_dependency->setStyleSheet(QStringLiteral("color: palette(mid);"));

    m_tags = new QWidget(this);
    auto* tagLayout = new QHBoxLayout(m_tags);
    tagLayout->setContentsMargins(0, 0, 0, 0);

    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setValue(100);
    m_speed = new QSlider(Qt::Horizontal, this);
    m_speed->setRange(0, 200);
    m_speed->setValue(100);

    auto* fill = new QComboBox(this);
    fill->addItem(QStringLiteral("填充"), QVariant::fromValue(int(FillMode::Cover)));
    fill->addItem(QStringLiteral("适应"), QVariant::fromValue(int(FillMode::Contain)));
    fill->addItem(QStringLiteral("拉伸"), QVariant::fromValue(int(FillMode::Stretch)));

    m_properties = new PropertyEditorWidget(this);

    auto* content = new QWidget(this);
    auto* body = new QVBoxLayout(content);
    body->setSpacing(16);
    body->setContentsMargins(12, 12, 12, 12);

    auto* centerPreview = new QHBoxLayout;
    centerPreview->addStretch(1);
    centerPreview->addWidget(m_preview);
    centerPreview->addStretch(1);
    body->addLayout(centerPreview);
    body->addWidget(m_title);
    body->addWidget(m_author);

    auto* favRow = new QHBoxLayout;
    favRow->addStretch(1);
    favRow->addWidget(new QLabel(QStringLiteral("☆☆☆☆☆"), this));
    favRow->addWidget(m_favorite);
    favRow->addStretch(1);
    body->addLayout(favRow);
    body->addWidget(m_typeSize);
    body->addWidget(m_dependency);
    body->addWidget(m_tags);

    auto* unsub = new QPushButton(QIcon::fromTheme("edit-delete"), QStringLiteral("Unsubscribe"), this);
    unsub->setEnabled(false);
    body->addWidget(unsub);

    body->addWidget(sectionHeader(QStringLiteral("播放控制")));
    auto* controls = new QGridLayout;
    controls->addWidget(new QLabel(QStringLiteral("音量"), this), 0, 0);
    controls->addWidget(m_volume, 0, 1);
    controls->addWidget(new QLabel(QStringLiteral("速度"), this), 1, 0);
    controls->addWidget(m_speed, 1, 1);
    controls->addWidget(new QLabel(QStringLiteral("填充模式"), this), 2, 0);
    controls->addWidget(fill, 2, 1);
    body->addLayout(controls);

    body->addWidget(sectionHeader(QStringLiteral("壁纸属性")));
    body->addWidget(m_properties);

    body->addWidget(sectionHeader(QStringLiteral("壁纸")));
    auto* applyAll = new QPushButton(QIcon::fromTheme("view-grid"), QStringLiteral("覆盖到所有显示器"), this);
    auto* stop = new QPushButton(QIcon::fromTheme("media-playback-stop"), QStringLiteral("停止壁纸"), this);
    body->addWidget(applyAll);
    body->addWidget(stop);

    body->addWidget(sectionHeader(QStringLiteral("预设")));
    auto* presetRow = new QHBoxLayout;
    auto* importPreset = new QPushButton(QIcon::fromTheme("document-open"), QStringLiteral("导入"), this);
    auto* exportPreset = new QPushButton(QIcon::fromTheme("document-save"), QStringLiteral("导出"), this);
    auto* resetPreset = new QPushButton(QIcon::fromTheme("view-refresh"), QStringLiteral("重置为默认"), this);
    importPreset->setEnabled(false);
    exportPreset->setEnabled(false);
    resetPreset->setEnabled(false);
    presetRow->addWidget(importPreset);
    presetRow->addWidget(exportPreset);
    body->addLayout(presetRow);
    body->addWidget(resetPreset);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* ok = new QPushButton(QStringLiteral("确定"), this);
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    ok->setFixedWidth(64);
    cancel->setFixedWidth(64);
    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch(1);
    closeRow->addWidget(ok);
    closeRow->addWidget(cancel);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll, 1);
    layout->addLayout(closeRow);
    setMinimumWidth(300);
    setMaximumWidth(340);

    connect(m_favorite, &QPushButton::clicked, this, [this] { emit favoriteRequested(m_wallpaper); });
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) { emit volumeChanged(value / 100.0); });
    connect(m_speed, &QSlider::valueChanged, this, [this](int value) { emit speedChanged(value / 100.0); });
    connect(fill, &QComboBox::currentIndexChanged, this, [this, fill](int) {
        emit fillModeChanged(static_cast<FillMode>(fill->currentData().toInt()));
    });
    connect(m_properties, &PropertyEditorWidget::propertyChanged, this, &WallpaperPreviewWidget::propertyChanged);
    connect(applyAll, &QPushButton::clicked, this, [this] { emit applyAllRequested(m_wallpaper); });
    connect(stop, &QPushButton::clicked, this, &WallpaperPreviewWidget::stopRequested);
    connect(ok, &QPushButton::clicked, this, &WallpaperPreviewWidget::closeRequested);
    connect(cancel, &QPushButton::clicked, this, &WallpaperPreviewWidget::closeRequested);
}

void WallpaperPreviewWidget::setWallpaper(const Wallpaper& wallpaper) {
    m_wallpaper = wallpaper;
    QPixmap pixmap;
    if (!wallpaper.project.preview.isEmpty()) pixmap.load(wallpaper.previewPath());
    if (pixmap.isNull()) pixmap = placeholderPixmap(m_preview->size());
    m_preview->setPixmap(pixmap.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

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
    m_favorite->setText(m_favorites->contains(wallpaper.id()) ? QStringLiteral("取消收藏") : QStringLiteral("收藏"));
    m_properties->setWallpaper(wallpaper);
    updateTags();
}

QLabel* WallpaperPreviewWidget::sectionHeader(const QString& title) {
    auto* label = new QLabel(QStringLiteral("%1 ─────────────────").arg(title), this);
    label->setStyleSheet(QStringLiteral("color: palette(highlight);"));
    return label;
}

void WallpaperPreviewWidget::updateTags() {
    auto* layout = qobject_cast<QHBoxLayout*>(m_tags->layout());
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (m_wallpaper.project.tags.isEmpty()) {
        auto* label = new QLabel(QStringLiteral("暂无标签"), m_tags);
        label->setStyleSheet(QStringLiteral("color: palette(mid);"));
        layout->addWidget(label);
    } else {
        for (const QString& tag : m_wallpaper.project.tags) {
            auto* label = new QLabel(tag, m_tags);
            label->setStyleSheet(QStringLiteral("padding: 4px 8px; border: 1px solid palette(mid); border-radius: 10px;"));
            layout->addWidget(label);
        }
    }
    layout->addStretch(1);
}

} // namespace Mirage

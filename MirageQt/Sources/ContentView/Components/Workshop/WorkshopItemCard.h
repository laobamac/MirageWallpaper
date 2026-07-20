#pragma once

#include "Services/WorkshopModels.h"

#include <QStyledItemDelegate>

#include <optional>

class QListWidgetItem;

namespace Mirage {

enum WorkshopCardRole {
    WorkshopItemRole = Qt::UserRole + 1,
    WorkshopPreviewRole,
    WorkshopDownloadedRole,
    WorkshopPresetNeedsDependencyRole,
    WorkshopDownloadStateRole,
};

class WorkshopItemCard final : public QStyledItemDelegate {
public:
    explicit WorkshopItemCard(QObject* parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};

void setWorkshopCardData(QListWidgetItem* row,
                         const WorkshopItem& item,
                         bool downloaded,
                         bool presetNeedsDependency,
                         const std::optional<DownloadState>& downloadState);

} // namespace Mirage

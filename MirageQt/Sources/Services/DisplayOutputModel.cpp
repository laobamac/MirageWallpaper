#include "Services/DisplayOutputModel.h"

#include <QMetaType>

namespace Mirage {

DisplayOutputModel::DisplayOutputModel(QObject* parent)
    : QAbstractListModel(parent) {
    qRegisterMetaType<DisplayOutputSnapshot>();
}

int DisplayOutputModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_items.size();
}

QVariant DisplayOutputModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const Item& item = m_items.at(index.row());
    switch (role) {
    case StableIdRole: return item.output.stableId;
    case NameRole: return item.output.name;
    case LogicalXRole: return item.output.logicalX;
    case LogicalYRole: return item.output.logicalY;
    case LogicalWidthRole: return item.output.logicalWidth;
    case LogicalHeightRole: return item.output.logicalHeight;
    case Scale120Role: return item.output.scale120;
    case RefreshMhzRole: return item.output.refreshMhz;
    case RunningRole: return item.running;
    case BoundRole: return item.bound;
    case WallpaperIdRole: return item.wallpaperId;
    case WallpaperTitleRole: return item.wallpaperTitle;
    case WallpaperPreviewRole: return item.wallpaperPreview;
    default: return {};
    }
}

QHash<int, QByteArray> DisplayOutputModel::roleNames() const {
    return {
        {StableIdRole, "stableId"}, {NameRole, "name"},
        {LogicalXRole, "logicalX"}, {LogicalYRole, "logicalY"},
        {LogicalWidthRole, "logicalWidth"}, {LogicalHeightRole, "logicalHeight"},
        {Scale120Role, "scale120"}, {RefreshMhzRole, "refreshMhz"},
        {RunningRole, "running"}, {BoundRole, "bound"},
        {WallpaperIdRole, "wallpaperId"}, {WallpaperTitleRole, "wallpaperTitle"},
        {WallpaperPreviewRole, "wallpaperPreview"},
    };
}

QString DisplayOutputModel::stableIdAt(int row) const {
    if (row < 0 || row >= m_items.size()) return {};
    return m_items.at(row).output.stableId;
}

int DisplayOutputModel::indexForStableId(const QString& stableId) const {
    for (int index = 0; index < m_items.size(); ++index) {
        if (m_items.at(index).output.stableId == stableId) return index;
    }
    return -1;
}

void DisplayOutputModel::addOutput(const DisplayOutputSnapshot& output) {
    const int existing = indexForStableId(output.stableId);
    if (existing >= 0) {
        updateOutput(output);
        return;
    }
    const int insertion = m_items.size();
    beginInsertRows(QModelIndex(), insertion, insertion);
    Item item;
    item.output = output;
    m_items.insert(insertion, item);
    endInsertRows();
    emit countChanged();
    recomputeVirtualGeometry();
}

void DisplayOutputModel::updateOutput(const DisplayOutputSnapshot& output) {
    const int index = indexForStableId(output.stableId);
    if (index < 0) {
        addOutput(output);
        return;
    }
    m_items[index].output = output;
    emit dataChanged(this->index(index), this->index(index),
                     {LogicalXRole, LogicalYRole, LogicalWidthRole, LogicalHeightRole,
                      NameRole, Scale120Role, RefreshMhzRole});
    recomputeVirtualGeometry();
}

void DisplayOutputModel::removeOutput(const QString& stableId) {
    const int index = indexForStableId(stableId);
    if (index < 0) return;
    beginRemoveRows(QModelIndex(), index, index);
    m_items.removeAt(index);
    endRemoveRows();
    emit countChanged();
    recomputeVirtualGeometry();
}

void DisplayOutputModel::setWallpaperState(const QString& stableId, bool running, bool bound,
                                            const QString& wallpaperId,
                                            const QString& wallpaperTitle,
                                            const QUrl& wallpaperPreview) {
    const int index = indexForStableId(stableId);
    if (index < 0) return;
    Item& item = m_items[index];
    item.running = running;
    item.bound = bound;
    item.wallpaperId = wallpaperId;
    item.wallpaperTitle = wallpaperTitle;
    item.wallpaperPreview = wallpaperPreview;
    emit dataChanged(this->index(index), this->index(index),
                     {RunningRole, BoundRole, WallpaperIdRole, WallpaperTitleRole,
                      WallpaperPreviewRole});
}

void DisplayOutputModel::recomputeVirtualGeometry() {
    QRectF next;
    bool initialized = false;
    for (const Item& item : std::as_const(m_items)) {
        const QRectF geometry(item.output.logicalX, item.output.logicalY,
                              item.output.logicalWidth, item.output.logicalHeight);
        next = initialized ? next.united(geometry) : geometry;
        initialized = true;
    }
    if (m_virtualGeometry == next) return;
    m_virtualGeometry = next;
    emit virtualGeometryChanged();
}

} // namespace Mirage

#pragma once

#include <QAbstractListModel>
#include <QRectF>
#include <QString>
#include <QUrl>

namespace Mirage {

/* Value-owned copy of one mirage-display output. Broker callbacks construct
 * this on the dispatch thread and emit it by value; the GUI model never keeps
 * a pointer into broker-owned storage. */
struct DisplayOutputSnapshot {
    QString stableId;
    QString name;
    int logicalX = 0;
    int logicalY = 0;
    int logicalWidth = 0;
    int logicalHeight = 0;
    int scale120 = 120;
    int refreshMhz = 0;
};

class DisplayOutputModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QRectF virtualGeometry READ virtualGeometry NOTIFY virtualGeometryChanged)

public:
    enum Role {
        StableIdRole = Qt::UserRole + 1,
        NameRole,
        LogicalXRole,
        LogicalYRole,
        LogicalWidthRole,
        LogicalHeightRole,
        Scale120Role,
        RefreshMhzRole,
        RunningRole,
        BoundRole,
        WallpaperIdRole,
        WallpaperTitleRole,
        WallpaperPreviewRole,
    };
    Q_ENUM(Role)

    explicit DisplayOutputModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    QRectF virtualGeometry() const { return m_virtualGeometry; }
    Q_INVOKABLE QString stableIdAt(int row) const;

public slots:
    void addOutput(const Mirage::DisplayOutputSnapshot& output);
    void updateOutput(const Mirage::DisplayOutputSnapshot& output);
    void removeOutput(const QString& stableId);
    void setWallpaperState(const QString& stableId, bool running, bool bound,
                           const QString& wallpaperId, const QString& wallpaperTitle,
                           const QUrl& wallpaperPreview);

signals:
    void countChanged();
    void virtualGeometryChanged();

private:
    struct Item {
        DisplayOutputSnapshot output;
        bool running = false;
        bool bound = false;
        QString wallpaperId;
        QString wallpaperTitle;
        QUrl wallpaperPreview;
    };

    void recomputeVirtualGeometry();
    int indexForStableId(const QString& stableId) const;

    QVector<Item> m_items;
    QRectF m_virtualGeometry;
};

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::DisplayOutputSnapshot)

#pragma once

#include <QObject>
#include <QSet>
#include <QString>

namespace Mirage {

class FavoritesManager : public QObject {
    Q_OBJECT

public:
    explicit FavoritesManager(QObject* parent = nullptr);

    bool contains(const QString& wallpaperId) const;
    QSet<QString> favorites() const;

public slots:
    void setFavorite(const QString& wallpaperId, bool favorite);
    void toggle(const QString& wallpaperId);

signals:
    void changed();

private:
    void load();
    void save() const;

    QSet<QString> m_favorites;
};

} // namespace Mirage

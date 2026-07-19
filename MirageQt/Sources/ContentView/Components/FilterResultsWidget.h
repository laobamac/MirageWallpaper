#pragma once

#include <QCheckBox>
#include <QSet>
#include <QVector>
#include <QWidget>

namespace Mirage {

struct WallpaperFilterState {
    bool approvedOnly = false;
    bool favoritesOnly = false;
    bool mobileOnly = false;
    bool audioOnly = false;
    bool customizableOnly = false;
    QSet<QString> types = {QStringLiteral("scene"), QStringLiteral("video"), QStringLiteral("web"),
                           QStringLiteral("application"), QStringLiteral("preset")};
    QSet<QString> ratings = {QStringLiteral("Everyone"), QStringLiteral("Questionable"), QStringLiteral("Mature")};
    QSet<QString> sources = {QStringLiteral("workshop"), QStringLiteral("imported")};
    QSet<QString> tags = {QStringLiteral("abstract"), QStringLiteral("animal"), QStringLiteral("anime"),
                          QStringLiteral("cartoon"), QStringLiteral("cgi"), QStringLiteral("cyberpunk"),
                          QStringLiteral("fantasy"), QStringLiteral("game"), QStringLiteral("girls"),
                          QStringLiteral("guys"), QStringLiteral("landscape"), QStringLiteral("medieval"),
                          QStringLiteral("memes"), QStringLiteral("mmd"), QStringLiteral("music"),
                          QStringLiteral("nature"), QStringLiteral("pixelart"), QStringLiteral("relaxing"),
                          QStringLiteral("retro"), QStringLiteral("scifi"), QStringLiteral("sports"),
                          QStringLiteral("technology"), QStringLiteral("television"), QStringLiteral("vehicle"),
                          QStringLiteral("unspecified")};
};

class FilterResultsWidget : public QWidget {
    Q_OBJECT

public:
    explicit FilterResultsWidget(QWidget* parent = nullptr);
    WallpaperFilterState filterState() const;

signals:
    void filtersChanged();

private:
    QCheckBox* addCheck(const QString& text,
                        const QString& value,
                        bool checked,
                        QVector<QCheckBox*>& destination,
                        QWidget* parent);

    QVector<QCheckBox*> m_showOnly;
    QVector<QCheckBox*> m_types;
    QVector<QCheckBox*> m_ratings;
    QVector<QCheckBox*> m_sources;
    QVector<QCheckBox*> m_tags;
};

} // namespace Mirage

#pragma once

#include <QString>
#include <QStringList>

namespace Mirage {

class WEUrlSchemeHandler {
public:
    WEUrlSchemeHandler(QString wallpaperDirectory = {}, QStringList overlays = {});

    QString wallpaperDirectory() const;
    QStringList overlays() const;

private:
    QString m_wallpaperDirectory;
    QStringList m_overlays;
};

} // namespace Mirage

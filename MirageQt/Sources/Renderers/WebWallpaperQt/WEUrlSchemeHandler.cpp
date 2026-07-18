#include "Renderers/WebWallpaperQt/WEUrlSchemeHandler.h"

namespace Mirage {

WEUrlSchemeHandler::WEUrlSchemeHandler(QString wallpaperDirectory, QStringList overlays)
    : m_wallpaperDirectory(std::move(wallpaperDirectory))
    , m_overlays(std::move(overlays)) {
}

QString WEUrlSchemeHandler::wallpaperDirectory() const {
    return m_wallpaperDirectory;
}

QStringList WEUrlSchemeHandler::overlays() const {
    return m_overlays;
}

} // namespace Mirage

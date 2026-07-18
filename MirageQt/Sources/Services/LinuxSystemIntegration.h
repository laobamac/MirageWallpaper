#pragma once

#include <QString>

namespace Mirage {

class LinuxSystemIntegration {
public:
    static QString sessionType();
    static bool isX11Session();
    static bool isWaylandSession();
    static QString wallpaperUnsupportedReason();

    static bool setAutoStartEnabled(bool enabled, const QString& executablePath);
    static bool isOnBattery();
    static bool activeWindowIsFullscreen();
};

} // namespace Mirage

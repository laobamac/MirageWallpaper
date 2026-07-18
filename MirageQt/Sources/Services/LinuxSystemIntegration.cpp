#include "Services/LinuxSystemIntegration.h"

#include "Services/Paths.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QTextStream>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace Mirage {
namespace {

Atom atom(Display* display, const char* name) {
    return XInternAtom(display, name, False);
}

bool readWindowProperty(Display* display, Window window, Atom property, Atom type, QVector<unsigned long>* values) {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;

    const int status = XGetWindowProperty(display, window, property, 0, 1024, False, type,
                                          &actualType, &actualFormat, &itemCount, &bytesAfter, &data);
    if (status != Success || actualType == None || data == nullptr) {
        if (data) XFree(data);
        return false;
    }

    values->clear();
    if (actualFormat == 32) {
        auto* raw = reinterpret_cast<unsigned long*>(data);
        for (unsigned long i = 0; i < itemCount; ++i) values->push_back(raw[i]);
    }
    XFree(data);
    return true;
}

QString shellQuoted(const QString& value) {
    QString out = value;
    out.replace('\'', "'\\''");
    return "'" + out + "'";
}

} // namespace

QString LinuxSystemIntegration::sessionType() {
    const QString xdg = QProcessEnvironment::systemEnvironment()
                            .value(QStringLiteral("XDG_SESSION_TYPE"))
                            .trimmed()
                            .toLower();
    if (!xdg.isEmpty()) return xdg;
    if (qGuiApp) return QGuiApplication::platformName().toLower();
    return {};
}

bool LinuxSystemIntegration::isX11Session() {
    const QString session = sessionType();
    return session == "x11" || session == "xcb";
}

bool LinuxSystemIntegration::isWaylandSession() {
    const QString session = sessionType();
    return session == "wayland";
}

QString LinuxSystemIntegration::wallpaperUnsupportedReason() {
    if (isX11Session()) return {};
    if (isWaylandSession()) return QStringLiteral("当前 Wayland 会话不支持动态桌面壁纸，请在 X11 会话下应用壁纸。");
    return QStringLiteral("当前桌面会话不支持动态桌面壁纸，请在 X11 会话下应用壁纸。");
}

bool LinuxSystemIntegration::setAutoStartEnabled(bool enabled, const QString& executablePath) {
    const QString desktopFile = Paths::autostartDesktopFile();
    if (!enabled) {
        QFile::remove(desktopFile);
        return true;
    }

    QDir().mkpath(QFileInfo(desktopFile).absolutePath());
    QFile file(desktopFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;

    QTextStream stream(&file);
    stream << "[Desktop Entry]\n"
           << "Type=Application\n"
           << "Name=MirageQt\n"
           << "Exec=" << shellQuoted(executablePath) << "\n"
           << "Icon=preferences-desktop-wallpaper\n"
           << "Terminal=false\n"
           << "Categories=Utility;DesktopSettings;\n"
           << "StartupNotify=false\n";
    return true;
}

bool LinuxSystemIntegration::isOnBattery() {
    QDBusInterface upower(QStringLiteral("org.freedesktop.UPower"),
                          QStringLiteral("/org/freedesktop/UPower"),
                          QStringLiteral("org.freedesktop.DBus.Properties"),
                          QDBusConnection::systemBus());
    if (!upower.isValid()) return false;

    QDBusReply<QDBusVariant> reply = upower.call(QStringLiteral("Get"),
                                                 QStringLiteral("org.freedesktop.UPower"),
                                                 QStringLiteral("OnBattery"));
    return reply.isValid() && reply.value().variant().toBool();
}

bool LinuxSystemIntegration::activeWindowIsFullscreen() {
    if (!isX11Session()) return false;

    Display* display = XOpenDisplay(nullptr);
    if (!display) return false;

    const Window root = DefaultRootWindow(display);
    QVector<unsigned long> activeValues;
    if (!readWindowProperty(display, root, atom(display, "_NET_ACTIVE_WINDOW"), XA_WINDOW, &activeValues) ||
        activeValues.isEmpty()) {
        XCloseDisplay(display);
        return false;
    }

    const Window active = static_cast<Window>(activeValues.first());
    QVector<unsigned long> stateValues;
    const bool hasState = readWindowProperty(display, active, atom(display, "_NET_WM_STATE"), XA_ATOM, &stateValues);
    const Atom fullscreen = atom(display, "_NET_WM_STATE_FULLSCREEN");
    XCloseDisplay(display);

    if (!hasState) return false;
    for (unsigned long value : stateValues) {
        if (static_cast<Atom>(value) == fullscreen) return true;
    }
    return false;
}

} // namespace Mirage

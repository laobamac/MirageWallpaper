#include "Services/LinuxSystemIntegration.h"

#include "Services/Paths.h"

#include <QCoreApplication>
#include <QDebug>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QTextStream>

namespace Mirage {
namespace {

QString shellQuoted(const QString& value) {
    QString out = value;
    out.replace('\'', "'\\''");
    return "'" + out + "'";
}

} // namespace

QString LinuxSystemIntegration::sessionType() {
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#else
    const QString xdg = QProcessEnvironment::systemEnvironment()
                            .value(QStringLiteral("XDG_SESSION_TYPE"))
                            .trimmed()
                            .toLower();
    if (!xdg.isEmpty()) return xdg;
    if (qGuiApp) return QGuiApplication::platformName().toLower();
    return {};
#endif
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
#ifdef Q_OS_WIN
    return {};
#else
    // Wallpapers display through the mirage-display protocol, whose consumer is
    // the KDE Plasma wallpaper adapter — required on both X11 and Wayland.
    const bool plasma = qEnvironmentVariable("XDG_CURRENT_DESKTOP")
                            .contains(QStringLiteral("KDE"), Qt::CaseInsensitive);
    if ((isX11Session() || isWaylandSession()) && plasma) return {};
    if (isX11Session() || isWaylandSession()) {
        return QStringLiteral("当前桌面尚未安装受支持的 Mirage 显示适配器（需要 KDE Plasma）。");
    }
    return QStringLiteral("当前桌面会话不支持动态桌面壁纸，请在 X11 或 Wayland 会话下使用。");
#endif
}

bool LinuxSystemIntegration::setAutoStartEnabled(bool enabled, const QString& executablePath) {
    if (Paths::isTestMode()) return false;
#ifdef Q_OS_WIN
    HKEY key = nullptr;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        qWarning() << "[Autostart] Cannot open Run registry key:" << status;
        return false;
    }
    if (enabled) {
        const std::wstring command = (QLatin1Char('"') + QDir::toNativeSeparators(executablePath) + QLatin1Char('"')).toStdWString();
        status = RegSetValueExW(key, L"MirageQtUI", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"MirageQtUI");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) qWarning() << "[Autostart] Registry update failed:" << status;
    return status == ERROR_SUCCESS;
#else
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
#endif
}

} // namespace Mirage

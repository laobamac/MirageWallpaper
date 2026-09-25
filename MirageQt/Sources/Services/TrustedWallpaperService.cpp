#include "Services/TrustedWallpaperService.h"

#include <QSettings>
#include <QStringList>

namespace Mirage {

namespace {
// QSettings 中持久化信任壁纸的键。
constexpr char kTrustedWallpapersKey[] = "TrustedWallpapers";
} // namespace

TrustedWallpaperService::TrustedWallpaperService(QObject* parent)
    : QObject(parent) {}

bool TrustedWallpaperService::isTrusted(const QString& id) const {
    if (m_sessionTrusted.contains(id)) return true;
    return QSettings().value(QString::fromLatin1(kTrustedWallpapersKey))
        .toStringList().contains(id);
}

void TrustedWallpaperService::trust(const QString& id, bool persist) {
    m_sessionTrusted.insert(id);
    if (!persist) return;

    QSettings settings;
    QStringList trusted = settings.value(QString::fromLatin1(kTrustedWallpapersKey)).toStringList();
    if (!trusted.contains(id)) {
        trusted.append(id);
        settings.setValue(QString::fromLatin1(kTrustedWallpapersKey), trusted);
    }
}

void TrustedWallpaperService::clear(const QString& id) {
    m_sessionTrusted.remove(id);

    QSettings settings;
    QStringList trusted = settings.value(QString::fromLatin1(kTrustedWallpapersKey)).toStringList();
    if (trusted.removeAll(id) > 0) {
        settings.setValue(QString::fromLatin1(kTrustedWallpapersKey), trusted);
    }
}


} // namespace Mirage

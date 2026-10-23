#pragma once

#include <QObject>
#include <QSet>
#include <QString>

namespace Mirage {

// 网页壁纸信任管理：会话内信任 + QSettings 持久化信任。
// 对应 macOS 版 WallpaperViewModel 中信任相关的逻辑；本类只按 id 判断
// 信任与否，壁纸类型（kind == Web）校验由调用方（MirageController）负责。
class TrustedWallpaperService : public QObject {
    Q_OBJECT
public:
    explicit TrustedWallpaperService(QObject* parent = nullptr);

    // 该 id 是否已信任（会话内或持久化列表中）。
    bool isTrusted(const QString& id) const;
    // 标记信任；persist 为 true 时写入 QSettings（跨启动保留）。
    void trust(const QString& id, bool persist);
    // 取消信任（会话与持久化列表同时移除）。
    void clear(const QString& id);

private:
    QSet<QString> m_sessionTrusted;
};

} // namespace Mirage

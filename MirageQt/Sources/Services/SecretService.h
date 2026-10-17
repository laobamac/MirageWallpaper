#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace Mirage {

// SecretService — Linux 系统密钥环（org.freedesktop.secrets Secret Service
// D-Bus API）的 QtDBus 封装，替代 SteamServiceManager 的 QSettings 明文
// 存储（对齐 macOS Keychain 的 kSecClassGenericPassword 存储模型）。
//
// 存储模型：凭据按 attributes {service, account} 组织，
//   service = "cn.laobamac.Mirage.SteamService"（与 macOS keychainService 一致）
//   account = "refresh-token:<username>" / "guard-data:<username>"（用户名小写，
//             与 macOS refreshTokenAccount/guardDataAccount 一致）
// 值存于 org.freedesktop.Secret.Item（content_type "text/plain"）。
//
// 实现要点（Secret Service API Draft 0.2）：
//   - OpenSession("plain")：plain 会话不加密传输层；D-Bus 会话总线本机通信
//     已由总线 ACL 保护，凭据在服务端（如 gnome-keyring）内加密存储。
//   - CreateItem(replace=true)：按 attributes 覆盖同名项（对齐 keychain
//     同一 service+account 只保留一个值）。
//   - 集合锁定或服务不可用（available() 为 false）时返回失败，调用方回退
//     原存储（SteamServiceManager 保留 QSettings 兜底，保证登录/恢复功能
//     不因密钥环缺失而回归）。
//
// 线程安全：仅限创建它的线程（GUI 线程）使用；方法同步调用 D-Bus
// （本机调用开销可忽略，凭据读写仅登录/恢复/登出时各发生一次）。
class SecretService {
public:
    static const char* kServiceName;

    // 密钥环可用性：会话总线存在 org.freedesktop.secrets 服务且
    // OpenSession 可建立 plain 会话。
    static bool available();

    // 写入/覆盖指定账户的凭据；密钥环不可用或集合锁定失败时返回 false。
    // 参数：account 为 attributes 的 account 值（如 "refresh-token:laobamac"）。
    static bool write(const QString& account, const QByteArray& value);

    // 读取指定账户凭据；不存在或失败返回 false 且不修改 value。
    static bool read(const QString& account, QByteArray& value);

    // 删除指定账户凭据；不存在视为成功，失败返回 false。
    static bool remove(const QString& account);

    // 列出本应用已保存的全部账户（account 值），用于恢复会话时枚举
    // 已登录用户名（对齐 macOS 枚举 keychain 查找 savedUsername）。
    static QStringList accounts();

private:
    static QString defaultCollectionPath();
    static bool openSession(QDBusObjectPath& session);
    static bool findItem(const QString& collection, const QString& account,
                         const QDBusObjectPath& session, QDBusObjectPath& item);
    static QDBusArgument secretArgument(const QDBusObjectPath& session,
                                        const QByteArray& value);
    static QVariantMap attributesFor(const QString& account);
    static QStringList itemAccounts(const QString& collection,
                                    const QDBusObjectPath& session);
};

} // namespace Mirage

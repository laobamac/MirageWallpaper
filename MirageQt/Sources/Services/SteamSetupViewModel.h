#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace Mirage {

class SteamServiceManager;

// Steam 登录流程的视图状态层：对应 macOS SteamSetup/SteamSetupViewModel.swift。
// 持有登录状态的字符串缓存（供 QML 直接展示），监听 SteamServiceManager
// （SteamKit2 服务）的信号刷新缓存，并向其转发用户操作。由 MirageController
// 聚合为 QML 的 mirage.steam* 门面。
class SteamSetupViewModel : public QObject {
    Q_OBJECT
public:
    explicit SteamSetupViewModel(SteamServiceManager* service, QObject* parent = nullptr);

    QString loginState() const;
    QString loginMessage() const;
    QString guardType() const;
    QString qrChallengeUrl() const;
    bool hasSavedSession() const;

    Q_INVOKABLE void loginWithQR();
    Q_INVOKABLE void login(const QString& username, const QString& password);
    Q_INVOKABLE void submitGuardCode(const QString& code);
    Q_INVOKABLE void useSavedSession();
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void cancelPendingWork();
    Q_INVOKABLE void logout();
    Q_INVOKABLE void copyLoginLog();

signals:
    // 任一缓存状态变化（登录态/守卫/QR/会话）。
    void steamChanged();

private:
    void refreshFromService();

    SteamServiceManager* m_service;
    QString m_loginState = QStringLiteral("idle");
    QString m_loginMessage;
    QStringList m_loginLog;
};

} // namespace Mirage

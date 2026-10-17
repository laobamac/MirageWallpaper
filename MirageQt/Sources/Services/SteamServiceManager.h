#pragma once

#include "Services/WorkshopModels.h"
#include "Services/SecretService.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>

namespace Mirage {

// SteamServiceManager — MirageSteamService（C# SteamKit2）进程的 Qt 客户端。
//
// 对齐 macOS 的 SteamServiceManager.swift：用 QProcess 启动
// `dotnet MirageSteamService.dll`，通过 stdin/stdout 逐行 JSON IPC 通信。
// 服务端命令（Program.cs）：
//   hello / ping / restoreSession / loginPassword / loginQr /
//   submitChallenge / cancelLogin / logout / listSubscriptions /
//   checkSubscriptionStates / subscribe / unsubscribe / getComments /
//   postComment / download / cancelDownload
// 服务端事件（type 字段）：response（带 requestId 回显）、authState、
// downloadState。
//
// 所有命令异步：命令带自增 requestId，回调按 requestId 配对；事件
// （authState/downloadState）直接广播信号。进程意外退出自动重启并
// 尝试恢复会话（对齐上游 scheduleRestoreRetry 的语义）。
//
// 线程安全：本类及 QProcess 均运行在创建它的线程（GUI 线程）事件循环中；
// 调用方不得跨线程直接调用。

class SteamServiceManager : public QObject {
    Q_OBJECT

public:
    enum class ServiceState {
        Stopped,   // 进程未启动/已停止
        Starting,  // 进程启动中，等待 hello
        Connected, // hello 握手完成，可收发命令
        Failed,    // 无法启动（二进制缺失等）
    };
    Q_ENUM(ServiceState)

    enum class LoginState {
        Idle,            // 未登录
        LoggingIn,       // 认证中（connecting/authenticating）
        WaitingForQR,    // 等待手机扫码（qr 挑战 URL）
        WaitingForGuard, // 等待验证码（GuardType 细分）
        Success,         // 已登录
        Failed,          // 登录失败（lastError 有详情）
    };
    Q_ENUM(LoginState)

    enum class GuardType {
        None,
        Email,          // 邮箱验证码
        Mobile,         // 手机验证码
        MobileConfirm,  // 手机确认（登录提示）
    };
    Q_ENUM(GuardType)

    // 订阅分页结果（对齐 WorkshopModels.swift 的 WorkshopSubscriptionPage）。
    struct SubscriptionItem {
        QString publishedFileId;
        qint64 subscribedAt { 0 };  // Unix 秒
        qint64 updatedAt { 0 };     // Unix 秒
        QString contentHash;
        qint64 fileSize { 0 };
    };
    struct SubscriptionPage {
        int total { 0 };
        int startIndex { 0 };
        QList<SubscriptionItem> items;
    };

    using RequestCallback = std::function<void(bool success, const QString& message,
                                               const QString& errorCode, const QJsonObject& data)>;
    using DownloadCallback = std::function<void(const DownloadState&)>;
    explicit SteamServiceManager(QObject* parent = nullptr);
    ~SteamServiceManager() override;

    // 服务生命周期。
    void start();
    void stop();

    // 认证命令。
    void restoreSessionIfNeeded();
    void loginWithQR();
    void login(const QString& username, const QString& password);
    bool submitGuardCode(const QString& code);
    void cancelLogin();
    void logout();

    // 订阅命令。
    void fetchSubscriptions(int startIndex,
                            std::function<void(bool, const SubscriptionPage&, const QString&)> cb);
    void fetchSubscriptionStates(const QStringList& workshopIds,
                                 std::function<void(bool, const QHash<QString, bool>&, const QString&)> cb);
    void subscribe(const QString& workshopId, RequestCallback cb);
    void unsubscribe(const QString& workshopId, RequestCallback cb);

    // 下载命令。taskId 由调用方生成；进度经 downloadStateChanged 信号或回调。
    void downloadItem(const QString& workshopId, const QString& taskId,
                      const QString& outputRoot, DownloadCallback cb = nullptr);
    void cancelDownload(const QString& taskId);

    // 状态查询。
    bool isRunning() const { return m_process != nullptr; }
    bool isLoggedIn() const { return m_loggedIn; }
    QString accountName() const { return m_accountName; }
    QString steamId() const { return m_steamId; }
    LoginState loginState() const { return m_loginState; }
    QString qrChallengeUrl() const { return m_qrChallengeUrl; }
    GuardType guardType() const { return m_guardType; }
    QString lastError() const { return m_lastError; }
    bool hasSavedSession() const;

signals:
    void serviceStateChanged(SteamServiceManager::ServiceState state);
    void loginStateChanged();
    void authenticationChanged(bool loggedIn, const QString& accountName, const QString& steamId);
    void downloadStateChanged(const QString& taskId, DownloadStateKind kind,
                              qint64 receivedBytes, qint64 totalBytes, double bytesPerSecond,
                              const QString& outputPath, const QString& message);

private:
    void launchProcess();
    void handleStdout();
    void handleFinished(int exitCode, QProcess::ExitStatus status);
    void handleLine(const QJsonObject& event);
    void handleAuthEvent(const QJsonObject& event);
    void handleDownloadEvent(const QJsonObject& event);
    void handleResponse(const QJsonObject& event);
    void sendCommand(const QString& command, const QJsonObject& fields = QJsonObject(),
                     RequestCallback cb = nullptr);
    // 清除本机保存的 Steam 会话（refreshToken/guardData）。
    void clearSavedSession();
    // 会话凭据持久化：密钥环（org.freedesktop.secrets）优先，失败回退
    // QSettings（保持既有行为，不因密钥环缺失回归）。kind 为
    // "refresh-token"/"guard-data"（对齐 macOS keychain 账户前缀）。
    QString secretAccountKey(const QString& kind, const QString& username) const;
    bool secretStoreWrite(const QString& kind, const QString& username, const QString& value);
    QString secretStoreRead(const QString& kind, const QString& username) const;
    void secretStoreRemove(const QString& kind, const QString& username);
    // 枚举已保存会话的用户名（refresh-token 账户），密钥环与 QSettings 并集。
    QStringList savedSessionUsernames() const;
    QString locateServiceExecutable() const;
    QString persistPath(const QString& key) const;
    void scheduleRestart();

    QProcess* m_process { nullptr };
    QByteArray m_stdoutBuffer;
    quint64 m_nextRequestId { 1 };
    bool m_stopping { false };
    int m_restartCount { 0 };

    // requestId -> 回调；authState/downloadState 事件不带 requestId。
    QHash<QString, RequestCallback> m_pendingRequests;
    QHash<QString, DownloadCallback> m_downloadHandlers;

    // 认证状态（与上游 Swift 对齐）。
    bool m_loggedIn { false };
    QString m_accountName;
    QString m_steamId;
    LoginState m_loginState { LoginState::Idle };
    GuardType m_guardType { GuardType::None };
    QString m_qrChallengeUrl;
    QString m_lastError;
    bool m_restoringSession { false };
};

} // namespace Mirage

#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QString>

namespace Mirage {

enum class SteamCMDInstallState {
    Detecting,
    Found,
    NotFound,
    Downloading,
    Extracting,
    Initializing,
    Installed,
    Failed,
};

enum class SteamLoginState {
    Idle,
    LoggingIn,
    WaitingForGuard,
    Success,
    Failed,
};

enum class DownloadStateKind {
    Queued,
    Starting,
    Downloading,
    Validating,
    Completed,
    Failed,
    Cancelled,
};

struct DownloadState {
    DownloadStateKind kind = DownloadStateKind::Queued;
    double percent = -1.0;
    QString message;
};

class SteamCMDManager : public QObject {
    Q_OBJECT

public:
    explicit SteamCMDManager(QObject* parent = nullptr);

    QString steamCMDPath() const;
    QString savedUsername() const;
    bool isLoggedIn() const;
    QStringList diagnosticEvents() const;

    QString detectSteamCMD();
    QString steamCMDContentDirectory() const;
    QStringList steamCMDContentDirectories() const;
    QString diagnosticReport() const;

public slots:
    void installSteamCMD();
    void cancelInstallation();
    void login(const QString& username, const QString& password);
    void submitGuardCode(const QString& code);
    void cancelLogin();
    void logout();
    void downloadItem(const QString& workshopId, qint64 expectedFileSize = 0);
    void cancelDownload(const QString& workshopId);

signals:
    void installStateChanged(Mirage::SteamCMDInstallState state, double progress, const QString& message);
    void loginStateChanged(Mirage::SteamLoginState state, const QString& message);
    void downloadStateChanged(const QString& workshopId, Mirage::DownloadState state);
    void steamCMDPathChanged(const QString& path);
    void authenticationChanged(bool loggedIn, const QString& message);
    void diagnosticEvent(const QString& line);

private:
    void setSteamCMDPath(const QString& path);
    bool isUsableLauncher(const QString& path) const;
    bool isReadyLauncher(const QString& path) const;
    QString preferredLauncher(const QString& path) const;
    void record(const QString& category, const QString& message, const QStringList& secrets = {});
    QString redact(QString text, const QStringList& secrets = {}) const;
    QProcess* startSteamCMD(const QStringList& arguments, QObject* owner);
    void finishLoginProcess(int exitCode, QProcess::ExitStatus status, const QString& username, QString output);
    void publishDownloadState(const QString& workshopId, DownloadStateKind kind, double percent, const QString& message);

    QString m_steamCMDPath;
    QString m_savedUsername;
    bool m_loggedIn = false;
    bool m_installCancelled = false;
    QNetworkReply* m_installReply = nullptr;
    QProcess* m_installProcess = nullptr;
    QProcess* m_loginProcess = nullptr;
    QHash<QString, QProcess*> m_downloadProcesses;
    QSet<QString> m_cancelledDownloads;
    QStringList m_diagnostics;
    QNetworkAccessManager m_network;
};

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::SteamCMDInstallState)
Q_DECLARE_METATYPE(Mirage::SteamLoginState)
Q_DECLARE_METATYPE(Mirage::DownloadState)

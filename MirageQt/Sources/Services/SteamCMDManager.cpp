#include "Services/SteamCMDManager.h"

#include "Services/Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

namespace Mirage {
namespace {

const char* kBootstrapUrl = "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz";

QString statePath() {
    return Paths::configDir() + "/steamcmd.json";
}


QString markerPath() {
    return Paths::steamCMDDir() + "/.mirage-ready";
}

QString archivePath() {
    return Paths::steamCMDDir() + "/steamcmd_linux.tar.gz";
}

QString readTextFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll());
}

void writeState(const QString& path, const QString& username) {
    QSaveFile file(statePath());
    if (!file.open(QIODevice::WriteOnly)) return;
    QJsonObject object;
    object["path"] = path;
    object["username"] = username;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    file.commit();
}

QJsonObject readState() {
    QFile file(statePath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool containsAny(const QString& haystack, const QStringList& needles) {
    const QString lower = haystack.toLower();
    for (const QString& needle : needles) {
        if (lower.contains(needle.toLower())) return true;
    }
    return false;
}

} // namespace

SteamCMDManager::SteamCMDManager(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<Mirage::SteamCMDInstallState>();
    qRegisterMetaType<Mirage::SteamLoginState>();
    qRegisterMetaType<Mirage::DownloadState>();

    const QJsonObject state = readState();
    m_savedUsername = state.value("username").toString();
    const QString storedPath = state.value("path").toString();
    if (!storedPath.isEmpty() && isReadyLauncher(preferredLauncher(storedPath))) {
        m_steamCMDPath = preferredLauncher(storedPath);
    }
    m_loggedIn = !m_savedUsername.isEmpty() &&
                 (QFileInfo::exists(Paths::steamCMDDir() + "/config/config.vdf") ||
                  QFileInfo::exists(Paths::steamCMDDir() + "/steam/config/config.vdf"));
}

QString SteamCMDManager::steamCMDPath() const {
    return m_steamCMDPath;
}

QString SteamCMDManager::savedUsername() const {
    return m_savedUsername;
}

bool SteamCMDManager::isLoggedIn() const {
    return m_loggedIn;
}

QStringList SteamCMDManager::diagnosticEvents() const {
    return m_diagnostics;
}

QString SteamCMDManager::detectSteamCMD() {
    emit installStateChanged(SteamCMDInstallState::Detecting, 0.0, QStringLiteral("检测 SteamCMD"));

    if (!m_steamCMDPath.isEmpty() && isReadyLauncher(m_steamCMDPath)) {
        emit installStateChanged(SteamCMDInstallState::Found, 1.0, m_steamCMDPath);
        return m_steamCMDPath;
    }

    const QStringList candidates = {
        Paths::steamCMDDir() + "/steamcmd.sh",
        QStandardPaths::findExecutable("steamcmd"),
        QDir::homePath() + "/steamcmd/steamcmd.sh",
        "/usr/bin/steamcmd",
        "/usr/local/bin/steamcmd",
    };

    for (const QString& candidate : candidates) {
        if (candidate.isEmpty()) continue;
        const QString launcher = preferredLauncher(candidate);
        if (isReadyLauncher(launcher)) {
            setSteamCMDPath(launcher);
            emit installStateChanged(SteamCMDInstallState::Found, 1.0, launcher);
            return launcher;
        }
    }

    emit installStateChanged(SteamCMDInstallState::NotFound, 0.0, QStringLiteral("未找到 SteamCMD"));
    return {};
}

QString SteamCMDManager::steamCMDContentDirectory() const {
    return Paths::steamCMDDir() + "/steamapps/workshop/content/431960";
}

QStringList SteamCMDManager::steamCMDContentDirectories() const {
    return Paths::steamCMDContentDirs();
}

QString SteamCMDManager::diagnosticReport() const {
    QStringList lines;
    lines << QStringLiteral("MirageQt Steam 创意工坊支持报告（已脱敏）")
          << QStringLiteral("生成时间：%1").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
          << QStringLiteral("SteamCMD：%1").arg(m_steamCMDPath.isEmpty() ? QStringLiteral("未安装") : m_steamCMDPath)
          << QString();
    lines << m_diagnostics;
    return lines.join('\n');
}

void SteamCMDManager::installSteamCMD() {
    if (m_installReply || m_installProcess) {
        emit installStateChanged(SteamCMDInstallState::Failed, 0.0, QStringLiteral("SteamCMD 正在安装"));
        return;
    }

    m_installCancelled = false;
    QDir().mkpath(Paths::steamCMDDir());
    QFile::remove(markerPath());
    QFile::remove(archivePath());

    record(QStringLiteral("SteamCMD 安装"), QStringLiteral("开始下载 Linux SteamCMD bootstrap"));
    QNetworkRequest request(QUrl(QString::fromLatin1(kBootstrapUrl)));
    m_installReply = m_network.get(request);
    emit installStateChanged(SteamCMDInstallState::Downloading, 0.0, QStringLiteral("下载 SteamCMD"));

    connect(m_installReply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        const double progress = total > 0 ? double(received) / double(total) : 0.0;
        emit installStateChanged(SteamCMDInstallState::Downloading, progress, QStringLiteral("下载 SteamCMD"));
    });

    connect(m_installReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply* reply = m_installReply;
        m_installReply = nullptr;

        if (m_installCancelled) {
            reply->deleteLater();
            emit installStateChanged(SteamCMDInstallState::Failed, 0.0, QStringLiteral("SteamCMD 安装已取消"));
            return;
        }

        const QByteArray bytes = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        reply->deleteLater();
        if (!error.isEmpty() || bytes.isEmpty()) {
            record(QStringLiteral("SteamCMD 安装"), QStringLiteral("下载失败：%1").arg(error));
            emit installStateChanged(SteamCMDInstallState::Failed, 0.0, error.isEmpty() ? QStringLiteral("安装包为空") : error);
            return;
        }

        QFile archive(archivePath());
        if (!archive.open(QIODevice::WriteOnly | QIODevice::Truncate) || archive.write(bytes) != bytes.size()) {
            emit installStateChanged(SteamCMDInstallState::Failed, 0.0, QStringLiteral("写入安装包失败"));
            return;
        }
        archive.close();

        emit installStateChanged(SteamCMDInstallState::Extracting, 0.0, QStringLiteral("解压 SteamCMD"));
        auto* tar = new QProcess(this);
        m_installProcess = tar;
        tar->setProgram("/usr/bin/tar");
        tar->setArguments({"-xzf", archivePath(), "-C", Paths::steamCMDDir()});
        connect(tar, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, tar](int exitCode, QProcess::ExitStatus status) {
            m_installProcess = nullptr;
            const QString stderrText = QString::fromUtf8(tar->readAllStandardError());
            tar->deleteLater();
            if (m_installCancelled) {
                emit installStateChanged(SteamCMDInstallState::Failed, 0.0, QStringLiteral("SteamCMD 安装已取消"));
                return;
            }
            if (status != QProcess::NormalExit || exitCode != 0) {
                emit installStateChanged(SteamCMDInstallState::Failed, 0.0, QStringLiteral("解压失败：%1").arg(stderrText.left(200)));
                return;
            }

            const QString launcher = Paths::steamCMDDir() + "/steamcmd.sh";
            QFile::setPermissions(launcher, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                            QFileDevice::ReadGroup | QFileDevice::ExeGroup);
            emit installStateChanged(SteamCMDInstallState::Initializing, 0.0, QStringLiteral("初始化 SteamCMD"));

            auto* init = new QProcess(this);
            m_installProcess = init;
            init->setProgram(launcher);
            init->setWorkingDirectory(Paths::steamCMDDir());
            init->setArguments({"+quit"});
            connect(init, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, init, launcher](int initCode, QProcess::ExitStatus initStatus) {
                m_installProcess = nullptr;
                const QString stderrText = QString::fromUtf8(init->readAllStandardError());
                const QString stdoutText = QString::fromUtf8(init->readAllStandardOutput());
                init->deleteLater();
                record(QStringLiteral("SteamCMD 安装"), redact(stdoutText + "\n" + stderrText));
                if (initStatus != QProcess::NormalExit || initCode != 0) {
                    emit installStateChanged(SteamCMDInstallState::Failed, 0.0, QStringLiteral("SteamCMD 初始化失败"));
                    return;
                }
                QFile marker(markerPath());
                if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) marker.write("ready");
                setSteamCMDPath(launcher);
                emit installStateChanged(SteamCMDInstallState::Installed, 1.0, launcher);
            });
            init->start();
        });
        tar->start();
    });
}

void SteamCMDManager::cancelInstallation() {
    m_installCancelled = true;
    if (m_installReply) m_installReply->abort();
    if (m_installProcess) m_installProcess->kill();
}

void SteamCMDManager::login(const QString& username, const QString& password) {
    if (m_steamCMDPath.isEmpty() && detectSteamCMD().isEmpty()) {
        emit loginStateChanged(SteamLoginState::Failed, QStringLiteral("SteamCMD 未安装"));
        return;
    }
    if (m_loginProcess && m_loginProcess->state() != QProcess::NotRunning) {
        emit loginStateChanged(SteamLoginState::Failed, QStringLiteral("SteamCMD 正在登录"));
        return;
    }

    m_loginProcess = startSteamCMD({"+login", username, "+quit"}, this);
    if (!m_loginProcess) {
        emit loginStateChanged(SteamLoginState::Failed, QStringLiteral("启动 SteamCMD 失败"));
        return;
    }

    QString* output = new QString;
    bool* passwordSent = new bool(false);
    emit loginStateChanged(SteamLoginState::LoggingIn, QStringLiteral("正在登录 Steam"));

    connect(m_loginProcess, &QProcess::readyReadStandardOutput, this, [this, username, password, output, passwordSent] {
        const QString chunk = QString::fromUtf8(m_loginProcess->readAllStandardOutput());
        *output += chunk;
        record(QStringLiteral("Steam 登录"), chunk, {username, password});
        if (!*passwordSent && containsAny(*output, {"password", "passwort", "密码"})) {
            m_loginProcess->write((password + "\n").toUtf8());
            *passwordSent = true;
        }
        if (containsAny(*output, {"steam guard", "two-factor", "authenticator", "令牌", "验证码"})) {
            emit loginStateChanged(SteamLoginState::WaitingForGuard, QStringLiteral("请输入 Steam Guard 验证码"));
        }
    });
    connect(m_loginProcess, &QProcess::readyReadStandardError, this, [this, username, password, output] {
        const QString chunk = QString::fromUtf8(m_loginProcess->readAllStandardError());
        *output += chunk;
        record(QStringLiteral("Steam 登录"), chunk, {username, password});
    });
    connect(m_loginProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, username, output, passwordSent](int exitCode, QProcess::ExitStatus status) {
                finishLoginProcess(exitCode, status, username, *output);
                delete output;
                delete passwordSent;
            });
}

void SteamCMDManager::submitGuardCode(const QString& code) {
    if (m_loginProcess && m_loginProcess->state() != QProcess::NotRunning) {
        m_loginProcess->write((code.trimmed() + "\n").toUtf8());
        record(QStringLiteral("Steam 登录"), QStringLiteral("已提交 Steam Guard 验证码"));
    }
}

void SteamCMDManager::cancelLogin() {
    if (m_loginProcess) m_loginProcess->kill();
}

void SteamCMDManager::logout() {
    m_savedUsername.clear();
    m_loggedIn = false;
    QDir(Paths::steamCMDDir() + "/config").removeRecursively();
    QDir(Paths::steamCMDDir() + "/steam/config").removeRecursively();
    writeState(m_steamCMDPath, {});
    emit authenticationChanged(false, QStringLiteral("未登录"));
}

void SteamCMDManager::downloadItem(const QString& workshopId, qint64 expectedFileSize) {
    Q_UNUSED(expectedFileSize)
    if (workshopId.isEmpty()) return;
    if (m_steamCMDPath.isEmpty() && detectSteamCMD().isEmpty()) {
        publishDownloadState(workshopId, DownloadStateKind::Failed, -1.0, QStringLiteral("SteamCMD 未安装"));
        return;
    }
    if (!m_loggedIn || m_savedUsername.isEmpty()) {
        publishDownloadState(workshopId, DownloadStateKind::Failed, -1.0, QStringLiteral("Steam 会话未验证"));
        return;
    }
    if (m_downloadProcesses.contains(workshopId)) {
        publishDownloadState(workshopId, DownloadStateKind::Failed, -1.0, QStringLiteral("该项目已在下载"));
        return;
    }

    auto* process = startSteamCMD({"+login", m_savedUsername,
                                   "+workshop_download_item", "431960", workshopId,
                                   "+quit"},
                                  this);
    if (!process) {
        publishDownloadState(workshopId, DownloadStateKind::Failed, -1.0, QStringLiteral("启动 SteamCMD 失败"));
        return;
    }

    m_downloadProcesses.insert(workshopId, process);
    publishDownloadState(workshopId, DownloadStateKind::Starting, -1.0, QStringLiteral("开始下载"));

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, workshopId] {
        const QString chunk = QString::fromUtf8(process->readAllStandardOutput());
        record(QStringLiteral("创意工坊下载"), chunk, {m_savedUsername});
        const QRegularExpression percentRe(QStringLiteral("(\\d+(?:\\.\\d+)?)\\s*%"));
        const auto match = percentRe.match(chunk);
        if (match.hasMatch()) {
            publishDownloadState(workshopId, DownloadStateKind::Downloading, match.captured(1).toDouble() / 100.0, QStringLiteral("下载中"));
        } else if (chunk.contains("Validating", Qt::CaseInsensitive)) {
            publishDownloadState(workshopId, DownloadStateKind::Validating, -1.0, QStringLiteral("校验中"));
        }
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        record(QStringLiteral("创意工坊下载"), QString::fromUtf8(process->readAllStandardError()), {m_savedUsername});
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, workshopId](int exitCode, QProcess::ExitStatus status) {
                m_downloadProcesses.remove(workshopId);
                process->deleteLater();
                if (m_cancelledDownloads.remove(workshopId)) {
                    publishDownloadState(workshopId, DownloadStateKind::Cancelled, -1.0, QStringLiteral("已取消"));
                } else if (status == QProcess::NormalExit && exitCode == 0) {
                    publishDownloadState(workshopId, DownloadStateKind::Completed, 1.0, QStringLiteral("下载完成"));
                } else {
                    publishDownloadState(workshopId, DownloadStateKind::Failed, -1.0, QStringLiteral("下载失败"));
                }
            });
}

void SteamCMDManager::cancelDownload(const QString& workshopId) {
    if (QProcess* process = m_downloadProcesses.value(workshopId)) {
        m_cancelledDownloads.insert(workshopId);
        process->kill();
    }
}

void SteamCMDManager::setSteamCMDPath(const QString& path) {
    m_steamCMDPath = QDir::cleanPath(path);
    writeState(m_steamCMDPath, m_savedUsername);
    emit steamCMDPathChanged(m_steamCMDPath);
}

bool SteamCMDManager::isUsableLauncher(const QString& path) const {
    const QFileInfo info(path);
    return info.exists() && (info.isExecutable() || path.endsWith(".sh"));
}

bool SteamCMDManager::isReadyLauncher(const QString& path) const {
    if (!isUsableLauncher(path)) return false;
    const QString clean = QDir::cleanPath(path);
    const QString managedRoot = QDir::cleanPath(Paths::steamCMDDir()) + "/";
    return !clean.startsWith(managedRoot) || QFileInfo::exists(markerPath());
}

QString SteamCMDManager::preferredLauncher(const QString& path) const {
    if (QFileInfo(path).fileName() == "steamcmd") {
        const QString script = QFileInfo(path).absolutePath() + "/steamcmd.sh";
        if (QFileInfo::exists(script)) return script;
    }
    return path;
}

void SteamCMDManager::record(const QString& category, const QString& message, const QStringList& secrets) {
    const QString safe = redact(message.trimmed(), secrets);
    if (safe.isEmpty()) return;
    const QString line = QStringLiteral("[%1] [%2] %3")
                             .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), category, safe);
    m_diagnostics << line;
    while (m_diagnostics.size() > 500) m_diagnostics.removeFirst();
    emit diagnosticEvent(line);
}

QString SteamCMDManager::redact(QString text, const QStringList& secrets) const {
    for (const QString& secret : secrets) {
        if (!secret.isEmpty()) text.replace(secret, QStringLiteral("[已隐藏]"));
    }
    const QVector<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral("(?i)(key|api[_-]?key|token|access[_-]?token|refresh[_-]?token|password)\\s*[=:]\\s*[^\\s&]+")),
        QRegularExpression(QStringLiteral("(?i)([?&](?:key|token|access_token|password)=)[^&\\s]+")),
    };
    for (const auto& re : patterns) {
        text.replace(re, QStringLiteral("\\1[已隐藏]"));
    }
    return text;
}

QProcess* SteamCMDManager::startSteamCMD(const QStringList& arguments, QObject* owner) {
    if (m_steamCMDPath.isEmpty()) return nullptr;
    auto* process = new QProcess(owner);
    process->setProgram(m_steamCMDPath);
    process->setArguments(arguments);
    process->setWorkingDirectory(Paths::steamCMDDir());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("HOME", Paths::steamCMDDir() + "/home");
    env.insert("STEAMEXE", m_steamCMDPath);
    process->setProcessEnvironment(env);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start();
    if (!process->waitForStarted(5000)) {
        process->deleteLater();
        return nullptr;
    }
    return process;
}

void SteamCMDManager::finishLoginProcess(int exitCode, QProcess::ExitStatus status, const QString& username, QString output) {
    if (m_loginProcess) {
        m_loginProcess->deleteLater();
        m_loginProcess = nullptr;
    }

    const bool success = status == QProcess::NormalExit &&
                         exitCode == 0 &&
                         (containsAny(output, {"success", "waiting for user info...ok", "logged in ok"}) ||
                          !containsAny(output, {"login failure", "failed", "incorrect"}));
    if (success) {
        m_savedUsername = username;
        m_loggedIn = true;
        writeState(m_steamCMDPath, m_savedUsername);
        emit authenticationChanged(true, QStringLiteral("已登录 %1").arg(username));
        emit loginStateChanged(SteamLoginState::Success, QStringLiteral("登录成功"));
    } else {
        m_loggedIn = false;
        emit authenticationChanged(false, QStringLiteral("登录失败"));
        emit loginStateChanged(SteamLoginState::Failed, QStringLiteral("登录失败或需要重新验证"));
    }
}

void SteamCMDManager::publishDownloadState(const QString& workshopId, DownloadStateKind kind, double percent, const QString& message) {
    DownloadState state;
    state.kind = kind;
    state.percent = percent;
    state.message = message;
    emit downloadStateChanged(workshopId, state);
}

} // namespace Mirage

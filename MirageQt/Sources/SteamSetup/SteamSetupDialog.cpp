#include "SteamSetup/SteamSetupDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace Mirage {

SteamSetupDialog::SteamSetupDialog(SteamCMDManager* steamCMD, QWidget* parent)
    : QDialog(parent)
    , m_steamCMD(steamCMD) {
    setWindowTitle(QStringLiteral("Steam 设置"));
    resize(560, 640);

    auto* detect = new QPushButton(QIcon::fromTheme("system-search"), QStringLiteral("检测 SteamCMD"), this);
    auto* install = new QPushButton(QIcon::fromTheme("go-down"), QStringLiteral("安装 SteamCMD"), this);
    auto* cancelInstall = new QPushButton(QIcon::fromTheme("process-stop"), QStringLiteral("取消安装"), this);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);

    auto* cmdRow = new QHBoxLayout;
    cmdRow->addWidget(detect);
    cmdRow->addWidget(install);
    cmdRow->addWidget(cancelInstall);

    m_username = new QLineEdit(this);
    m_username->setText(m_steamCMD->savedUsername());
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_guard = new QLineEdit(this);
    m_guard->setPlaceholderText(QStringLiteral("Steam Guard 验证码"));
    auto* login = new QPushButton(QIcon::fromTheme("dialog-password"), QStringLiteral("登录"), this);
    auto* submitGuard = new QPushButton(QIcon::fromTheme("dialog-ok"), QStringLiteral("验证"), this);
    auto* logout = new QPushButton(QIcon::fromTheme("system-log-out"), QStringLiteral("退出登录"), this);

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("账号"), m_username);
    form->addRow(QStringLiteral("密码"), m_password);
    form->addRow(QStringLiteral("Steam Guard"), m_guard);

    auto* loginRow = new QHBoxLayout;
    loginRow->addWidget(login);
    loginRow->addWidget(submitGuard);
    loginRow->addWidget(logout);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);

    auto* closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(cmdRow);
    layout->addWidget(m_progress);
    layout->addLayout(form);
    layout->addLayout(loginRow);
    layout->addWidget(m_log, 1);
    layout->addWidget(closeButtons);

    connect(detect, &QPushButton::clicked, this, [this] {
        const QString path = m_steamCMD->detectSteamCMD();
        m_log->appendPlainText(path.isEmpty() ? QStringLiteral("未找到 SteamCMD") : QStringLiteral("找到：%1").arg(path));
    });
    connect(install, &QPushButton::clicked, m_steamCMD, &SteamCMDManager::installSteamCMD);
    connect(cancelInstall, &QPushButton::clicked, m_steamCMD, &SteamCMDManager::cancelInstallation);
    connect(login, &QPushButton::clicked, this, [this] {
        m_steamCMD->login(m_username->text(), m_password->text());
    });
    connect(submitGuard, &QPushButton::clicked, this, [this] {
        m_steamCMD->submitGuardCode(m_guard->text());
    });
    connect(logout, &QPushButton::clicked, m_steamCMD, &SteamCMDManager::logout);
    connect(closeButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_steamCMD, &SteamCMDManager::installStateChanged, this, [this](SteamCMDInstallState, double progress, const QString& message) {
        m_progress->setValue(qRound(progress * 100.0));
        m_log->appendPlainText(message);
    });
    connect(m_steamCMD, &SteamCMDManager::loginStateChanged, this, [this](SteamLoginState, const QString& message) {
        m_log->appendPlainText(message);
    });
    connect(m_steamCMD, &SteamCMDManager::diagnosticEvent, this, [this](const QString& line) {
        m_log->appendPlainText(line);
    });
}

} // namespace Mirage

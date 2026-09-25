#include "Services/SteamSetupViewModel.h"

#include "Services/SteamServiceManager.h"

#include <QClipboard>
#include <QGuiApplication>

namespace Mirage {

namespace {

QString loginStateKey(SteamServiceManager::LoginState state) {
    switch (state) {
    case SteamServiceManager::LoginState::Idle: return QStringLiteral("idle");
    case SteamServiceManager::LoginState::LoggingIn: return QStringLiteral("loggingIn");
    case SteamServiceManager::LoginState::WaitingForQR: return QStringLiteral("waitingForQR");
    case SteamServiceManager::LoginState::WaitingForGuard: return QStringLiteral("waitingForGuard");
    case SteamServiceManager::LoginState::Success: return QStringLiteral("success");
    case SteamServiceManager::LoginState::Failed: return QStringLiteral("failed");
    }
    return {};
}

QString guardTypeKey(SteamServiceManager::GuardType type) {
    switch (type) {
    case SteamServiceManager::GuardType::None: return QStringLiteral("");
    case SteamServiceManager::GuardType::Email: return QStringLiteral("email");
    case SteamServiceManager::GuardType::Mobile: return QStringLiteral("mobile");
    case SteamServiceManager::GuardType::MobileConfirm: return QStringLiteral("mobileConfirm");
    }
    return {};
}

} // namespace

SteamSetupViewModel::SteamSetupViewModel(SteamServiceManager* service, QObject* parent)
    : QObject(parent)
    , m_service(service) {
    connect(m_service, &SteamServiceManager::loginStateChanged, this,
            [this] {
                refreshFromService();
                emit steamChanged();
            });
    connect(m_service, &SteamServiceManager::authenticationChanged, this,
            [this](bool, const QString&, const QString&) { emit steamChanged(); });
    connect(m_service, &SteamServiceManager::serviceStateChanged, this,
            [this](SteamServiceManager::ServiceState) { emit steamChanged(); });
}

void SteamSetupViewModel::refreshFromService() {
    m_loginState = loginStateKey(m_service->loginState());
    m_loginMessage = m_service->lastError();
}

QString SteamSetupViewModel::loginState() const { return m_loginState; }
QString SteamSetupViewModel::loginMessage() const { return m_loginMessage; }
QString SteamSetupViewModel::guardType() const { return guardTypeKey(m_service->guardType()); }
QString SteamSetupViewModel::qrChallengeUrl() const { return m_service->qrChallengeUrl(); }
bool SteamSetupViewModel::hasSavedSession() const { return m_service->hasSavedSession(); }

void SteamSetupViewModel::loginWithQR() {
    m_service->loginWithQR();
}

void SteamSetupViewModel::login(const QString& username, const QString& password) {
    m_loginLog.clear();
    emit steamChanged();
    m_service->login(username, password);
}

void SteamSetupViewModel::submitGuardCode(const QString& code) {
    m_service->submitGuardCode(code);
}

void SteamSetupViewModel::useSavedSession() {
    m_service->restoreSessionIfNeeded();
}

void SteamSetupViewModel::cancelLogin() {
    m_service->cancelLogin();
}

void SteamSetupViewModel::cancelPendingWork() {
    m_service->cancelLogin();
}

void SteamSetupViewModel::logout() {
    m_service->logout();
    m_loginState = QStringLiteral("idle");
    m_loginMessage = QStringLiteral("未登录");
    emit steamChanged();
}

void SteamSetupViewModel::copyLoginLog() {
    QGuiApplication::clipboard()->setText(m_loginLog.join(QStringLiteral("\n")));
}

} // namespace Mirage

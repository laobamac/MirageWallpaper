#include "ControlChannel.h"

#include <QJsonDocument>
#include <QSocketNotifier>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

VRControlChannel::VRControlChannel(CommandHandler commandHandler, CloseHandler closeHandler,
                                   QObject* parent)
    : QObject(parent)
    , m_commandHandler(std::move(commandHandler))
    , m_closeHandler(std::move(closeHandler)) {
}

VRControlChannel::~VRControlChannel() {
    if (m_originalFlags >= 0) fcntl(STDIN_FILENO, F_SETFL, m_originalFlags);
}

bool VRControlChannel::start(QString* error) {
    if (m_notifier) return true;

    m_originalFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (m_originalFlags < 0 || fcntl(STDIN_FILENO, F_SETFL, m_originalFlags | O_NONBLOCK) < 0) {
        if (error) *error = QStringLiteral("cannot configure stdin control channel");
        return false;
    }

    m_notifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { readAvailable(); });
    return true;
}

void VRControlChannel::readAvailable() {
    char chunk[4096];
    for (;;) {
        const ssize_t count = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count > 0) {
            m_buffer.append(chunk, static_cast<qsizetype>(count));
            for (;;) {
                const qsizetype newline = m_buffer.indexOf('\n');
                if (newline < 0) break;
                const QByteArray line = m_buffer.left(newline);
                m_buffer.remove(0, newline + 1);
                handleLine(line);
                if (m_finished) return;
            }
            continue;
        }
        if (count == 0) {
            finish();
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        finish();
        return;
    }
}

void VRControlChannel::handleLine(const QByteArray& line) {
    if (line.trimmed().isEmpty()) return;
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) return;

    const QJsonObject command = document.object();
    const QString name = command.value(QStringLiteral("cmd")).toString();
    if (name.isEmpty()) return;
    if (name == QStringLiteral("quit")) {
        finish();
        return;
    }
    if (m_commandHandler) m_commandHandler(command);
}

void VRControlChannel::finish() {
    if (m_finished) return;
    m_finished = true;
    if (m_notifier) m_notifier->setEnabled(false);
    if (m_closeHandler) m_closeHandler();
}

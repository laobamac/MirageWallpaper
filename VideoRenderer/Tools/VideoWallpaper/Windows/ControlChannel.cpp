#include "ControlChannel.h"

#include <QJsonDocument>
#include <QMetaObject>
#include <algorithm>

VRControlChannel::VRControlChannel(CommandHandler command, CloseHandler close, QObject* parent)
    : QObject(parent), m_command(std::move(command)), m_close(std::move(close)) {}
VRControlChannel::~VRControlChannel() { stop(); }

bool VRControlChannel::start(QString* error) {
    if (m_thread.joinable()) return true;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    m_type = GetFileType(input);
    if (!input || input == INVALID_HANDLE_VALUE || (m_type != FILE_TYPE_PIPE && m_type != FILE_TYPE_DISK)) {
        if (error) *error = QStringLiteral("stdin control requires a pipe or redirected file");
        return false;
    }
    if (!DuplicateHandle(GetCurrentProcess(), input, GetCurrentProcess(), &m_input, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        if (error) *error = QStringLiteral("cannot duplicate stdin control handle");
        return false;
    }
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) {
        CloseHandle(m_input); m_input = INVALID_HANDLE_VALUE;
        if (error) *error = QStringLiteral("cannot create stdin cancellation event");
        return false;
    }
    m_thread = std::thread([this] { readLoop(); });
    return true;
}
void VRControlChannel::stop() {
    m_finished = true;
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_thread.joinable()) {
        CancelSynchronousIo(static_cast<HANDLE>(m_thread.native_handle()));
        m_thread.join();
    }
    if (m_input != INVALID_HANDLE_VALUE) { CloseHandle(m_input); m_input = INVALID_HANDLE_VALUE; }
    if (m_stopEvent) { CloseHandle(m_stopEvent); m_stopEvent = nullptr; }
}
void VRControlChannel::readLoop() {
    char chunk[4096];
    while (WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0) {
        DWORD available = sizeof(chunk);
        if (m_type == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(m_input, nullptr, 0, nullptr, &available, nullptr)) break;
            if (!available) {
                if (WaitForSingleObject(m_stopEvent, 10) == WAIT_OBJECT_0) return;
                continue;
            }
        }
        DWORD read = 0;
        // 仅读取 pipe 已公布的字节，避免 CancelSynchronousIo 与进入阻塞读取之间的竞态。
        if (!ReadFile(m_input, chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr) || !read) break;
        const QByteArray data(chunk, static_cast<qsizetype>(read));
        QMetaObject::invokeMethod(this, [this, data] { consume(data); }, Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(this, [this] { finish(); }, Qt::QueuedConnection);
}
void VRControlChannel::consume(const QByteArray& bytes) {
    if (m_finished) return;
    m_buffer.append(bytes);
    // 外部输入边界，禁止无限增长的未终止 JSON 行。
    if (m_buffer.size() > 1024 * 1024) { finish(); return; }
    for (;;) {
        const qsizetype newline = m_buffer.indexOf('\n');
        if (newline < 0) return;
        const QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) continue;
        const auto command = document.object();
        const QString name = command.value(QStringLiteral("cmd")).toString();
        if (name == QStringLiteral("quit")) { finish(); return; }
        if (!name.isEmpty() && m_command) m_command(command);
    }
}
void VRControlChannel::finish() {
    if (m_finished) return;
    m_finished = true;
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_close) m_close();
}

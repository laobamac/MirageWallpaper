#include "ControlChannel.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <cstdio>
#include <utility>

ControlChannel::ControlChannel(Handler handler, std::function<void()> onEof, QObject* parent)
    : QObject(parent), m_handler(std::move(handler)), m_onEof(std::move(onEof)) {}

ControlChannel::~ControlChannel() {
    if (qEnvironmentVariableIsSet("WR_DEBUG")) std::fprintf(stderr, "WebWallpaper: stopping stdin reader\n");
    m_stopping.store(true);
    if (m_reader.joinable()) {
        // stdin 可为同步或父进程创建的异步管道；两种取消都要覆盖。
        // 取消可能与下一次 ReadFile 的启动交错，因此等到线程确认退出。
        bool loggedCancellation = false;
        do {
            const BOOL cancelled = CancelSynchronousIo(static_cast<HANDLE>(m_reader.native_handle()));
            const DWORD cancellationError = cancelled ? ERROR_SUCCESS : GetLastError();
            CancelIoEx(m_input, nullptr);
            if (!loggedCancellation && qEnvironmentVariableIsSet("WR_DEBUG")) {
                std::fprintf(stderr, "WebWallpaper: stdin cancellation result %lu\n", cancellationError);
                loggedCancellation = true;
            }
        } while (WaitForSingleObject(m_finished, 10) == WAIT_TIMEOUT);
        m_reader.join();
    }
    if (m_input) CloseHandle(m_input);
    if (m_finished) CloseHandle(m_finished);
    if (qEnvironmentVariableIsSet("WR_DEBUG")) std::fprintf(stderr, "WebWallpaper: stdin reader stopped\n");
}

void ControlChannel::start() {
    if (m_reader.joinable()) return;
    const HANDLE source = GetStdHandle(STD_INPUT_HANDLE);
    m_finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_finished || !source || source == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &m_input,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        std::fprintf(stderr, "WebWallpaper: cannot open stdin control channel\n");
        QMetaObject::invokeMethod(this, [this] { m_onEof(); }, Qt::QueuedConnection);
        return;
    }
    m_reader = std::thread([this] { readCommands(); SetEvent(m_finished); });
}

void ControlChannel::dispatch(const QByteArray& line) {
    if (line.trimmed().isEmpty()) return;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        std::fprintf(stderr, "WebWallpaper: invalid JSON control command\n");
        return;
    }
    const QJsonObject command = document.object();
    if (qEnvironmentVariableIsSet("WR_DEBUG")) std::fprintf(stderr, "WebWallpaper: queued command %s\n", qPrintable(command.value("cmd").toString()));
    QMetaObject::invokeMethod(this, [this, command] { if (!m_stopping.load()) m_handler(command); },
                              Qt::QueuedConnection);
}

void ControlChannel::readCommands() {
    QByteArray line;
    bool oversized = false;
    char buffer[4096];
    while (!m_stopping.load()) {
        DWORD count = 0;
        if (!ReadFile(m_input, buffer, sizeof(buffer), &count, nullptr) || count == 0) break;
        for (DWORD index = 0; index < count; ++index) {
            const char value = buffer[index];
            if (value == '\n') {
                if (!oversized) dispatch(line);
                line.clear();
                oversized = false;
            } else if (!oversized) {
                line.append(value);
                if (line.size() > 1024 * 1024) {
                    line.clear();
                    oversized = true;
                    std::fprintf(stderr, "WebWallpaper: control command exceeds 1 MiB\n");
                }
            }
        }
    }
    if (!m_stopping.load()) {
        if (!oversized && !line.isEmpty()) dispatch(line);
        QMetaObject::invokeMethod(this, [this] { if (!m_stopping.load()) m_onEof(); }, Qt::QueuedConnection);
    }
}

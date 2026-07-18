#include "Renderers/Common/ControlReader.h"

#include <QJsonDocument>
#include <QTextStream>

#include <atomic>
#include <iostream>

namespace Mirage {

ControlReader::ControlReader(QObject* parent)
    : QObject(parent) {
}

ControlReader::~ControlReader() {
    stop();
}

void ControlReader::start() {
    if (m_thread) return;
    m_running = true;
    m_thread = QThread::create([this] {
        std::string line;
        while (m_running && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(line));
            if (doc.isObject()) emit commandReceived(doc.object());
        }
        emit inputClosed();
    });
    m_thread->start();
}

void ControlReader::stop() {
    m_running = false;
    if (!m_thread) return;
    m_thread->quit();
    m_thread->wait(1000);
    delete m_thread;
    m_thread = nullptr;
}

} // namespace Mirage

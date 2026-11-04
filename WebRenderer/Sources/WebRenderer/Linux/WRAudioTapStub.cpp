#include "WRAudioTap.h"

// The stub is an explicit build-time selection for systems without an audio
// backend. It preserves rendering while clearly reporting that no spectrum can
// be produced; it is never selected automatically at runtime.
class WRAudioTap::Impl final {
public:
    bool start(QString* error) {
        *error = QStringLiteral("audio spectrum backend is disabled at build time");
        return false;
    }
    void stop() {}
    bool isRunning() const { return false; }
    bool copySpectrum(std::array<float, 64>&, std::array<float, 64>&) const { return false; }
};

WRAudioTap::WRAudioTap() : m_impl(std::make_unique<Impl>()) {}
WRAudioTap::~WRAudioTap() = default;
bool WRAudioTap::start(QString* error) { return m_impl->start(error); }
void WRAudioTap::stop() { m_impl->stop(); }
bool WRAudioTap::isRunning() const { return m_impl->isRunning(); }
bool WRAudioTap::copySpectrum(std::array<float, 64>& left, std::array<float, 64>& right) const {
    return m_impl->copySpectrum(left, right);
}

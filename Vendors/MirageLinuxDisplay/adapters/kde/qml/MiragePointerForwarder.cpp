#include "MiragePointerForwarder.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr uint32_t BtnLeft = 0x110u;
constexpr uint32_t BtnRight = 0x111u;
constexpr uint32_t BtnMiddle = 0x112u;
constexpr uint32_t BtnSide = 0x113u;
constexpr uint32_t BtnExtra = 0x114u;

constexpr uint32_t ModShift = 1u << 0u;
constexpr uint32_t ModControl = 1u << 2u;
constexpr uint32_t ModAlt = 1u << 3u;
constexpr uint32_t ModNum = 1u << 4u;
constexpr uint32_t ModMeta = 1u << 6u;

} // namespace

void MiragePointerForwarder::setGeometry(const QRectF& bounds, int physicalWidth,
                                         int physicalHeight) {
    m_bounds = bounds;
    m_physicalWidth = std::max(physicalWidth, 1);
    m_physicalHeight = std::max(physicalHeight, 1);
}

uint32_t MiragePointerForwarder::linuxButton(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton: return BtnLeft;
    case Qt::RightButton: return BtnRight;
    case Qt::MiddleButton: return BtnMiddle;
    case Qt::BackButton: return BtnSide;
    case Qt::ForwardButton: return BtnExtra;
    default: return 0;
    }
}

uint32_t MiragePointerForwarder::linuxModifiers(Qt::KeyboardModifiers modifiers) {
    uint32_t result = 0;
    if (modifiers.testFlag(Qt::ShiftModifier)) result |= ModShift;
    if (modifiers.testFlag(Qt::ControlModifier)) result |= ModControl;
    if (modifiers.testFlag(Qt::AltModifier)) result |= ModAlt;
    if (modifiers.testFlag(Qt::MetaModifier)) result |= ModMeta;
    if (modifiers.testFlag(Qt::KeypadModifier)) result |= ModNum;
    return result;
}

bool MiragePointerForwarder::mapPosition(const QPointF& localPosition, float& x, float& y,
                                         bool clampToBounds, bool* insideBounds) const {
    if (insideBounds != nullptr) *insideBounds = false;
    if (m_bounds.width() <= 0.0 || m_bounds.height() <= 0.0) return false;

    QPointF local = localPosition;
    const bool inside = m_bounds.contains(local);
    if (insideBounds != nullptr) *insideBounds = inside;
    if (!inside) {
        if (!clampToBounds) return false;
        const qreal right = std::nextafter(m_bounds.right(), m_bounds.left());
        const qreal bottom = std::nextafter(m_bounds.bottom(), m_bounds.top());
        local.setX(std::clamp(local.x(), m_bounds.left(), right));
        local.setY(std::clamp(local.y(), m_bounds.top(), bottom));
    }

    const qreal normalizedX = (local.x() - m_bounds.left()) / m_bounds.width();
    const qreal normalizedY = (local.y() - m_bounds.top()) / m_bounds.height();
    x = static_cast<float>(normalizedX * static_cast<qreal>(m_physicalWidth));
    y = static_cast<float>(normalizedY * static_cast<qreal>(m_physicalHeight));
    x = std::clamp(x, 0.0f, std::nextafter(static_cast<float>(m_physicalWidth), 0.0f));
    y = std::clamp(y, 0.0f, std::nextafter(static_cast<float>(m_physicalHeight), 0.0f));
    return true;
}

void MiragePointerForwarder::emitEvent(const Event& event) {
    if (m_sink) m_sink(event);
}

void MiragePointerForwarder::ensureEnter(float x, float y, uint64_t timestamp) {
    if (m_pointerInside) return;
    m_pointerInside = true;
    Event event;
    event.type = Event::Type::Enter;
    event.x = x;
    event.y = y;
    event.timestamp = timestamp;
    emitEvent(event);
}

bool MiragePointerForwarder::handleMove(const QPointF& localPosition,
                                        Qt::KeyboardModifiers modifiers,
                                        uint64_t timestamp) {
    float x = 0.0f;
    float y = 0.0f;
    if (!mapPosition(localPosition, x, y, m_pressedButtons != Qt::NoButton, nullptr)) {
        if (!m_pointerInside) return false;
        m_pointerInside = false;
        Event event;
        event.type = Event::Type::Leave;
        event.timestamp = timestamp;
        emitEvent(event);
        return true;
    }
    m_lastX = x;
    m_lastY = y;
    ensureEnter(x, y, timestamp);
    Event event;
    event.type = Event::Type::Motion;
    event.x = x;
    event.y = y;
    event.modifiers = linuxModifiers(modifiers);
    event.timestamp = timestamp;
    emitEvent(event);
    return true;
}

bool MiragePointerForwarder::handleButton(const QPointF& localPosition, Qt::MouseButton button,
                                          bool pressed, Qt::KeyboardModifiers modifiers,
                                          uint64_t timestamp) {
    const uint32_t code = linuxButton(button);
    if (code == 0) return false;
    const bool trackedRelease = !pressed && m_pressedButtons.testFlag(button);
    float x = 0.0f;
    float y = 0.0f;
    bool inside = false;
    if (!mapPosition(localPosition, x, y, trackedRelease, &inside)) return false;

    m_lastX = x;
    m_lastY = y;
    if (inside || m_pointerInside) ensureEnter(x, y, timestamp);
    if (pressed) m_pressedButtons |= button;
    else m_pressedButtons &= ~Qt::MouseButtons(button);

    Event event;
    event.type = Event::Type::Button;
    event.x = x;
    event.y = y;
    event.button = code;
    event.buttonState = pressed ? MD_BUTTON_PRESSED : MD_BUTTON_RELEASED;
    event.modifiers = linuxModifiers(modifiers);
    event.timestamp = timestamp;
    emitEvent(event);

    if (!pressed && !inside && m_pressedButtons == Qt::NoButton && m_pointerInside) {
        m_pointerInside = false;
        Event leave;
        leave.type = Event::Type::Leave;
        leave.timestamp = timestamp;
        emitEvent(leave);
    }
    return true;
}

bool MiragePointerForwarder::handleWheel(const QPointF& localPosition, const QPoint& angleDelta,
                                         const QPoint& pixelDelta,
                                         Qt::KeyboardModifiers modifiers,
                                         uint64_t timestamp) {
    float x = 0.0f;
    float y = 0.0f;
    if (!mapPosition(localPosition, x, y, false, nullptr)) return false;

    const bool continuous = !pixelDelta.isNull() && angleDelta.isNull();
    const float deltaX = continuous ? static_cast<float>(pixelDelta.x()) / 120.0f
                                    : static_cast<float>(angleDelta.x()) / 120.0f;
    const float deltaY = continuous ? static_cast<float>(pixelDelta.y()) / 120.0f
                                    : static_cast<float>(angleDelta.y()) / 120.0f;
    if (deltaX == 0.0f && deltaY == 0.0f) return false;

    m_lastX = x;
    m_lastY = y;
    ensureEnter(x, y, timestamp);
    Event event;
    event.type = Event::Type::Axis;
    event.x = x;
    event.y = y;
    event.deltaX = deltaX;
    event.deltaY = deltaY;
    event.axisSource = continuous ? MD_AXIS_CONTINUOUS : MD_AXIS_WHEEL;
    event.modifiers = linuxModifiers(modifiers);
    event.timestamp = timestamp;
    emitEvent(event);
    return true;
}

bool MiragePointerForwarder::handleLeave(uint64_t timestamp) {
    if (!m_pointerInside || m_pressedButtons != Qt::NoButton) return false;
    m_pointerInside = false;
    Event event;
    event.type = Event::Type::Leave;
    event.timestamp = timestamp;
    emitEvent(event);
    return true;
}

void MiragePointerForwarder::reset(uint64_t timestamp) {
    const Qt::MouseButton buttons[] = {
        Qt::LeftButton, Qt::RightButton, Qt::MiddleButton,
        Qt::BackButton, Qt::ForwardButton,
    };
    for (Qt::MouseButton button : buttons) {
        if (!m_pressedButtons.testFlag(button)) continue;
        const uint32_t code = linuxButton(button);
        if (code == 0) continue;
        Event event;
        event.type = Event::Type::Button;
        event.x = m_lastX;
        event.y = m_lastY;
        event.button = code;
        event.buttonState = MD_BUTTON_RELEASED;
        event.timestamp = timestamp;
        emitEvent(event);
    }
    if (m_pointerInside) {
        Event event;
        event.type = Event::Type::Leave;
        event.timestamp = timestamp;
        emitEvent(event);
    }
    m_pressedButtons = Qt::NoButton;
    m_pointerInside = false;
}

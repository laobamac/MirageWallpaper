#pragma once

#include <mirage_display.h>

#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <Qt>

#include <cstdint>
#include <functional>
#include <utility>

/* Window-independent pointer state used by the Plasma wallpaper item. */
class MiragePointerForwarder final {
public:
    struct Event {
        enum class Type {
            Enter,
            Leave,
            Motion,
            Button,
            Axis,
        };

        Type type = Type::Motion;
        float x = 0.0f;
        float y = 0.0f;
        float deltaX = 0.0f;
        float deltaY = 0.0f;
        uint32_t button = 0;
        md_button_state_t buttonState = MD_BUTTON_RELEASED;
        md_axis_source_t axisSource = MD_AXIS_WHEEL;
        uint32_t modifiers = 0;
        uint64_t timestamp = 0;
    };

    using Sink = std::function<void(const Event&)>;

    MiragePointerForwarder() = default;

    void setSink(Sink sink) { m_sink = std::move(sink); }
    void setGeometry(const QRectF& bounds, int physicalWidth, int physicalHeight);

    bool handleMove(const QPointF& localPosition, Qt::KeyboardModifiers modifiers,
                    uint64_t timestamp);
    bool handleButton(const QPointF& localPosition, Qt::MouseButton button, bool pressed,
                      Qt::KeyboardModifiers modifiers, uint64_t timestamp);
    bool handleWheel(const QPointF& localPosition, const QPoint& angleDelta,
                     const QPoint& pixelDelta, Qt::KeyboardModifiers modifiers,
                     uint64_t timestamp);
    bool handleLeave(uint64_t timestamp);
    void reset(uint64_t timestamp);

    bool pointerInside() const { return m_pointerInside; }
    Qt::MouseButtons pressedButtons() const { return m_pressedButtons; }
    float lastX() const { return m_lastX; }
    float lastY() const { return m_lastY; }

    static uint32_t linuxButton(Qt::MouseButton button);
    static uint32_t linuxModifiers(Qt::KeyboardModifiers modifiers);

private:
    bool mapPosition(const QPointF& localPosition, float& x, float& y,
                     bool clampToBounds, bool* insideBounds) const;
    void emitEvent(const Event& event);
    void ensureEnter(float x, float y, uint64_t timestamp);

    QRectF m_bounds;
    int m_physicalWidth = 1;
    int m_physicalHeight = 1;
    bool m_pointerInside = false;
    Qt::MouseButtons m_pressedButtons;
    float m_lastX = 0.0f;
    float m_lastY = 0.0f;
    Sink m_sink;
};

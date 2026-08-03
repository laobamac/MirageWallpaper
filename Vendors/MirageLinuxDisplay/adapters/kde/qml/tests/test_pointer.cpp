#include "MiragePointerForwarder.hpp"

#include <QVector>

/* Keep assertions live even in Release builds (-DNDEBUG). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <cmath>

using Event = MiragePointerForwarder::Event;

static bool close_enough(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

static MiragePointerForwarder make_forwarder(QVector<Event>& events) {
    MiragePointerForwarder forwarder;
    forwarder.setGeometry(QRectF(0.0, 0.0, 100.0, 50.0), 200, 100);
    forwarder.setSink([&events](const Event& event) { events.append(event); });
    return forwarder;
}

static void test_click_and_enter_leave(void) {
    QVector<Event> events;
    MiragePointerForwarder forwarder = make_forwarder(events);

    assert(forwarder.handleButton(QPointF(25.0, 10.0), Qt::LeftButton, true,
                                  Qt::ShiftModifier, 100));
    assert(events.size() == 2);
    assert(events[0].type == Event::Type::Enter);
    assert(events[1].type == Event::Type::Button);
    assert(events[1].button == UINT32_C(0x110));
    assert(events[1].buttonState == MD_BUTTON_PRESSED);
    assert(events[1].modifiers == UINT32_C(1));
    assert(close_enough(events[1].x, 50.0f));
    assert(close_enough(events[1].y, 20.0f));

    assert(forwarder.handleButton(QPointF(25.0, 10.0), Qt::LeftButton, false,
                                  Qt::NoModifier, 110));
    assert(events.size() == 3);
    assert(events[2].type == Event::Type::Button);
    assert(events[2].buttonState == MD_BUTTON_RELEASED);
    assert(forwarder.pressedButtons() == Qt::NoButton);
    assert(forwarder.pointerInside());

    assert(forwarder.handleLeave(120));
    assert(events.size() == 4);
    assert(events[3].type == Event::Type::Leave);
    assert(events[3].timestamp == 120);
    assert(!forwarder.pointerInside());
}

static void test_drag_outside_release(void) {
    QVector<Event> events;
    MiragePointerForwarder forwarder = make_forwarder(events);

    assert(forwarder.handleButton(QPointF(50.0, 25.0), Qt::LeftButton, true,
                                  Qt::NoModifier, 200));
    events.clear();

    assert(forwarder.handleMove(QPointF(150.0, 80.0), Qt::ControlModifier, 210));
    assert(events.size() == 1);
    assert(events[0].type == Event::Type::Motion);
    assert(events[0].x >= 0.0f && events[0].x < 200.0f);
    assert(events[0].y >= 0.0f && events[0].y < 100.0f);
    assert(events[0].modifiers == (UINT32_C(1) << 2));
    assert(forwarder.pointerInside());
    assert(!forwarder.handleLeave(215));

    assert(forwarder.handleButton(QPointF(150.0, 80.0), Qt::LeftButton, false,
                                  Qt::NoModifier, 220));
    assert(events.size() == 3);
    assert(events[1].type == Event::Type::Button);
    assert(events[1].buttonState == MD_BUTTON_RELEASED);
    assert(events[1].x < 200.0f && events[1].y < 100.0f);
    assert(events[2].type == Event::Type::Leave);
    assert(events[2].timestamp == 220);
    assert(forwarder.pressedButtons() == Qt::NoButton);
    assert(!forwarder.pointerInside());
}

static void test_wheel_sources(void) {
    QVector<Event> events;
    MiragePointerForwarder forwarder = make_forwarder(events);

    assert(forwarder.handleWheel(QPointF(10.0, 20.0), QPoint(120, -240), QPoint(),
                                 Qt::AltModifier, 300));
    assert(events.size() == 2);
    assert(events[0].type == Event::Type::Enter);
    assert(events[1].type == Event::Type::Axis);
    assert(events[1].axisSource == MD_AXIS_WHEEL);
    assert(close_enough(events[1].deltaX, 1.0f));
    assert(close_enough(events[1].deltaY, -2.0f));
    assert(events[1].modifiers == (UINT32_C(1) << 3));

    assert(forwarder.handleWheel(QPointF(10.0, 20.0), QPoint(), QPoint(30, 60),
                                 Qt::NoModifier, 310));
    assert(events.size() == 3);
    assert(events[2].type == Event::Type::Axis);
    assert(events[2].axisSource == MD_AXIS_CONTINUOUS);
    assert(close_enough(events[2].deltaX, 0.25f));
    assert(close_enough(events[2].deltaY, 0.5f));

    assert(!forwarder.handleWheel(QPointF(10.0, 20.0), QPoint(), QPoint(),
                                  Qt::NoModifier, 320));
    assert(events.size() == 3);
    assert(!forwarder.handleWheel(QPointF(110.0, 20.0), QPoint(0, 120), QPoint(),
                                  Qt::NoModifier, 330));
}

static void test_deactivation_recovery(void) {
    QVector<Event> events;
    MiragePointerForwarder forwarder = make_forwarder(events);

    assert(forwarder.handleMove(QPointF(40.0, 15.0), Qt::NoModifier, 400));
    assert(forwarder.handleButton(QPointF(40.0, 15.0), Qt::LeftButton, true,
                                  Qt::NoModifier, 410));
    assert(forwarder.handleButton(QPointF(40.0, 15.0), Qt::RightButton, true,
                                  Qt::NoModifier, 420));
    events.clear();

    forwarder.reset(430);
    assert(events.size() == 3);
    assert(events[0].type == Event::Type::Button);
    assert(events[0].button == UINT32_C(0x110));
    assert(events[0].buttonState == MD_BUTTON_RELEASED);
    assert(events[1].type == Event::Type::Button);
    assert(events[1].button == UINT32_C(0x111));
    assert(events[1].buttonState == MD_BUTTON_RELEASED);
    assert(events[2].type == Event::Type::Leave);
    assert(events[2].timestamp == 430);
    assert(forwarder.pressedButtons() == Qt::NoButton);
    assert(!forwarder.pointerInside());

    forwarder.reset(440);
    assert(events.size() == 3);
}

int main(void) {
    test_click_and_enter_leave();
    test_drag_outside_release();
    test_wheel_sources();
    test_deactivation_recovery();
    return 0;
}

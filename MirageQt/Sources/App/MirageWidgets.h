// MirageWidgets — shared widget variants used across the MirageQt UI.
//
// MirageComboBox is a plain QComboBox: its rounded-corner look is fully
// controlled by the global Mirage style sheet (see the QComboBox rules in
// MirageStyle.cpp) instead of custom-drawn widgets. It keeps one behavioural
// fix over the stock control: the stock control changes its current index on
// mouse-wheel events even when the popup is closed, which is surprising while
// scrolling a page (e.g. the settings forms). We only let the wheel through
// when the popup is open; otherwise the event is ignored so it propagates to
// the parent scroll area.

#pragma once

#include <QAbstractItemView>
#include <QComboBox>
#include <QWheelEvent>

namespace Mirage {

class MirageComboBox : public QComboBox {
public:
    explicit MirageComboBox(QWidget* parent = nullptr)
        : QComboBox(parent) {}

protected:
    void wheelEvent(QWheelEvent* event) override {
        if (!view() || !view()->isVisible()) {
            event->ignore();
            return;
        }
        QComboBox::wheelEvent(event);
    }
};

} // namespace Mirage

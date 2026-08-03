// MirageWidgets — shared widget variants used across the MirageQt UI.
//
// MirageComboBox bundles two fixes over the stock QComboBox:
//
//  1. Wheel safety: the stock control changes its current index on mouse-wheel
//     events even when the popup is closed, which is surprising while scrolling
//     a page (e.g. the settings forms). We only let the wheel through when the
//     popup is open; otherwise the event is ignored so it propagates to the
//     parent scroll area.
//
//  2. Rounded popup: Qt style sheets cannot clip the QAbstractItemView
//     background or the per-item selection highlight to a border-radius (the
//     fill stays square under a rounded border). MirageComboBox therefore uses
//     a custom translucent list view that paints the rounded panel itself and a
//     delegate that paints rounded selection highlights.

#pragma once

#include "App/MirageStyle.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QPointer>
#include <QListView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QWheelEvent>

namespace Mirage {

// Popup list view for MirageComboBox: translucent panel with a rounded
// background and border painted by hand (QSS cannot round the item-view fill).
class MirageComboBoxView : public QListView {
public:
    explicit MirageComboBoxView(QWidget* parent = nullptr)
        : QListView(parent) {
        setFrameShape(QFrame::NoFrame);
        setAttribute(Qt::WA_TranslucentBackground);
        viewport()->setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setUniformItemSizes(true);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        const QRectF panel = QRectF(viewport()->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(currentBorderColor(), 1.0));
        painter.setBrush(pal.color(QPalette::Base));
        painter.drawRoundedRect(panel, 8.0, 8.0);
        QListView::paintEvent(event);
    }
};

// Rounds the selection/hover highlight and paints the item text manually so no
// square QSS/selection background leaks through.
class MirageComboBoxDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        // The view passes a mostly empty QStyleOptionViewItem to the delegate;
        // initStyleOption() fills in the display text, icon and item state from
        // the model, so paint from the initialized copy.
        QStyleOptionViewItem copy = option;
        initStyleOption(&copy, index);

        const QPalette& pal = copy.palette;
        const bool enabled = copy.state & QStyle::State_Enabled;
        const bool selected = enabled && (copy.state & QStyle::State_Selected);
        const bool hovered = enabled && (copy.state & QStyle::State_MouseOver);

        if (selected || hovered) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            QColor highlight = pal.color(QPalette::Highlight);
            if (hovered && !selected) highlight = highlight.lighter(125);
            painter->setBrush(highlight);
            painter->drawRoundedRect(QRectF(copy.rect).adjusted(3.0, 2.0, -3.0, -2.0), 6.0, 6.0);
            painter->restore();
        }

        QColor textColor = pal.color(QPalette::Text);
        if (!enabled) {
            textColor = pal.color(QPalette::Disabled, QPalette::Text);
        } else if (selected || hovered) {
            textColor = pal.color(QPalette::HighlightedText);
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(textColor);
        QRect textRect = copy.rect.adjusted(14, 0, -10, 0);
        if (!copy.icon.isNull()) {
            const QSize iconSize = copy.icon.actualSize(copy.decorationSize);
            const QRect iconRect(textRect.left(), textRect.center().y() - iconSize.height() / 2,
                                 iconSize.width(), iconSize.height());
            painter->drawPixmap(iconRect, copy.icon.pixmap(iconSize));
            textRect.adjust(iconSize.width() + 8, 0, 0, 0);
        }
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, copy.text);
        painter->restore();
    }
};

class MirageComboBox : public QComboBox {
public:
    MirageComboBox(QWidget* parent = nullptr)
        : QComboBox(parent) {
        auto* view = new MirageComboBoxView(this);
        view->setItemDelegate(new MirageComboBoxDelegate(view));
        setView(view);
        if (QFrame* container = qobject_cast<QFrame*>(view->parentWidget())) {
            // QComboBoxPrivateContainer paints a square frame around the view
            // even after stripping its frame shape; suppress it so only the
            // rounded panel from MirageComboBoxView is visible.
            m_container = container;
            container->setFrameShape(QFrame::NoFrame);
            container->setAttribute(Qt::WA_TranslucentBackground);
            container->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_container && event->type() == QEvent::Paint) return true;
        return QComboBox::eventFilter(watched, event);
    }

    void wheelEvent(QWheelEvent* event) override {
        if (!view() || !view()->isVisible()) {
            event->ignore();
            return;
        }
        QComboBox::wheelEvent(event);
    }

private:
    QPointer<QFrame> m_container;
};

} // namespace Mirage

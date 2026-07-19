#include "App/MirageStyle.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

namespace Mirage {

void applyMirageStyle(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFont font(QStringLiteral("Noto Sans CJK SC"));
    font.setPointSize(10);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    app.setFont(font);

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(61, 56, 50));
    palette.setColor(QPalette::WindowText, QColor(241, 239, 236));
    palette.setColor(QPalette::Base, QColor(49, 45, 40));
    palette.setColor(QPalette::AlternateBase, QColor(72, 67, 61));
    palette.setColor(QPalette::ToolTipBase, QColor(39, 36, 32));
    palette.setColor(QPalette::ToolTipText, QColor(247, 246, 244));
    palette.setColor(QPalette::Text, QColor(241, 239, 236));
    palette.setColor(QPalette::Button, QColor(99, 94, 87));
    palette.setColor(QPalette::ButtonText, QColor(247, 246, 244));
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Light, QColor(128, 121, 113));
    palette.setColor(QPalette::Midlight, QColor(107, 101, 94));
    palette.setColor(QPalette::Mid, QColor(79, 74, 68));
    palette.setColor(QPalette::Dark, QColor(35, 32, 29));
    palette.setColor(QPalette::Shadow, QColor(20, 18, 16));
    palette.setColor(QPalette::Highlight, QColor(10, 132, 255));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor(41, 151, 255));
    palette.setColor(QPalette::LinkVisited, QColor(111, 174, 255));
    palette.setColor(QPalette::PlaceholderText, QColor(163, 158, 151));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(139, 134, 128));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(139, 134, 128));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(139, 134, 128));
    palette.setColor(QPalette::Disabled, QPalette::Button, QColor(76, 72, 67));
    app.setPalette(palette);

    app.setStyleSheet(QStringLiteral(R"QSS(
        QWidget {
            color: #f1efec;
            font-family: "Noto Sans CJK SC", "Noto Sans", sans-serif;
            font-size: 14px;
        }
        QMainWindow, QDialog, QMessageBox, QStackedWidget {
            background-color: #3d3832;
        }
        QToolTip {
            color: #f7f6f4;
            background-color: #272420;
            border: 1px solid #736d65;
            padding: 5px;
        }
        QPushButton, QToolButton {
            min-height: 20px;
            padding: 3px 10px;
            color: #f7f6f4;
            background-color: #625d57;
            border: 1px solid #7b756d;
            border-radius: 5px;
        }
        QPushButton:hover, QToolButton:hover {
            background-color: #716b64;
            border-color: #8a837a;
        }
        QPushButton:pressed, QToolButton:pressed {
            background-color: #514c47;
        }
        QPushButton:checked, QToolButton:checked,
        QPushButton[accent="true"], QToolButton[accent="true"] {
            color: white;
            background-color: #0a84ff;
            border-color: #238fff;
        }
        QPushButton[accent="true"]:hover, QToolButton[accent="true"]:hover {
            background-color: #248fff;
        }
        QPushButton[danger="true"] {
            color: white;
            background-color: #c44336;
            border-color: #d05a4e;
        }
        QPushButton[danger="true"]:disabled {
            color: #d3a09a;
            background-color: #963d35;
            border-color: #a64a41;
        }
        QPushButton:disabled, QToolButton:disabled {
            color: #8b8680;
            background-color: #4c4843;
            border-color: #5b5650;
        }
        QToolButton[flatButton="true"] {
            background: transparent;
            border: 0;
            border-radius: 3px;
            padding: 4px 8px;
        }
        QToolButton[flatButton="true"]:hover {
            background-color: #57524c;
        }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
            min-height: 18px;
            color: #f4f2ef;
            background-color: #4b4741;
            border: 1px solid #69635c;
            border-radius: 5px;
            padding: 2px 8px;
            selection-background-color: #0a84ff;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
            border-color: #0a84ff;
        }
        QComboBox::drop-down {
            width: 25px;
            border: 0;
        }
        QComboBox QAbstractItemView {
            color: #f4f2ef;
            background-color: #4b4741;
            border: 1px solid #736d65;
            selection-background-color: #0a84ff;
        }
        QCheckBox {
            spacing: 7px;
        }
        QCheckBox::indicator {
            width: 17px;
            height: 17px;
        }
        QGroupBox {
            border: 1px solid #625d56;
            border-radius: 0;
            margin-top: 10px;
            padding-top: 9px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 9px;
            padding: 0 4px;
            color: #e4e1dd;
            background-color: #3d3832;
        }
        QScrollArea, QScrollArea > QWidget > QWidget, QListWidget,
        QAbstractScrollArea::viewport {
            background: transparent;
        }
        QScrollBar:vertical {
            width: 12px;
            margin: 0;
            background: transparent;
        }
        QScrollBar::handle:vertical {
            min-height: 34px;
            margin: 2px 3px;
            background-color: #77716a;
            border: 1px solid #89827a;
            border-radius: 5px;
        }
        QScrollBar::handle:vertical:hover {
            background-color: #89827a;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            height: 0;
            background: transparent;
        }
        QScrollBar:horizontal {
            height: 15px;
            margin: 0;
            background: transparent;
        }
        QScrollBar::handle:horizontal {
            min-width: 34px;
            margin: 3px 2px;
            background-color: #77716a;
            border: 1px solid #89827a;
            border-radius: 5px;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
            width: 0;
            background: transparent;
        }
        QSlider::groove:horizontal {
            height: 4px;
            background-color: #706a63;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background-color: #0a84ff;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 17px;
            margin: -7px 0;
            background-color: #aaa59f;
            border: 1px solid #c2bdb7;
            border-radius: 8px;
        }
        QSplitter::handle {
            background-color: #1d1b18;
        }
        QStatusBar {
            color: #b8b3ad;
            background-color: #35312d;
            border-top: 1px solid #211f1c;
        }
        QMenu {
            color: #f4f2ef;
            background-color: #45413b;
            border: 1px solid #746e66;
            padding: 5px;
        }
        QMenu::item {
            padding: 6px 24px 6px 10px;
            border-radius: 4px;
        }
        QMenu::item:selected {
            background-color: #0a84ff;
        }
    )QSS"));
}

} // namespace Mirage

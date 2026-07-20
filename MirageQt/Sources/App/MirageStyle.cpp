#include "App/MirageStyle.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace Mirage {
namespace {

struct StyleColors {
    QString window;
    QString text;
    QString secondary;
    QString tertiary;
    QString base;
    QString alternate;
    QString tooltip;
    QString tooltipText;
    QString button;
    QString buttonHover;
    QString buttonPressed;
    QString border;
    QString disabledText;
    QString disabledButton;
    QString scroll;
    QString scrollHover;
    QString splitter;
    QString menu;
};

StyleColors darkColors() {
    return {
        QStringLiteral("#3d3832"), QStringLiteral("#f1efec"), QStringLiteral("#b8b3ad"),
        QStringLiteral("#8f8982"), QStringLiteral("#312d28"), QStringLiteral("#48433d"),
        QStringLiteral("#272420"), QStringLiteral("#f7f6f4"), QStringLiteral("#625d57"),
        QStringLiteral("#716b64"), QStringLiteral("#514c47"), QStringLiteral("#7b756d"),
        QStringLiteral("#8b8680"), QStringLiteral("#4c4843"), QStringLiteral("#77716a"),
        QStringLiteral("#89827a"), QStringLiteral("#1d1b18"), QStringLiteral("#45413b")};
}

StyleColors lightColors() {
    return {
        QStringLiteral("#f5f5f7"), QStringLiteral("#1d1d1f"), QStringLiteral("#66666b"),
        QStringLiteral("#8a8a90"), QStringLiteral("#ffffff"), QStringLiteral("#e9e9ed"),
        QStringLiteral("#2d2d30"), QStringLiteral("#ffffff"), QStringLiteral("#e5e5e9"),
        QStringLiteral("#d8d8dd"), QStringLiteral("#c9c9ce"), QStringLiteral("#b8b8be"),
        QStringLiteral("#99999f"), QStringLiteral("#ededf0"), QStringLiteral("#aaaab0"),
        QStringLiteral("#8f8f95"), QStringLiteral("#c7c7cc"), QStringLiteral("#ffffff")};
}

bool useDarkAppearance(QApplication& app, const QString& appearance) {
    if (appearance == QStringLiteral("dark")) return true;
    if (appearance == QStringLiteral("light")) return false;
    const Qt::ColorScheme scheme = app.styleHints()->colorScheme();
    return scheme == Qt::ColorScheme::Dark || scheme == Qt::ColorScheme::Unknown;
}

QString styleSheet(const StyleColors& color) {
    QString sheet = QStringLiteral(R"QSS(
        QWidget {
            color: @TEXT@;
            font-family: "Noto Sans CJK SC", "Noto Sans", sans-serif;
            font-size: 14px;
        }
        QMainWindow, QDialog, QMessageBox, QStackedWidget {
            background-color: @WINDOW@;
        }
        QLabel[secondary="true"] { color: @SECONDARY@; }
        QLabel[tertiary="true"] { color: @TERTIARY@; }
        QLabel[error="true"] { color: #ff453a; }
        QLabel[warning="true"] { color: #ff9f0a; }
        QLabel[successText="true"], QLabel#steamAccount { color: #30b653; }
        QToolTip {
            color: @TOOLTIP_TEXT@;
            background-color: @TOOLTIP@;
            border: 1px solid @BORDER@;
            padding: 5px;
        }
        QPushButton, QToolButton {
            min-height: 20px;
            padding: 3px 10px;
            color: @TEXT@;
            background-color: @BUTTON@;
            border: 1px solid @BORDER@;
            border-radius: 5px;
        }
        QPushButton:hover, QToolButton:hover {
            background-color: @BUTTON_HOVER@;
        }
        QPushButton:pressed, QToolButton:pressed {
            background-color: @BUTTON_PRESSED@;
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
        QPushButton[success="true"] {
            color: white;
            background-color: #29964a;
            border-color: #35ab58;
        }
        QPushButton[warningAccent="true"] {
            color: white;
            background-color: #d98222;
            border-color: #ed9938;
        }
        QPushButton:disabled, QToolButton:disabled {
            color: @DISABLED_TEXT@;
            background-color: @DISABLED_BUTTON@;
            border-color: @BORDER@;
        }
        QPushButton[flatAction="true"] {
            color: @SECONDARY@;
            background: transparent;
            border: 0;
            padding: 2px 5px;
        }
        QPushButton[flatAction="true"]:hover { color: #0a84ff; }
        QPushButton[segment="true"] { border-radius: 3px; }
        QToolButton[flatButton="true"] {
            background: transparent;
            border: 0;
            border-radius: 3px;
            padding: 4px 8px;
        }
        QToolButton[flatButton="true"]:hover { background-color: @BUTTON_HOVER@; }
        QToolButton[settingsToolbar="true"] {
            background: transparent;
            border: 0;
            border-radius: 6px;
            padding: 4px;
        }
        QToolButton[settingsToolbar="true"]:hover { background-color: @ALTERNATE@; }
        QToolButton[settingsToolbar="true"]:checked { color: white; background-color: #0a84ff; }
        QToolButton[tagChip="true"] { border-radius: 13px; padding: 2px 9px; }
        QToolButton[dangerChip="true"] { color: #ff453a; background: transparent; border-color: #c44336; border-radius: 13px; }
        QToolButton[tabButton="true"] {
            color: @TEXT@;
            background: transparent;
            padding: 3px 8px;
            border: 2px solid #0a84ff;
            border-radius: 0;
            font-size: 16px;
            font-weight: 600;
        }
        QToolButton[tabButton="true"]:hover,
        QToolButton[tabButton="true"]:checked { background-color: #0a84ff; color: white; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
            min-height: 18px;
            color: @TEXT@;
            background-color: @BASE@;
            border: 1px solid @BORDER@;
            border-radius: 5px;
            padding: 2px 8px;
            selection-background-color: #0a84ff;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #0a84ff; }
        QLineEdit:read-only { color: @SECONDARY@; }
        QComboBox::drop-down { width: 25px; border: 0; }
        QComboBox QAbstractItemView {
            color: @TEXT@;
            background-color: @BASE@;
            border: 1px solid @BORDER@;
            selection-background-color: #0a84ff;
        }
        QCheckBox { spacing: 7px; }
        QCheckBox::indicator { width: 17px; height: 17px; }
        QGroupBox {
            border: 1px solid @BORDER@;
            border-radius: 6px;
            margin-top: 12px;
            padding-top: 10px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 9px;
            padding: 0 5px;
            color: @TEXT@;
            background-color: @WINDOW@;
            font-weight: 600;
        }
        QScrollArea, QScrollArea > QWidget > QWidget, QListWidget,
        QAbstractScrollArea::viewport { background: transparent; }
        QScrollBar:vertical { width: 12px; margin: 0; background: transparent; }
        QScrollBar::handle:vertical {
            min-height: 34px; margin: 2px 3px; background-color: @SCROLL@;
            border: 1px solid @SCROLL_HOVER@; border-radius: 5px;
        }
        QScrollBar::handle:vertical:hover { background-color: @SCROLL_HOVER@; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { height: 0; background: transparent; }
        QScrollBar:horizontal { height: 12px; margin: 0; background: transparent; }
        QScrollBar::handle:horizontal {
            min-width: 34px; margin: 3px 2px; background-color: @SCROLL@;
            border: 1px solid @SCROLL_HOVER@; border-radius: 5px;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { width: 0; background: transparent; }
        QSlider::groove:horizontal { height: 4px; background-color: @BORDER@; border-radius: 2px; }
        QSlider::sub-page:horizontal { background-color: #0a84ff; border-radius: 2px; }
        QSlider::handle:horizontal {
            width: 17px; margin: -7px 0; background-color: @SCROLL_HOVER@;
            border: 1px solid @BORDER@; border-radius: 8px;
        }
        QProgressBar { border: 1px solid @BORDER@; border-radius: 3px; background-color: @BASE@; }
        QProgressBar::chunk { background-color: #0a84ff; border-radius: 2px; }
        QSplitter::handle { background-color: @SPLITTER@; }
        QMenu {
            color: @TEXT@; background-color: @MENU@; border: 1px solid @BORDER@; padding: 5px;
        }
        QMenu::item { padding: 6px 24px 6px 10px; border-radius: 4px; }
        QMenu::item:selected { color: white; background-color: #0a84ff; }
        QWidget#apiKeyBanner, QLabel#apiKeyInlineWarning {
            background-color: rgba(255, 159, 10, 34);
            border: 1px solid rgba(255, 159, 10, 95);
            border-radius: 8px;
            padding: 8px;
        }
        QWidget#steamSetupBanner {
            background-color: rgba(10, 132, 255, 28);
            border: 1px solid rgba(10, 132, 255, 78);
            border-radius: 8px;
        }
        QWidget#downloadPopover { background-color: @WINDOW@; border: 1px solid @BORDER@; border-radius: 8px; }
        QWidget#downloadRow { background-color: @BASE@; border-bottom: 1px solid @BORDER@; }
        QLabel#downloadBadge {
            color: white; background-color: #ff453a; border-radius: 8px;
            font-size: 9px; font-weight: 700;
        }
        QLabel#workshopDetailPreview { background-color: @ALTERNATE@; border: 3px solid @BASE@; border-radius: 8px; }
        QLabel#presetNotice {
            color: #bf5af2; background-color: rgba(191, 90, 242, 28);
            border: 1px solid rgba(191, 90, 242, 80); border-radius: 8px; padding: 7px;
        }
        QLabel#sectionHeader { border-bottom: 1px solid #0a84ff; padding-bottom: 3px; }
        QWidget#discoverBanner { background-color: @ALTERNATE@; border-radius: 8px; }
        QLabel#bannerCaption { color: white; background-color: rgba(0, 0, 0, 145); padding: 8px 14px; }
    )QSS");

    const QVector<QPair<QString, QString>> replacements = {
        {QStringLiteral("@WINDOW@"), color.window}, {QStringLiteral("@TEXT@"), color.text},
        {QStringLiteral("@SECONDARY@"), color.secondary}, {QStringLiteral("@TERTIARY@"), color.tertiary},
        {QStringLiteral("@BASE@"), color.base}, {QStringLiteral("@ALTERNATE@"), color.alternate},
        {QStringLiteral("@TOOLTIP@"), color.tooltip}, {QStringLiteral("@TOOLTIP_TEXT@"), color.tooltipText},
        {QStringLiteral("@BUTTON@"), color.button}, {QStringLiteral("@BUTTON_HOVER@"), color.buttonHover},
        {QStringLiteral("@BUTTON_PRESSED@"), color.buttonPressed}, {QStringLiteral("@BORDER@"), color.border},
        {QStringLiteral("@DISABLED_TEXT@"), color.disabledText}, {QStringLiteral("@DISABLED_BUTTON@"), color.disabledButton},
        {QStringLiteral("@SCROLL@"), color.scroll}, {QStringLiteral("@SCROLL_HOVER@"), color.scrollHover},
        {QStringLiteral("@SPLITTER@"), color.splitter}, {QStringLiteral("@MENU@"), color.menu},
    };
    for (const auto& replacement : replacements) sheet.replace(replacement.first, replacement.second);
    return sheet;
}

} // namespace

void applyMirageStyle(QApplication& app, const QString& appearance) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font(QStringLiteral("Noto Sans CJK SC"));
    font.setPointSize(10);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    app.setFont(font);

    const bool dark = useDarkAppearance(app, appearance);
    const StyleColors color = dark ? darkColors() : lightColors();
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(color.window));
    palette.setColor(QPalette::WindowText, QColor(color.text));
    palette.setColor(QPalette::Base, QColor(color.base));
    palette.setColor(QPalette::AlternateBase, QColor(color.alternate));
    palette.setColor(QPalette::ToolTipBase, QColor(color.tooltip));
    palette.setColor(QPalette::ToolTipText, QColor(color.tooltipText));
    palette.setColor(QPalette::Text, QColor(color.text));
    palette.setColor(QPalette::Button, QColor(color.button));
    palette.setColor(QPalette::ButtonText, QColor(color.text));
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#0a84ff")));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#0a84ff")));
    palette.setColor(QPalette::PlaceholderText, QColor(color.tertiary));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(color.disabledText));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(color.disabledText));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(color.disabledText));
    palette.setColor(QPalette::Disabled, QPalette::Button, QColor(color.disabledButton));
    app.setPalette(palette);
    app.setStyleSheet(styleSheet(color));
}

} // namespace Mirage

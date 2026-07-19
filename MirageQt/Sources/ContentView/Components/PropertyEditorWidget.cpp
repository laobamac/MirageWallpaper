#include "ContentView/Components/PropertyEditorWidget.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <cmath>

namespace Mirage {
namespace {

constexpr int kPropertyWidth = 282;

const QHash<QString, QString>& localizationTable() {
    static const QHash<QString, QString> table = [] {
        QHash<QString, QString> result;
        const QString path = QString::fromUtf8(MIRAGEQT_REPO_ROOT)
            + QStringLiteral("/Mirage/Mirage Wallpaper/Resources/ui_zh-chs.json");
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return result;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.value().isString()) result.insert(it.key(), it.value().toString());
        }
        return result;
    }();
    return table;
}

QString resolveLocalization(const QString& raw) {
    const QString key = raw.trimmed();
    if (!key.startsWith(QStringLiteral("ui_"))) return raw;
    const auto& table = localizationTable();
    if (table.contains(key)) return table.value(key);

    QString readable = key;
    const QStringList prefixes = {
        QStringLiteral("ui_editor_script_snippet_"), QStringLiteral("ui_editor_properties_"),
        QStringLiteral("ui_browse_properties_"), QStringLiteral("ui_editor_general_"),
        QStringLiteral("ui_editor_effect_"), QStringLiteral("ui_editor_preset_"),
        QStringLiteral("ui_editor_"), QStringLiteral("ui_browse_"), QStringLiteral("ui_")};
    for (const QString& prefix : prefixes) {
        if (readable.startsWith(prefix)) {
            readable.remove(0, prefix.size());
            break;
        }
    }
    readable.replace('_', ' ');
    return readable.trimmed();
}

QString normalizedHtml(QString raw) {
    raw.replace(QStringLiteral("＜"), QStringLiteral("<"));
    raw.replace(QStringLiteral("＞"), QStringLiteral(">"));
    return raw;
}

bool isRichHtml(const QString& raw) {
    static const QRegularExpression expression(
        QStringLiteral("<\\s*(img|a|table|center|iframe|video)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(normalizedHtml(raw)).hasMatch();
}

QString plainText(const QString& raw) {
    const QString localized = resolveLocalization(raw);
    if (localized != raw) return localized;
    QString text = QTextDocumentFragment::fromHtml(normalizedHtml(raw)).toPlainText();
    text.replace(QRegularExpression(QStringLiteral("[ \\t]+")), QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral("\\n{3,}")), QStringLiteral("\n\n"));
    return text.trimmed();
}

QString propertyText(const QString& key, const ProjectProperty& property) {
    const QString raw = property.text.isEmpty() ? key : property.text;
    return plainText(raw);
}

void fitLabelToWidth(QLabel* label, int width) {
    label->setMaximumWidth(width);
    const int height = qMax(label->fontMetrics().height(), label->heightForWidth(width));
    label->setFixedHeight(height);
}

QLabel* wrappingLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    fitLabelToWidth(label, kPropertyWidth);
    return label;
}

QWidget* horizontalRow(QLabel* label, QWidget* editor, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    int editorWidth = editor->sizeHint().width();
    if (editor->maximumWidth() < QWIDGETSIZE_MAX) editorWidth = qMin(editorWidth, editor->maximumWidth());
    editorWidth = qMax(editorWidth, editor->minimumWidth());
    fitLabelToWidth(label, qMax(80, kPropertyWidth - editorWidth - layout->spacing()));
    editor->setSizePolicy(editor->sizePolicy().horizontalPolicy(), QSizePolicy::Fixed);
    layout->addWidget(label, 1);
    layout->addWidget(editor);
    row->setFixedHeight(qMax(label->height(), editor->sizeHint().height()));
    return row;
}

void fixWidgetToLayoutHeight(QWidget* widget, QLayout* layout) {
    layout->activate();
    widget->setFixedHeight(layout->sizeHint().height());
}

QColor parseColor(const QString& value) {
    const QStringList parts = value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() < 3) return Qt::white;
    return QColor::fromRgbF(qBound(0.0, parts.at(0).toDouble(), 1.0),
                            qBound(0.0, parts.at(1).toDouble(), 1.0),
                            qBound(0.0, parts.at(2).toDouble(), 1.0));
}

QString encodeColor(const QColor& color) {
    return QStringLiteral("%1 %2 %3")
        .arg(color.redF(), 0, 'f', 5)
        .arg(color.greenF(), 0, 'f', 5)
        .arg(color.blueF(), 0, 'f', 5);
}

void styleColorButton(QPushButton* button, const QColor& color) {
    const QColor border = color.lightnessF() > 0.55 ? QColor(QStringLiteral("#5a554f")) : QColor(QStringLiteral("#aaa49c"));
    button->setStyleSheet(QStringLiteral(
        "QPushButton { min-height: 0; background-color: %1; border: 1px solid %2; border-radius: 5px; padding: 0; }"
        "QPushButton:hover { border: 2px solid #0a84ff; }")
        .arg(color.name(QColor::HexRgb), border.name(QColor::HexRgb)));
}

class ExternalLinkPage final : public QWebEnginePage {
public:
    explicit ExternalLinkPage(QObject* parent = nullptr)
        : QWebEnginePage(parent) {
        setBackgroundColor(Qt::transparent);
    }

protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType type, bool isMainFrame) override {
        if (type == QWebEnginePage::NavigationTypeLinkClicked) {
            QDesktopServices::openUrl(url);
            return false;
        }
        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }
};

class RichHtmlView final : public QWebEngineView {
public:
    explicit RichHtmlView(const QString& html, QWidget* parent = nullptr)
        : QWebEngineView(parent) {
        setPage(new ExternalLinkPage(this));
        setContextMenuPolicy(Qt::NoContextMenu);
        setFocusPolicy(Qt::NoFocus);
        setMinimumWidth(0);
        setMaximumWidth(282);
        setFixedHeight(24);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        settings()->setAttribute(QWebEngineSettings::ShowScrollBars, false);
        settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);

        connect(this, &QWebEngineView::loadFinished, this, [this] {
            measure();
            QTimer::singleShot(450, this, [this] { measure(); });
            QTimer::singleShot(1400, this, [this] { measure(); });
        });

        const QString wrapped = QStringLiteral(R"HTML(
            <!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
            <style>
            :root { color-scheme: dark; }
            html, body { margin: 0; padding: 0; background: transparent; overflow: hidden; }
            html, body, body * { user-select: none; -webkit-user-drag: none; }
            body { color: #f1efec; font: 13px/1.45 "Noto Sans CJK SC", sans-serif;
                   overflow-wrap: anywhere; word-break: break-word; }
            a { color: #2997ff; text-decoration: none; }
            img { display: block; max-width: 100%; height: auto; margin: 4px 0; border-radius: 6px; }
            center { text-align: center; }
            p { margin: 4px 0; }
            table { max-width: 100%; }
            </style></head><body>%1</body></html>)HTML")
            .arg(normalizedHtml(html));
        setHtml(wrapped);
    }

private:
    void measure() {
        page()->runJavaScript(
            QStringLiteral("Math.max(document.body.scrollHeight, document.documentElement.scrollHeight)"),
            [this](const QVariant& result) {
                const int measured = qBound(24, qCeil(result.toDouble()), 360);
                if (measured == height()) return;
                setFixedHeight(measured);
                updateGeometry();
                QWidget* ancestor = parentWidget();
                while (ancestor) {
                    ancestor->updateGeometry();
                    ancestor = ancestor->parentWidget();
                }
            });
    }
};

} // namespace

PropertyEditorWidget::PropertyEditorWidget(QWidget* parent)
    : QWidget(parent) {
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(12);
    m_layout->setAlignment(Qt::AlignTop);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void PropertyEditorWidget::setWallpaper(const Wallpaper& wallpaper) {
    m_wallpaper = wallpaper;
    clear();

    QVector<QString> keys;
    for (auto it = wallpaper.project.properties.constBegin(); it != wallpaper.project.properties.constEnd(); ++it) {
        if (!it.value().presetOnly) keys.push_back(it.key());
    }
    std::sort(keys.begin(), keys.end(), [&](const QString& a, const QString& b) {
        const ProjectProperty pa = wallpaper.project.properties.value(a);
        const ProjectProperty pb = wallpaper.project.properties.value(b);
        const int oa = pa.order >= 0 ? pa.order : (pa.index >= 0 ? pa.index : INT_MAX);
        const int ob = pb.order >= 0 ? pb.order : (pb.index >= 0 ? pb.index : INT_MAX);
        if (oa != ob) return oa < ob;
        return a < b;
    });

    if (keys.isEmpty()) {
        auto* empty = wrappingLabel(QStringLiteral("此壁纸没有可调节的属性。"), this);
        empty->setStyleSheet(QStringLiteral("color: #aaa59f; font-size: 12px;"));
        m_layout->addWidget(empty);
    } else {
        for (const QString& key : keys) {
            QWidget* widget = widgetFor(key, wallpaper.project.properties.value(key));
            if (widget) m_layout->addWidget(widget);
        }
    }
    updateGeometry();
}

QWidget* PropertyEditorWidget::widgetFor(const QString& key, ProjectProperty property) {
    const QString labelText = propertyText(key, property);

    switch (property.propertyKind()) {
    case PropertyKind::Bool: {
        auto* check = new QCheckBox(this);
        check->setChecked(property.boolValue());
        connect(check, &QCheckBox::toggled, this, [this, key, property](bool checked) mutable {
            property.value = checked;
            emit propertyChanged(key, property);
        });
        return horizontalRow(wrappingLabel(labelText, this), check, this);
    }
    case PropertyKind::Slider: {
        const double minimum = property.hasMin ? property.min : 0.0;
        double maximum = property.hasMax ? property.max : 100.0;
        if (minimum >= maximum) maximum = minimum + 1.0;
        const double step = property.hasStep && property.step > 0.0
            ? property.step
            : (property.fraction ? (maximum - minimum) / 1000.0 : 1.0);
        const int positions = qBound(1, qRound((maximum - minimum) / qMax(step, 0.000001)), 10000);

        auto* row = new QWidget(this);
        row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto* header = new QHBoxLayout;
        header->setContentsMargins(0, 0, 0, 0);
        auto* label = wrappingLabel(labelText, row);
        auto* value = new QLabel(row);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setStyleSheet(QStringLiteral("color: #aaa59f; font-family: monospace; font-size: 12px;"));
        fitLabelToWidth(label, 220);
        header->addWidget(label, 1);
        header->addWidget(value);

        auto* slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(0, positions);
        slider->setValue(qBound(0, qRound((property.doubleValue() - minimum) / (maximum - minimum) * positions), positions));
        const auto numberFor = [minimum, maximum, positions, property](int position) {
            double number = minimum + (maximum - minimum) * position / positions;
            if (!property.fraction) number = qRound(number);
            return number;
        };
        const auto textFor = [property](double number) {
            return property.fraction ? QString::number(number, 'f', 2) : QString::number(qRound64(number));
        };
        value->setText(textFor(numberFor(slider->value())));
        connect(slider, &QSlider::valueChanged, this,
                [this, key, property, value, numberFor, textFor](int position) mutable {
                    const double number = numberFor(position);
                    value->setText(textFor(number));
                    property.value = number;
                    emit propertyChanged(key, property);
                });
        layout->addLayout(header);
        layout->addWidget(slider);
        fixWidgetToLayoutHeight(row, layout);
        return row;
    }
    case PropertyKind::Color: {
        auto* swatch = new QPushButton(this);
        swatch->setFixedSize(48, 26);
        const QColor initial = parseColor(property.stringValue());
        styleColorButton(swatch, initial);
        connect(swatch, &QPushButton::clicked, this, [this, key, property, swatch, initial]() mutable {
            const QColor selected = QColorDialog::getColor(parseColor(property.stringValue()), this,
                                                           QStringLiteral("选择颜色"), QColorDialog::DontUseNativeDialog);
            if (!selected.isValid()) return;
            property.value = encodeColor(selected);
            styleColorButton(swatch, selected);
            emit propertyChanged(key, property);
        });
        return horizontalRow(wrappingLabel(labelText, this), swatch, this);
    }
    case PropertyKind::Combo: {
        auto* combo = new QComboBox(this);
        combo->setMaximumWidth(170);
        int selected = 0;
        for (int i = 0; i < property.options.size(); ++i) {
            const auto& option = property.options.at(i);
            const QString optionLabel = plainText(option.label.isEmpty() ? option.value : option.label);
            combo->addItem(optionLabel, option.value);
            if (option.value == property.stringValue()) selected = i;
        }
        combo->setCurrentIndex(selected);
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key, property](int) mutable {
            property.value = combo->currentData().toString();
            emit propertyChanged(key, property);
        });
        return horizontalRow(wrappingLabel(labelText, this), combo, this);
    }
    case PropertyKind::TextInput: {
        auto* row = new QWidget(this);
        row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        layout->addWidget(wrappingLabel(labelText, row));
        auto* edit = new QLineEdit(property.stringValue(), row);
        connect(edit, &QLineEdit::textChanged, this, [this, key, property](const QString& text) mutable {
            property.value = text;
            emit propertyChanged(key, property);
        });
        layout->addWidget(edit);
        fixWidgetToLayoutHeight(row, layout);
        return row;
    }
    case PropertyKind::Text: {
        const QString raw = property.text.isEmpty() ? key : property.text;
        if (isRichHtml(raw)) return new RichHtmlView(raw, this);
        return wrappingLabel(labelText, this);
    }
    case PropertyKind::Group: {
        auto* group = new QWidget(this);
        group->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto* layout = new QVBoxLayout(group);
        layout->setContentsMargins(0, 8, 0, 0);
        layout->setSpacing(5);
        const QString raw = property.text.isEmpty() ? key : property.text;
        const bool rich = isRichHtml(raw);
        if (rich) {
            layout->addWidget(new RichHtmlView(raw, group));
        } else {
            auto* title = wrappingLabel(labelText, group);
            title->setStyleSheet(QStringLiteral("font-weight: 600;"));
            layout->addWidget(title);
        }
        auto* line = new QFrame(group);
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background: rgba(10, 132, 255, 128); border: 0;"));
        layout->addWidget(line);
        if (!rich) fixWidgetToLayoutHeight(group, layout);
        return group;
    }
    case PropertyKind::File:
    case PropertyKind::Directory:
    case PropertyKind::SceneTexture: {
        auto* row = new QWidget(this);
        row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        layout->addWidget(wrappingLabel(labelText, row));

        auto* picker = new QHBoxLayout;
        picker->setContentsMargins(0, 0, 0, 0);
        auto* path = new QLabel(row);
        path->setText(property.stringValue().isEmpty() ? QStringLiteral("未选择") : QFileInfo(property.stringValue()).fileName());
        path->setStyleSheet(QStringLiteral("color: #aaa59f; font-size: 12px;"));
        auto* clear = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear")), QString(), row);
        clear->setProperty("flatButton", true);
        clear->setToolTip(QStringLiteral("清除"));
        clear->setFixedWidth(32);
        clear->setVisible(!property.stringValue().isEmpty());
        auto* choose = new QPushButton(QStringLiteral("选择…"), row);
        picker->addWidget(path, 1);
        picker->addWidget(clear);
        picker->addWidget(choose);
        layout->addLayout(picker);

        connect(clear, &QPushButton::clicked, this, [this, key, property, path, clear]() mutable {
            property.value = QString();
            path->setText(QStringLiteral("未选择"));
            clear->hide();
            emit propertyChanged(key, property);
        });
        connect(choose, &QPushButton::clicked, this, [this, key, property, path, clear]() mutable {
            const QString selected = property.propertyKind() == PropertyKind::Directory
                ? QFileDialog::getExistingDirectory(this, QStringLiteral("选择文件夹"))
                : QFileDialog::getOpenFileName(this, QStringLiteral("选择文件"));
            if (selected.isEmpty()) return;
            property.value = selected;
            path->setText(QFileInfo(selected).fileName());
            clear->show();
            emit propertyChanged(key, property);
        });
        fixWidgetToLayoutHeight(row, layout);
        return row;
    }
    case PropertyKind::UserShortcut: {
        auto* edit = new QLineEdit(property.stringValue(), this);
        edit->setMaximumWidth(170);
        edit->setPlaceholderText(QStringLiteral("快捷方式"));
        connect(edit, &QLineEdit::textChanged, this, [this, key, property](const QString& text) mutable {
            property.value = text;
            emit propertyChanged(key, property);
        });
        return horizontalRow(wrappingLabel(labelText, this), edit, this);
    }
    case PropertyKind::Unknown:
        return nullptr;
    }
    return nullptr;
}

void PropertyEditorWidget::clear() {
    while (QLayoutItem* item = m_layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

} // namespace Mirage

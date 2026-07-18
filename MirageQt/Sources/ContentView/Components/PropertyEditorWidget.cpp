#include "ContentView/Components/PropertyEditorWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QVBoxLayout>

namespace Mirage {

PropertyEditorWidget::PropertyEditorWidget(QWidget* parent)
    : QWidget(parent) {
    auto* inner = new QWidget(this);
    m_form = new QFormLayout(inner);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(inner);
    scroll->setWidgetResizable(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(scroll);
}

void PropertyEditorWidget::setWallpaper(const Wallpaper& wallpaper) {
    m_wallpaper = wallpaper;
    clear();

    if (wallpaper.project.properties.isEmpty()) {
        m_form->addRow(new QLabel(QStringLiteral("该壁纸没有可编辑属性"), this));
        return;
    }

    QVector<QString> keys;
    for (auto it = wallpaper.project.properties.constBegin(); it != wallpaper.project.properties.constEnd(); ++it) {
        keys.push_back(it.key());
    }
    std::sort(keys.begin(), keys.end(), [&](const QString& a, const QString& b) {
        const ProjectProperty pa = wallpaper.project.properties.value(a);
        const ProjectProperty pb = wallpaper.project.properties.value(b);
        const int oa = pa.order >= 0 ? pa.order : (pa.index >= 0 ? pa.index : INT_MAX);
        const int ob = pb.order >= 0 ? pb.order : (pb.index >= 0 ? pb.index : INT_MAX);
        if (oa != ob) return oa < ob;
        return a < b;
    });

    for (const QString& key : keys) {
        const ProjectProperty property = wallpaper.project.properties.value(key);
        const QString label = property.text.isEmpty() ? key : property.text;
        m_form->addRow(label, editorFor(key, property));
    }
}

QWidget* PropertyEditorWidget::editorFor(const QString& key, ProjectProperty property) {
    switch (property.propertyKind()) {
    case PropertyKind::Bool: {
        auto* box = new QCheckBox(this);
        box->setChecked(property.boolValue());
        connect(box, &QCheckBox::toggled, this, [this, key, property](bool checked) mutable {
            property.value = checked;
            emit propertyChanged(key, property);
        });
        return box;
    }
    case PropertyKind::Slider: {
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(property.hasMin ? property.min : -100000.0,
                       property.hasMax ? property.max : 100000.0);
        spin->setSingleStep(property.hasStep ? property.step : 1.0);
        spin->setDecimals(property.fraction ? 3 : 2);
        spin->setValue(property.doubleValue());
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key, property](double value) mutable {
            property.value = value;
            emit propertyChanged(key, property);
        });
        return spin;
    }
    case PropertyKind::Combo: {
        auto* combo = new QComboBox(this);
        int selected = 0;
        for (int i = 0; i < property.options.size(); ++i) {
            const auto& option = property.options.at(i);
            combo->addItem(option.label.isEmpty() ? option.value : option.label, option.value);
            if (option.value == property.stringValue()) selected = i;
        }
        combo->setCurrentIndex(selected);
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key, property](int) mutable {
            property.value = combo->currentData().toString();
            emit propertyChanged(key, property);
        });
        return combo;
    }
    case PropertyKind::Color:
    case PropertyKind::TextInput:
    case PropertyKind::Text:
    case PropertyKind::File:
    case PropertyKind::Directory:
    case PropertyKind::SceneTexture:
    case PropertyKind::UserShortcut:
    case PropertyKind::Group:
    case PropertyKind::Unknown:
        break;
    }

    auto* edit = new QLineEdit(property.stringValue(), this);
    connect(edit, &QLineEdit::editingFinished, this, [this, edit, key, property]() mutable {
        property.value = edit->text();
        emit propertyChanged(key, property);
    });
    return edit;
}

void PropertyEditorWidget::clear() {
    while (QLayoutItem* item = m_form->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

} // namespace Mirage

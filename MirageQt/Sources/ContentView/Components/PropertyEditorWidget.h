#pragma once

#include "Services/WEConditionEvaluator.h"
#include "Services/WEProject.h"

#include <QHash>
#include <QVariant>
#include <QWidget>

class QVBoxLayout;

namespace Mirage {

class PropertyEditorWidget : public QWidget {
    Q_OBJECT

public:
    explicit PropertyEditorWidget(QWidget* parent = nullptr);

public slots:
    void setWallpaper(const Mirage::Wallpaper& wallpaper);
    void setPropertyOverrides(const QHash<QString, QVariant>& overrides);
    void setEffectiveProperties(const QHash<QString, ProjectProperty>& properties);

signals:
    void propertyChanged(const QString& key, const Mirage::ProjectProperty& property);

private:
    QWidget* widgetFor(const QString& key, ProjectProperty property);
    void rebuild();
    void clear();
    ProjectProperty effectiveProperty(const QString& key) const;
    void applyLocalOverride(const QString& key, const QVariant& value, bool rebuildNow = true);

    QVBoxLayout* m_layout = nullptr;
    Wallpaper m_wallpaper;
    QHash<QString, QVariant> m_overrides;
    QHash<QString, ProjectProperty> m_effectiveProperties;
    WEConditionEvaluator m_conditions;
};

} // namespace Mirage

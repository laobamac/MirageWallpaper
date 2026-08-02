#pragma once

#include "Services/WEProject.h"

#include <QHash>
#include <QJSEngine>
#include <QObject>
#include <QString>
#include <QVariant>

namespace Mirage {

class WEConditionEvaluator {
public:
    WEConditionEvaluator();

    void updateContext(const QHash<QString, ProjectProperty>& properties,
                       const QHash<QString, QVariant>& overrides);
    bool evaluate(const QString& condition) const;

private:
    QJSValue jsValueFor(const QVariant& value, PropertyKind kind) const;

    mutable QJSEngine m_engine;
};

} // namespace Mirage

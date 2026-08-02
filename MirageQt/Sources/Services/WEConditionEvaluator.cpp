#include "Services/WEConditionEvaluator.h"

namespace Mirage {

WEConditionEvaluator::WEConditionEvaluator() {
    // Empty/failing conditions resolve to visible so a stray expression never hides a row.
}

void WEConditionEvaluator::updateContext(const QHash<QString, ProjectProperty>& properties,
                                         const QHash<QString, QVariant>& overrides) {
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        const QVariant raw = overrides.contains(it.key()) ? overrides.value(it.key()) : it.value().value;
        QJSValue object = m_engine.newObject();
        object.setProperty(QStringLiteral("value"), jsValueFor(raw, it.value().propertyKind()));
        m_engine.globalObject().setProperty(it.key(), object);
    }
}

bool WEConditionEvaluator::evaluate(const QString& condition) const {
    const QString trimmed = condition.trimmed();
    if (trimmed.isEmpty()) return true;

    const QJSValue result = m_engine.evaluate(trimmed);
    if (result.isError() || result.isUndefined() || result.isNull()) return true;
    if (result.isBool()) return result.toBool();
    if (result.isNumber()) return result.toNumber() != 0.0;
    return result.toBool();
}

QJSValue WEConditionEvaluator::jsValueFor(const QVariant& value, PropertyKind kind) const {
    if (value.typeId() == QMetaType::Bool) return QJSValue(value.toBool());
    if (value.canConvert<double>() && value.typeId() != QMetaType::QString) return QJSValue(value.toDouble());

    const QString text = value.toString();
    if (kind == PropertyKind::Bool) {
        const QString lowered = text.trimmed().toLower();
        return QJSValue(lowered == QStringLiteral("true") || lowered == QStringLiteral("1")
                        || lowered == QStringLiteral("yes") || lowered == QStringLiteral("on"));
    }

    bool ok = false;
    const int asInt = text.toInt(&ok);
    if (ok) return QJSValue(asInt);
    const double asDouble = text.toDouble(&ok);
    if (ok) return QJSValue(asDouble);
    if (text == QStringLiteral("true")) return QJSValue(true);
    if (text == QStringLiteral("false")) return QJSValue(false);
    return QJSValue(text);
}

} // namespace Mirage

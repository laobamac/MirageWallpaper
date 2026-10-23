#include "Services/SecretService.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QMetaType>
#include <QDebug>

namespace Mirage {

const char* SecretService::kServiceName = "cn.laobamac.Mirage.SteamService";

namespace {
constexpr auto kBusName = "org.freedesktop.secrets";
constexpr auto kServicePath = "/org/freedesktop/secrets";
constexpr auto kServiceInterface = "org.freedesktop.Secret.Service";
constexpr auto kCollectionInterface = "org.freedesktop.Secret.Collection";
constexpr auto kItemInterface = "org.freedesktop.Secret.Item";
// 默认集合别名：桌面会话（gnome-keyring 等）通常在用户解锁时自动解锁
// 默认 collection，CreateItem/SearchItems 无需走 Prompt 交互流程。
constexpr auto kDefaultCollection = "/org/freedesktop/secrets/aliases/default";
// D-Bus 本机调用超时（毫秒）；避免密钥环守护进程挂起时阻塞登录。
constexpr int kBusTimeoutMs = 3000;
constexpr auto kContentType = "text/plain";
constexpr auto kServiceAttribute = "service";
constexpr auto kAccountAttribute = "account";
} // namespace

static bool callBus(const QDBusMessage& message, QDBusMessage& reply) {
    reply = QDBusConnection::sessionBus().call(message, QDBus::Block, kBusTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        // 诊断日志：密钥环 D-Bus 调用失败时记录接口与错误，便于定位
        // 会话恢复/写入链路（服务缺失、集合锁定、类型不匹配等）。
        qWarning().noquote() << QStringLiteral("[SecretService] %1 failed: %2 %3")
            .arg(message.interface() + QLatin1Char('.') + message.member(),
                 reply.errorName(), reply.errorMessage());
        return false;
    }
    return true;
}

// dict<string,string>（a{ss}）的显式构造：Secret Service 的 attributes 参数
// 是 a{ss}，而 QVariantMap 默认序列化为 a{sv}（变体字典），直接发送会
// 报 InvalidArgs 类型不匹配，必须用 QDBusArgument 构造 dict 类型。
static QDBusArgument stringMapArgument(const QVariantMap& map) {
    QDBusArgument argument;
    argument.beginMap(QMetaType::QString, QMetaType::QString);
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        argument.beginMapEntry();
        argument << it.key();
        argument << it.value().toString();
        argument.endMapEntry();
    }
    argument.endMap();
    return argument;
}

// 解析 dict<string,string>（a{ss}）：与发送对称，QVariantMap 不能直接
// 承载 a{ss}，需手动遍历 D-Bus dict。
static QVariantMap parseStringMap(const QDBusArgument& argument) {
    QVariantMap map;
    argument.beginMap();
    while (!argument.atEnd()) {
        QString key;
        QString value;
        argument.beginMapEntry();
        argument >> key >> value;
        argument.endMapEntry();
        map.insert(key, value);
    }
    argument.endMap();
    return map;
}

QString SecretService::defaultCollectionPath() {
    return QString::fromLatin1(kDefaultCollection);
}

bool SecretService::available() {
    QDBusObjectPath session;
    return openSession(session);
}

bool SecretService::openSession(QDBusObjectPath& session) {
    // OpenSession(algorithm, input) -> (output, result)。
    // input 参数类型是 Variant（v），QDBusVariant 强制按 v 序列化；
    // 直接传 QVariant(QString) 会被 Qt 展开成 string（s）导致类型不匹配。
    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kBusName), QLatin1String(kServicePath),
        QLatin1String(kServiceInterface), QStringLiteral("OpenSession"));
    call << QStringLiteral("plain") << QVariant::fromValue(QDBusVariant(QVariant(QString())));
    QDBusMessage reply;
    if (!callBus(call, reply)) return false;
    const QList<QVariant> args = reply.arguments();
    if (args.size() < 2) return false;
    session = qvariant_cast<QDBusObjectPath>(args.value(1));
    return !session.path().isEmpty();
}

QVariantMap SecretService::attributesFor(const QString& account) {
    QVariantMap attributes;
    attributes.insert(QLatin1String(kServiceAttribute), QString::fromLatin1(kServiceName));
    attributes.insert(QLatin1String(kAccountAttribute), account);
    return attributes;
}

QDBusArgument SecretService::secretArgument(const QDBusObjectPath& session,
                                            const QByteArray& value) {
    // Secret 结构 (oayays)：session、parameters（plain 为空）、value、content_type。
    QDBusArgument argument;
    argument.beginStructure();
    argument << session;
    argument << QByteArray();
    argument << value;
    argument << QString::fromLatin1(kContentType);
    argument.endStructure();
    return argument;
}

bool SecretService::write(const QString& account, const QByteArray& value) {
    QDBusObjectPath session;
    if (!openSession(session)) return false;
    // CreateItem(properties a{sv}, secret (oayays), replace b) -> (item, prompt)。
    // Attributes 是 a{ss}，必须用 QDBusArgument 显式构造（见 stringMapArgument）。
    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kBusName), defaultCollectionPath(),
        QLatin1String(kCollectionInterface), QStringLiteral("CreateItem"));
    QVariantMap properties;
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"), account);
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
                      QVariant::fromValue(stringMapArgument(attributesFor(account))));
    call << properties << QVariant::fromValue(secretArgument(session, value)) << true;
    QDBusMessage reply;
    if (!callBus(call, reply)) return false;
    const QList<QVariant> args = reply.arguments();
    if (args.size() < 1) return false;
    const QDBusObjectPath item = qvariant_cast<QDBusObjectPath>(args.value(0));
    // item 为 '/' 表示需要 Prompt 交互（集合锁定），视为不可用返回失败。
    const bool ok = item.path() != QLatin1String("/");
    qInfo().noquote() << QStringLiteral("[SecretService] write %1 -> %2").arg(account,
                                                                              ok ? "ok" : "locked");
    return ok;
}

bool SecretService::findItem(const QString& collection, const QString& account,
                             const QDBusObjectPath& session, QDBusObjectPath& item) {
    // SearchItems(attributes a{ss}) -> (items ao, locked ao)。
    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kBusName), collection, QLatin1String(kCollectionInterface),
        QStringLiteral("SearchItems"));
    call << QVariant::fromValue(stringMapArgument(attributesFor(account)));
    QDBusMessage reply;
    if (!callBus(call, reply)) return false;
    const QList<QVariant> args = reply.arguments();
    if (args.isEmpty()) return false;
    const QDBusArgument itemsArg = args.value(0).value<QDBusArgument>();
    QList<QDBusObjectPath> items;
    itemsArg >> items;
    if (items.isEmpty()) return false;
    item = items.first();
    return true;
}

bool SecretService::read(const QString& account, QByteArray& value) {
    QDBusObjectPath session;
    if (!openSession(session)) return false;
    QDBusObjectPath item;
    if (!findItem(defaultCollectionPath(), account, session, item)) {
        qInfo().noquote() << QStringLiteral("[SecretService] read %1 -> not found").arg(account);
        return false;
    }
    // GetSecret(session o) -> Secret (oayays)。
    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kBusName), item.path(), QLatin1String(kItemInterface),
        QStringLiteral("GetSecret"));
    call << session;
    QDBusMessage reply;
    if (!callBus(call, reply)) return false;
    if (reply.arguments().isEmpty()) return false;
    const QDBusArgument secret = reply.arguments().value(0).value<QDBusArgument>();
    secret.beginStructure();
    QDBusObjectPath sessionOut;
    QByteArray parameters;
    QByteArray secretValue;
    QString contentType;
    secret >> sessionOut >> parameters >> secretValue >> contentType;
    secret.endStructure();
    value = secretValue;
    qInfo().noquote() << QStringLiteral("[SecretService] read %1 -> %2 bytes").arg(
        account, QString::number(value.size()));
    return true;
}

bool SecretService::remove(const QString& account) {
    QDBusObjectPath session;
    if (!openSession(session)) return false;
    QDBusObjectPath item;
    if (!findItem(defaultCollectionPath(), account, session, item)) return true; // 不存在视为成功
    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kBusName), item.path(), QLatin1String(kItemInterface),
        QStringLiteral("Delete"));
    QDBusMessage reply;
    return callBus(call, reply);
}

QStringList SecretService::itemAccounts(const QString& collection,
                                        const QDBusObjectPath& session) {
    // 枚举集合全部 item：Collection.Items 属性 -> 每个 item 的
    // Item.Attributes 属性 -> 过滤 service 匹配的 account。
    QDBusMessage getItems = QDBusMessage::createMethodCall(
        QLatin1String(kBusName), collection,
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    getItems << QStringLiteral("org.freedesktop.Secret.Collection")
             << QStringLiteral("Items");
    QDBusMessage itemsReply;
    if (!callBus(getItems, itemsReply) || itemsReply.arguments().isEmpty()) return {};
    const QDBusVariant itemsVariant =
        qvariant_cast<QDBusVariant>(itemsReply.arguments().value(0));
    QList<QDBusObjectPath> items;
    itemsVariant.variant().value<QDBusArgument>() >> items;

    QStringList accounts;
    for (const QDBusObjectPath& itemPath : items) {
        QDBusMessage getAttrs = QDBusMessage::createMethodCall(
            QLatin1String(kBusName), itemPath.path(),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        getAttrs << QStringLiteral("org.freedesktop.Secret.Item")
                 << QStringLiteral("Attributes");
        QDBusMessage attrsReply;
        if (!callBus(getAttrs, attrsReply) || attrsReply.arguments().isEmpty()) continue;
        const QDBusVariant attrsVariant =
            qvariant_cast<QDBusVariant>(attrsReply.arguments().value(0));
        // Attributes 属性返回 a{ss}（dict），需手动解析（见 parseStringMap）。
        const QVariantMap attributes =
            parseStringMap(attrsVariant.variant().value<QDBusArgument>());
        if (attributes.value(QLatin1String(kServiceAttribute)).toString()
            == QString::fromLatin1(kServiceName)) {
            accounts.append(attributes.value(QLatin1String(kAccountAttribute)).toString());
        }
    }
    return accounts;
}

QStringList SecretService::accounts() {
    QDBusObjectPath session;
    if (!openSession(session)) return {};
    const QStringList accounts = itemAccounts(defaultCollectionPath(), session);
    qInfo().noquote() << QStringLiteral("[SecretService] accounts: %1").arg(accounts.join(", "));
    return accounts;
}

} // namespace Mirage

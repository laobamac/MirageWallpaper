// Copyright © 2026 王孝慈. All rights reserved.
#include "UiBridge.h"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QTimer>

#include <algorithm>

UiBridge::UiBridge(QObject* parent) : QObject(parent), m_language(m_settings.value("language", "system").toString()) {
    readTranslations();
    m_conditionTimeout.setSingleShot(true);
    connect(&m_conditionTimeout, &QTimer::timeout, &m_conditionProcess, &QProcess::kill);
    connect(&m_conditionProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_conditionTimeout.stop();
        if (code == 0 && status == QProcess::NormalExit) {
            m_conditions = QJsonDocument::fromJson(m_conditionProcess.readAllStandardOutput()).object().toVariantMap();
            emit conditionsChanged();
        }
    });
    connect(&m_scanTimer, &QTimer::timeout, this, [this]() {
        if (!m_scanQueue.isEmpty()) scan(m_scanQueue.takeFirst());
        if (m_scanQueue.isEmpty()) {
            m_scanTimer.stop(); m_busy = false; emit libraryChanged(); emit busyChanged();
        }
    });
    connect(qApp, &QGuiApplication::screenAdded, this, &UiBridge::screensChanged);
    connect(qApp, &QGuiApplication::screenRemoved, this, &UiBridge::screensChanged);
    QTimer::singleShot(0, this, &UiBridge::refresh);
}
UiBridge::~UiBridge() {
    if (m_conditionProcess.state() != QProcess::NotRunning) {
        m_conditionProcess.kill();
        m_conditionProcess.waitForFinished(1000);
    }
}
QString UiBridge::language() const { return m_language; }
void UiBridge::setLanguage(const QString& value) {
    if (m_language == value) return;
    m_language = value;
    readTranslations();
    emit languageChanged();
}
QVariantList UiBridge::library() const { return m_library; }
bool UiBridge::busy() const { return m_busy; }
QStringList UiBridge::sources() const { return m_settings.value("sources").toStringList(); }
QVariantList UiBridge::screens() const {
    QVariantList result;
    for (QScreen* screen : QGuiApplication::screens()) {
        result.append(QVariantMap{{"name", screen->name()}, {"width", screen->size().width()},
            {"height", screen->size().height()}, {"scale", screen->devicePixelRatio()},
            {"primary", screen == QGuiApplication::primaryScreen()}});
    }
    return result;
}
QString UiBridge::text(const QString& key) const { return m_text.value(key, key); }
QString UiBridge::propertyText(const QString& key) const { return m_propertyText.value(key, text(key)); }
QVariant UiBridge::load(const QString& key, const QVariant& fallback) const { return m_settings.value(key, fallback); }
void UiBridge::save(const QString& key, const QVariant& value) { m_settings.setValue(key, value); }
void UiBridge::readTranslations() {
    QString locale = m_language;
    if (locale == "system") {
        const QString system = QLocale::system().name();
        locale = system.startsWith("zh") ? ((system.contains("TW") || system.contains("HK")) ? "zh-Hant" : "zh-Hans") : "en";
    }
    m_text.clear();
    QFile strings(":/translations/" + locale + ".lproj/Localizable.strings");
    if (strings.open(QIODevice::ReadOnly)) {
        const QString data = QString::fromUtf8(strings.readAll());
        const QRegularExpression expression(QStringLiteral("\"((?:\\\\.|[^\"\\\\])*)\"\\s*=\\s*\"((?:\\\\.|[^\"\\\\])*)\"\\s*;"));
        auto matches = expression.globalMatch(data);
        while (matches.hasNext()) {
            const auto match = matches.next();
            auto decode = [](QString s) { return s.replace("\\n", "\n").replace("\\\"", "\"").replace("\\\\", "\\"); };
            m_text.insert(decode(match.captured(1)), decode(match.captured(2)));
        }
    }
    QFile extra(":/Resources/Translations.json");
    if (extra.open(QIODevice::ReadOnly)) {
        const auto table = QJsonDocument::fromJson(extra.readAll()).object();
        for (auto it = table.begin(); it != table.end(); ++it) {
            const QString translated = it.value().toObject().value(locale).toString();
            if (!translated.isEmpty()) m_text.insert(it.key(), translated);
        }
    }
    m_propertyText.clear();
    QFile properties(locale == "zh-Hant" ? ":/translations/ui_zh-cht.json" : ":/translations/ui_zh-chs.json");
    if (locale != "en" && properties.open(QIODevice::ReadOnly)) {
        const auto table = QJsonDocument::fromJson(properties.readAll()).object();
        for (auto it = table.begin(); it != table.end(); ++it) if (it.value().isString()) m_propertyText.insert(it.key(), it.value().toString());
    }
}
void UiBridge::addSource(const QUrl& url) {
    if (!url.isLocalFile()) return;
    QStringList paths = sources();
    const QString path = QFileInfo(url.toLocalFile()).absoluteFilePath();
    if (!paths.contains(path)) paths.append(path);
    m_settings.setValue("sources", paths);
    refresh();
}
void UiBridge::removeSource(const QString& path) {
    QStringList paths = sources(); paths.removeAll(path); m_settings.setValue("sources", paths); refresh();
}
void UiBridge::refresh() {
    if (m_busy) return;
    m_busy = true; emit busyChanged();
    m_library.clear();
    m_scanQueue = sources();
    m_scanTimer.start(0);
}
void UiBridge::scan(const QString& path) {
    if (m_settings.value("hiddenWallpapers").toStringList().contains(path)) return;
    const QFileInfo info(path);
    const QStringList videos{"mp4", "webm", "mkv", "mov", "m4v"};
    if (info.isFile() && videos.contains(info.suffix().toLower())) {
        m_library.append(QVariantMap{{"id", info.absoluteFilePath()}, {"title", info.completeBaseName()},
            {"type", "video"}, {"preview", ""}, {"path", info.absolutePath()}, {"properties", QVariantList()},
            {"tags", QStringList()}, {"bytes", info.size()}, {"modified", info.lastModified().toMSecsSinceEpoch()}});
        return;
    }
    if (!info.isDir()) return;
    QFile project(QDir(path).filePath("project.json"));
    if (!project.exists()) {
        for (const QFileInfo& child : QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
            if (QFileInfo::exists(QDir(child.absoluteFilePath()).filePath("project.json"))) m_scanQueue.append(child.absoluteFilePath());
        }
        return;
    }
    if (!project.open(QIODevice::ReadOnly)) { emit error(project.errorString()); return; }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(project.readAll(), &parseError);
    if (!document.isObject()) { emit error(path + ": " + parseError.errorString()); return; }
    const auto object = document.object();
    QVariantMap wallpaper = object.toVariantMap();
    wallpaper["id"] = info.absoluteFilePath();
    wallpaper["path"] = info.absoluteFilePath();
    wallpaper["title"] = object.value("title").toString(info.fileName());
    wallpaper["preview"] = object.value("preview").toString().isEmpty() ? QString() : QUrl::fromLocalFile(QDir(path).filePath(object.value("preview").toString())).toString();
    wallpaper["modified"] = info.lastModified().toMSecsSinceEpoch();
    const auto propertyObject = object.value("general").toObject().value("properties").toObject();
    QVariantList properties;
    for (auto it = propertyObject.begin(); it != propertyObject.end(); ++it) {
        QVariantMap property = it.value().toObject().toVariantMap();
        property["key"] = it.key(); properties.append(property);
    }
    std::stable_sort(properties.begin(), properties.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("order", 0).toDouble() < b.toMap().value("order", 0).toDouble();
    });
    wallpaper["properties"] = properties;
    for (const QVariant& existing : std::as_const(m_library)) if (existing.toMap().value("id") == wallpaper.value("id")) return;
    m_library.append(wallpaper);
}
void UiBridge::copyText(const QString& value) { QGuiApplication::clipboard()->setText(value); }
void UiBridge::openFolder(const QString& path) { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); }
bool UiBridge::exportPreset(const QUrl& url, const QVariantMap& values) {
    if (!url.isLocalFile()) return false;
    QSaveFile file(url.toLocalFile());
    const QByteArray data = QJsonDocument(QJsonObject::fromVariantMap(values)).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) { emit error(file.errorString()); return false; }
    return true;
}
QVariantMap UiBridge::importPreset(const QUrl& url) {
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) { emit error(file.errorString()); return {}; }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) { emit error(parseError.errorString()); return {}; }
    return document.object().toVariantMap();
}
void UiBridge::hideWallpaper(const QString& id) {
    QStringList hidden = m_settings.value("hiddenWallpapers").toStringList();
    if (!hidden.contains(id)) hidden.append(id);
    m_settings.setValue("hiddenWallpapers", hidden);
    refresh();
}
QVariantMap UiBridge::conditions() const { return m_conditions; }
void UiBridge::evaluateConditions(const QVariantList& properties, const QVariantMap& overrides) {
    if (m_conditionProcess.state() != QProcess::NotRunning) {
        m_conditionProcess.kill();
        m_conditionProcess.waitForFinished(100);
    }
    QJsonArray expressions;
    QJsonObject values;
    for (const QVariant& item : properties) {
        const auto property = item.toMap();
        const QString key = property.value("key").toString();
        QVariant value = overrides.value(key, property.value("value"));
        if (value.metaType().id() == QMetaType::QString) {
            const QString string = value.toString();
            bool numeric = false;
            const double number = string.toDouble(&numeric);
            if (numeric) value = number;
            else if (string == "true" || string == "false") value = string == "true";
        }
        values[key] = QJsonObject{{"value", QJsonValue::fromVariant(value)}};
        const QString expression = property.value("condition").toString();
        if (!expression.isEmpty()) expressions.append(expression);
        for (const QVariant& option : property.value("options").toList()) {
            const QString condition = option.toMap().value("condition").toString();
            if (!condition.isEmpty()) expressions.append(condition);
        }
    }
    m_conditions.clear();
    emit conditionsChanged();
    if (expressions.isEmpty()) return;
    m_conditionProcess.start(QCoreApplication::applicationFilePath(), {"--condition-worker"});
    const QByteArray data = QJsonDocument(QJsonObject{{"expressions", expressions}, {"values", values}}).toJson(QJsonDocument::Compact);
    m_conditionProcess.write(data);
    m_conditionProcess.closeWriteChannel();
    m_conditionTimeout.start(1000);
}

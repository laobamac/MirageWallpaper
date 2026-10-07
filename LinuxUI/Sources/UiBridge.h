// Copyright © 2026 王孝慈. All rights reserved.
#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QSettings>
#include <QUrl>
#include <QHash>
#include <QProcess>
#include <QTimer>

class UiBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QVariantList library READ library NOTIFY libraryChanged)
    Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged)
    Q_PROPERTY(QStringList sources READ sources NOTIFY libraryChanged)
    Q_PROPERTY(QVariantMap conditions READ conditions NOTIFY conditionsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
public:
    explicit UiBridge(QObject* parent = nullptr);
    ~UiBridge() override;
    QString language() const;
    void setLanguage(const QString& value);
    QVariantList library() const;
    QVariantList screens() const;
    QStringList sources() const;
    bool busy() const;
    Q_INVOKABLE QString text(const QString& key) const;
    Q_INVOKABLE QString propertyText(const QString& key) const;
    Q_INVOKABLE QVariant load(const QString& key, const QVariant& fallback = QVariant()) const;
    Q_INVOKABLE void save(const QString& key, const QVariant& value);
    Q_INVOKABLE void addSource(const QUrl& url);
    Q_INVOKABLE void removeSource(const QString& path);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void copyText(const QString& value);
    Q_INVOKABLE void openFolder(const QString& path);
    Q_INVOKABLE bool exportPreset(const QUrl& url, const QVariantMap& values);
    Q_INVOKABLE QVariantMap importPreset(const QUrl& url);
    Q_INVOKABLE void hideWallpaper(const QString& id);
    Q_INVOKABLE void evaluateConditions(const QVariantList& properties, const QVariantMap& overrides);
    QVariantMap conditions() const;
signals:
    void conditionsChanged();
    void languageChanged();
    void libraryChanged();
    void screensChanged();
    void busyChanged();
    void error(const QString& message);
private:
    QSettings m_settings;
    QString m_language;
    QVariantList m_library;
    QHash<QString, QString> m_text;
    QHash<QString, QString> m_propertyText;
    bool m_busy = false;
    QProcess m_conditionProcess;
    QTimer m_conditionTimeout;
    QTimer m_scanTimer;
    QStringList m_scanQueue;
    QVariantMap m_conditions;
    void readTranslations();
    void scan(const QString& path);
};

// Copyright © 2026 王孝慈. All rights reserved.
#pragma once

#include <QObject>
#include <QUrl>
#include <QVariantMap>

// The QML-facing controller owns business state; this adapter contains only
// local file and desktop integration that QML cannot perform directly.
class UiBridge final : public QObject {
    Q_OBJECT

public:
    explicit UiBridge(QObject* parent = nullptr);

    // System-boundary operations stay here because QML cannot open native folders or access the platform clipboard.
    // Methods execute on the GUI thread; string, URL, and map values are borrowed for the call only.
    Q_INVOKABLE void openFolder(const QString& path);
    Q_INVOKABLE void copyText(const QString& text);

    // Preset files remain local JSON and use atomic writes; invalid URLs or I/O/parse failures emit error().
    Q_INVOKABLE bool exportPreset(const QUrl& url, const QVariantMap& values);
    Q_INVOKABLE QVariantMap importPreset(const QUrl& url);

signals:
    void error(const QString& message);
};

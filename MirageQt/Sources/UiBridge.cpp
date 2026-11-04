// Copyright © 2026 王孝慈. All rights reserved.
#include "UiBridge.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

UiBridge::UiBridge(QObject* parent)
    : QObject(parent) {}

void UiBridge::openFolder(const QString& path) {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        emit error(QStringLiteral("无法打开目录：%1").arg(path));
    }
}

void UiBridge::copyText(const QString& text) {
    QGuiApplication::clipboard()->setText(text);
}

bool UiBridge::exportPreset(const QUrl& url, const QVariantMap& values) {
    if (!url.isLocalFile()) {
        emit error(QStringLiteral("预设必须保存到本地文件"));
        return false;
    }
    QSaveFile file(url.toLocalFile());
    const QByteArray data = QJsonDocument(QJsonObject::fromVariantMap(values)).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        emit error(file.errorString());
        return false;
    }
    return true;
}

QVariantMap UiBridge::importPreset(const QUrl& url) {
    if (!url.isLocalFile()) {
        emit error(QStringLiteral("预设必须从本地文件读取"));
        return {};
    }
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        emit error(file.errorString());
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        emit error(parseError.errorString());
        return {};
    }
    return document.object().toVariantMap();
}

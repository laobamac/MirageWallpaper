#pragma once

#include <QJsonObject>
#include <QString>

// Linux manifest value object. The parser is intentionally strict about the
// documented project.json fields so malformed wallpapers fail at startup.
class WRManifest final {
public:
    static WRManifest loadFromDirectory(const QString& directory, QString* error);

    QString workshopDirectory() const { return m_directory; }
    QString entryHtml() const { return m_entryHtml; }
    QString title() const { return m_title; }
    QJsonObject userProperties() const { return m_userProperties; }

private:
    QString m_directory;
    QString m_entryHtml;
    QString m_title;
    QJsonObject m_userProperties;
};

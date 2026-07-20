#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace Mirage {

enum class WallpaperKind {
    Scene,
    Web,
    Video,
    Unsupported,
};

enum class PropertyKind {
    Bool,
    Slider,
    Color,
    Combo,
    TextInput,
    Text,
    Group,
    File,
    Directory,
    SceneTexture,
    UserShortcut,
    Unknown,
};

enum class PresetStatus {
    NotPreset,
    Resolved,
    MissingDependency,
    InvalidDependency,
    CircularDependency,
};

struct ProjectPropertyOption {
    QString label;
    QString value;
    QString condition;
};

struct ProjectProperty {
    QString condition;
    int index = -1;
    QVector<ProjectPropertyOption> options;
    int order = -1;
    double min = 0.0;
    double max = 0.0;
    double step = 0.0;
    bool hasMin = false;
    bool hasMax = false;
    bool hasStep = false;
    bool fraction = false;
    QString mode;
    bool presetOnly = false;

    QString text;
    QString type = QStringLiteral("text");
    QVariant value;

    PropertyKind propertyKind() const;
    bool boolValue() const;
    double doubleValue() const;
    QString stringValue() const;
};

struct Project {
    bool approved = false;
    bool hasApproved = false;
    QString author;
    QString contentRating;
    QString dependency;
    QString description;
    QString file;
    QHash<QString, ProjectProperty> properties;
    QHash<QString, QVariant> preset;
    QString preview;
    QStringList tags;
    QString title = QStringLiteral("未命名");
    QString visibility;
    QString workshopId;
    QString workshopUrl;
    QString type;
    int version = 0;
    bool hasVersion = false;

    WallpaperKind kind() const;
    bool isWorkshopPreset() const;
    QString resolvedAuthor() const;
    Project applyingPreset(const Project& presetProject) const;

    static Project invalid();
    static Project fromJson(const QJsonObject& object);
    QJsonObject toJson() const;
};

struct Wallpaper {
    QString wallpaperDirectory;
    QString renderDirectory;
    QStringList assetOverlayDirectories;
    Project project;
    QString presetDependency;
    PresetStatus presetStatus = PresetStatus::NotPreset;

    QString id() const;
    QString entryPath() const;
    QString resolvedEntryPath() const;
    QString previewPath() const;
    WallpaperKind kind() const;
    bool isPreset() const;
    bool isValid() const;
    QString presetStatusDescription() const;
};

QString wallpaperKindName(WallpaperKind kind);
QString wallpaperKindKey(WallpaperKind kind);
WallpaperKind wallpaperKindFromString(const QString& raw);

QString presetStatusKey(PresetStatus status);
PresetStatus presetStatusFromKey(const QString& raw);

QVariant jsonValueToVariant(const QJsonValue& value);
QJsonValue variantToJsonValue(const QVariant& value);
ProjectProperty propertyFromJson(const QJsonObject& object);
QJsonObject propertyToJson(const ProjectProperty& property);

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::ProjectProperty)
Q_DECLARE_METATYPE(Mirage::Wallpaper)

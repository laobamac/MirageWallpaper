#include "Services/WEProject.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

namespace Mirage {
namespace {

QString jsonStringOrNumber(const QJsonValue& value) {
    if (value.isString()) return value.toString();
    if (value.isDouble()) return QString::number(qint64(value.toDouble()));
    return {};
}

QString trimmed(const QString& value) {
    return value.trimmed();
}

QString inferPropertyType(const QVariant& value) {
    if (value.typeId() == QMetaType::Bool) return QStringLiteral("bool");
    if (value.canConvert<double>() && value.typeId() != QMetaType::QString) return QStringLiteral("slider");
    return QStringLiteral("textinput");
}

QString extractBracketAuthor(const QString& value) {
    static const QRegularExpression re(QStringLiteral("^\\s*[\\[【]([^\\]】]{1,40})[\\]】]"));
    const auto match = re.match(value);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QString extractLabeledAuthor(const QString& value) {
    const QVector<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral("作者[:：]\\s*([^\\n\\r，,。;；]{1,40})")),
        QRegularExpression(QStringLiteral("\\bby\\s+([^\\n\\r,;]{1,40})"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("author[:：]\\s*([^\\n\\r,;]{1,40})"),
                           QRegularExpression::CaseInsensitiveOption),
    };

    for (const auto& re : patterns) {
        const auto match = re.match(value);
        if (match.hasMatch()) {
            const QString author = match.captured(1).trimmed();
            if (!author.isEmpty()) return author;
        }
    }
    return {};
}

} // namespace

PropertyKind ProjectProperty::propertyKind() const {
    const QString key = type.toLower();
    if (key == "bool") return PropertyKind::Bool;
    if (key == "slider") return PropertyKind::Slider;
    if (key == "color") return PropertyKind::Color;
    if (key == "combo") return PropertyKind::Combo;
    if (key == "textinput") return PropertyKind::TextInput;
    if (key == "text") return PropertyKind::Text;
    if (key == "group") return PropertyKind::Group;
    if (key == "file") return PropertyKind::File;
    if (key == "directory") return PropertyKind::Directory;
    if (key == "scenetexture") return PropertyKind::SceneTexture;
    if (key == "usershortcut") return PropertyKind::UserShortcut;
    return PropertyKind::Unknown;
}

bool ProjectProperty::boolValue() const {
    if (value.typeId() == QMetaType::Bool) return value.toBool();
    if (value.canConvert<double>() && value.typeId() != QMetaType::QString) return value.toDouble() != 0.0;
    const QString textValue = value.toString().trimmed().toLower();
    return textValue == "true" || textValue == "yes" || textValue == "1" || textValue == "on";
}

double ProjectProperty::doubleValue() const {
    if (value.typeId() == QMetaType::Bool) return value.toBool() ? 1.0 : 0.0;
    bool ok = false;
    const double out = value.toDouble(&ok);
    return ok ? out : 0.0;
}

QString ProjectProperty::stringValue() const {
    if (value.typeId() == QMetaType::Bool) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.canConvert<double>() && value.typeId() != QMetaType::QString) {
        const double number = value.toDouble();
        if (qAbs(number - qRound64(number)) < 0.0000001 && qAbs(number) < 1e15) {
            return QString::number(qRound64(number));
        }
        return QString::number(number, 'g', 15);
    }
    return value.toString();
}

WallpaperKind Project::kind() const {
    return wallpaperKindFromString(type);
}

bool Project::isWorkshopPreset() const {
    return !dependency.isEmpty() && !preset.isEmpty();
}

QString Project::resolvedAuthor() const {
    const QString explicitAuthor = trimmed(author);
    if (!explicitAuthor.isEmpty()) return explicitAuthor;
    const QString fromTitle = extractBracketAuthor(title);
    if (!fromTitle.isEmpty()) return fromTitle;
    return extractLabeledAuthor(description);
}

Project Project::applyingPreset(const Project& presetProject) const {
    Project result = *this;

    for (auto it = presetProject.preset.constBegin(); it != presetProject.preset.constEnd(); ++it) {
        ProjectProperty property = result.properties.value(it.key());
        if (property.type.isEmpty()) property.type = inferPropertyType(it.value());
        property.value = it.value();
        property.presetOnly = !result.properties.contains(it.key());
        result.properties.insert(it.key(), property);
    }

    result.approved = presetProject.hasApproved ? presetProject.approved : result.approved;
    result.hasApproved = presetProject.hasApproved || result.hasApproved;
    if (!presetProject.author.isEmpty()) result.author = presetProject.author;
    if (!presetProject.contentRating.isEmpty()) result.contentRating = presetProject.contentRating;
    result.dependency = presetProject.dependency;
    if (!presetProject.description.isEmpty()) result.description = presetProject.description;
    result.preset = presetProject.preset;
    if (!presetProject.preview.isEmpty()) result.preview = presetProject.preview;
    if (!presetProject.tags.isEmpty()) result.tags = presetProject.tags;
    if (!presetProject.title.isEmpty()) result.title = presetProject.title;
    if (!presetProject.visibility.isEmpty()) result.visibility = presetProject.visibility;
    if (!presetProject.workshopId.isEmpty()) result.workshopId = presetProject.workshopId;
    if (!presetProject.workshopUrl.isEmpty()) result.workshopUrl = presetProject.workshopUrl;
    return result;
}

Project Project::invalid() {
    Project project;
    project.title = QStringLiteral("未知");
    project.type = QStringLiteral("video");
    return project;
}

Project Project::fromJson(const QJsonObject& object) {
    Project project;
    if (object.contains("approved")) {
        project.approved = object.value("approved").toBool();
        project.hasApproved = true;
    }
    project.author = object.value("author").toString();
    project.contentRating = object.value("contentrating").toString();
    project.dependency = jsonStringOrNumber(object.value("dependency"));
    project.description = object.value("description").toString();
    project.file = object.value("file").toString();
    project.preview = object.value("preview").toString();
    project.title = object.value("title").toString(QStringLiteral("未命名"));
    project.visibility = object.value("visibility").toString();
    project.workshopId = jsonStringOrNumber(object.value("workshopid"));
    project.workshopUrl = object.value("workshopurl").toString();
    project.type = object.value("type").toString();
    if (object.contains("version")) {
        project.version = object.value("version").toInt();
        project.hasVersion = true;
    }

    const auto tags = object.value("tags").toArray();
    for (const auto& tag : tags) project.tags.push_back(tag.toString());

    const auto general = object.value("general").toObject();
    const auto props = general.value("properties").toObject();
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        if (it.value().isObject()) project.properties.insert(it.key(), propertyFromJson(it.value().toObject()));
    }

    const auto presetObject = object.value("preset").toObject();
    for (auto it = presetObject.constBegin(); it != presetObject.constEnd(); ++it) {
        project.preset.insert(it.key(), jsonValueToVariant(it.value()));
    }

    return project;
}

QJsonObject Project::toJson() const {
    QJsonObject object;
    if (hasApproved) object.insert("approved", approved);
    if (!author.isEmpty()) object.insert("author", author);
    if (!contentRating.isEmpty()) object.insert("contentrating", contentRating);
    if (!dependency.isEmpty()) object.insert("dependency", dependency);
    if (!description.isEmpty()) object.insert("description", description);
    if (!file.isEmpty()) object.insert("file", file);
    if (!properties.isEmpty()) {
        QJsonObject props;
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
            props.insert(it.key(), propertyToJson(it.value()));
        }
        QJsonObject general;
        general.insert("properties", props);
        object.insert("general", general);
    }
    if (!preset.isEmpty()) {
        QJsonObject presetObject;
        for (auto it = preset.constBegin(); it != preset.constEnd(); ++it) {
            presetObject.insert(it.key(), variantToJsonValue(it.value()));
        }
        object.insert("preset", presetObject);
    }
    if (!preview.isEmpty()) object.insert("preview", preview);
    if (!tags.isEmpty()) {
        QJsonArray array;
        for (const QString& tag : tags) array.push_back(tag);
        object.insert("tags", array);
    }
    object.insert("title", title);
    if (!visibility.isEmpty()) object.insert("visibility", visibility);
    if (!workshopId.isEmpty()) object.insert("workshopid", workshopId);
    if (!workshopUrl.isEmpty()) object.insert("workshopurl", workshopUrl);
    if (!type.isEmpty()) object.insert("type", type);
    if (hasVersion) object.insert("version", version);
    return object;
}

QString Wallpaper::id() const {
    return QDir::cleanPath(wallpaperDirectory);
}

QString Wallpaper::entryPath() const {
    return QDir::cleanPath(renderDirectory + "/" + project.file);
}

QString Wallpaper::resolvedEntryPath() const {
    if (kind() == WallpaperKind::Scene) {
        const QString package = QDir::cleanPath(renderDirectory + "/scene.pkg");
        if (QFileInfo::exists(package)) return package;
    }
    return entryPath();
}

QString Wallpaper::previewPath() const {
    return QDir::cleanPath(wallpaperDirectory + "/" + project.preview);
}

WallpaperKind Wallpaper::kind() const {
    return project.kind();
}

bool Wallpaper::isPreset() const {
    return !presetDependency.isEmpty();
}

bool Wallpaper::isValid() const {
    return !project.file.isEmpty() &&
           kind() != WallpaperKind::Unsupported &&
           (!isPreset() || presetStatus == PresetStatus::Resolved);
}

QString Wallpaper::presetStatusDescription() const {
    switch (presetStatus) {
    case PresetStatus::NotPreset:
    case PresetStatus::Resolved:
        return {};
    case PresetStatus::MissingDependency:
        return QStringLiteral("缺少基础壁纸");
    case PresetStatus::InvalidDependency:
        return QStringLiteral("基础壁纸无效");
    case PresetStatus::CircularDependency:
        return QStringLiteral("预设循环依赖");
    }
    return {};
}

QString wallpaperKindName(WallpaperKind kind) {
    switch (kind) {
    case WallpaperKind::Scene: return QStringLiteral("场景");
    case WallpaperKind::Web: return QStringLiteral("网页");
    case WallpaperKind::Video: return QStringLiteral("视频");
    case WallpaperKind::Unsupported: return QStringLiteral("不支持");
    }
    return QStringLiteral("不支持");
}

QString wallpaperKindKey(WallpaperKind kind) {
    switch (kind) {
    case WallpaperKind::Scene: return QStringLiteral("scene");
    case WallpaperKind::Web: return QStringLiteral("web");
    case WallpaperKind::Video: return QStringLiteral("video");
    case WallpaperKind::Unsupported: return QStringLiteral("unsupported");
    }
    return QStringLiteral("unsupported");
}

WallpaperKind wallpaperKindFromString(const QString& raw) {
    const QString key = raw.toLower();
    if (key == "scene") return WallpaperKind::Scene;
    if (key == "web") return WallpaperKind::Web;
    if (key == "video") return WallpaperKind::Video;
    return WallpaperKind::Unsupported;
}

QString presetStatusKey(PresetStatus status) {
    switch (status) {
    case PresetStatus::NotPreset: return QStringLiteral("notPreset");
    case PresetStatus::Resolved: return QStringLiteral("resolved");
    case PresetStatus::MissingDependency: return QStringLiteral("missingDependency");
    case PresetStatus::InvalidDependency: return QStringLiteral("invalidDependency");
    case PresetStatus::CircularDependency: return QStringLiteral("circularDependency");
    }
    return QStringLiteral("notPreset");
}

PresetStatus presetStatusFromKey(const QString& raw) {
    if (raw == "resolved") return PresetStatus::Resolved;
    if (raw == "missingDependency") return PresetStatus::MissingDependency;
    if (raw == "invalidDependency") return PresetStatus::InvalidDependency;
    if (raw == "circularDependency") return PresetStatus::CircularDependency;
    return PresetStatus::NotPreset;
}

QVariant jsonValueToVariant(const QJsonValue& value) {
    if (value.isBool()) return value.toBool();
    if (value.isDouble()) return value.toDouble();
    if (value.isString()) return value.toString();
    if (value.isArray()) return value.toArray().toVariantList();
    if (value.isObject()) return value.toObject().toVariantMap();
    return {};
}

QJsonValue variantToJsonValue(const QVariant& value) {
    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        return QJsonValue::fromVariant(value);
    case QMetaType::QString:
        return value.toString();
    default:
        return QJsonValue::fromVariant(value);
    }
}

ProjectProperty propertyFromJson(const QJsonObject& object) {
    ProjectProperty property;
    property.condition = object.value("condition").toString();
    if (object.contains("index")) property.index = object.value("index").toInt();
    if (object.contains("order")) property.order = object.value("order").toInt();
    if (object.contains("min")) {
        property.min = object.value("min").toDouble();
        property.hasMin = true;
    }
    if (object.contains("max")) {
        property.max = object.value("max").toDouble();
        property.hasMax = true;
    }
    if (object.contains("step")) {
        property.step = object.value("step").toDouble();
        property.hasStep = true;
    }
    property.fraction = object.value("fraction").toBool(false);
    property.mode = object.value("mode").toString();
    property.presetOnly = object.value("_miragePresetOnly").toBool(false);
    property.text = object.value("text").toString();
    property.type = object.value("type").toString(QStringLiteral("text"));
    property.value = jsonValueToVariant(object.value("value"));

    const auto options = object.value("options").toArray();
    for (const auto& item : options) {
        const QJsonObject optObject = item.toObject();
        ProjectPropertyOption opt;
        opt.label = optObject.value("label").toString();
        opt.value = jsonStringOrNumber(optObject.value("value"));
        opt.condition = optObject.value("condition").toString();
        property.options.push_back(opt);
    }
    return property;
}

QJsonObject propertyToJson(const ProjectProperty& property) {
    QJsonObject object;
    if (!property.condition.isEmpty()) object.insert("condition", property.condition);
    if (property.index >= 0) object.insert("index", property.index);
    if (property.order >= 0) object.insert("order", property.order);
    if (property.hasMin) object.insert("min", property.min);
    if (property.hasMax) object.insert("max", property.max);
    if (property.hasStep) object.insert("step", property.step);
    if (property.fraction) object.insert("fraction", property.fraction);
    if (!property.mode.isEmpty()) object.insert("mode", property.mode);
    if (property.presetOnly) object.insert("_miragePresetOnly", property.presetOnly);
    if (!property.text.isEmpty()) object.insert("text", property.text);
    object.insert("type", property.type);
    object.insert("value", variantToJsonValue(property.value));

    if (!property.options.isEmpty()) {
        QJsonArray array;
        for (const auto& opt : property.options) {
            QJsonObject optObject;
            optObject.insert("label", opt.label);
            optObject.insert("value", opt.value);
            if (!opt.condition.isEmpty()) optObject.insert("condition", opt.condition);
            array.push_back(optObject);
        }
        object.insert("options", array);
    }
    return object;
}

} // namespace Mirage

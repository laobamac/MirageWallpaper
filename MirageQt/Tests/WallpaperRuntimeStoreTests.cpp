#include "Services/WallpaperRuntimeStore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class WallpaperRuntimeStoreTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_config.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("MirageQtTests"));
        QCoreApplication::setApplicationName(QStringLiteral("WallpaperRuntimeStore"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_config.path());
        QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                           QCoreApplication::organizationName(),
                           QCoreApplication::applicationName());
        settings.clear();
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }

    void roundTripsPositionAndScriptStorage() {
        const Mirage::Wallpaper wallpaper = makeWallpaper(QStringLiteral("runtime-round-trip"));
        Mirage::WallpaperRuntimeStore store;
        Mirage::WallpaperRuntimeState state;
        state.position = QPointF(0.2, 0.8);
        state.scriptStorage.insert(QStringLiteral("clock"), QStringLiteral("17:45"));
        state.scriptStorage.insert(QStringLiteral("theme"), QStringLiteral("dark"));
        store.setRuntime(wallpaper, state, true);
        QTest::qWait(300);

        Mirage::WallpaperRuntimeStore reloaded;
        const Mirage::WallpaperRuntimeState actual = reloaded.loadRuntime(wallpaper);
        QCOMPARE(actual.position, state.position);
        QCOMPARE(actual.scriptStorage, state.scriptStorage);
    }

    void migratesRecordWithoutNewFieldsToCenter() {
        const Mirage::Wallpaper wallpaper = makeWallpaper(QStringLiteral("runtime-legacy"));
        const QJsonObject legacy {
            {QStringLiteral("volume"), 0.4},
            {QStringLiteral("speed"), 1.25},
            {QStringLiteral("muted"), false},
            {QStringLiteral("fillMode"), QStringLiteral("cover")},
            {QStringLiteral("propertyOverrides"), QJsonObject()},
        };
        QSettings settings;
        settings.setValue(QStringLiteral("Runtime_%1").arg(wallpaper.id()),
                          QJsonDocument(legacy).toJson(QJsonDocument::Compact));
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);

        Mirage::WallpaperRuntimeStore store;
        const Mirage::WallpaperRuntimeState actual = store.loadRuntime(wallpaper);
        QCOMPARE(actual.position, QPointF(0.5, 0.5));
        QVERIFY(actual.scriptStorage.isEmpty());
    }

private:
    Mirage::Wallpaper makeWallpaper(const QString& id) const {
        Mirage::Wallpaper wallpaper;
        wallpaper.wallpaperDirectory = id;
        wallpaper.renderDirectory = id;
        wallpaper.project.type = QStringLiteral("scene");
        wallpaper.project.file = QStringLiteral("scene.json");
        return wallpaper;
    }

    QTemporaryDir m_config;
};

QTEST_GUILESS_MAIN(WallpaperRuntimeStoreTests)
#include "WallpaperRuntimeStoreTests.moc"

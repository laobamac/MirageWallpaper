#include "Services/Paths.h"
#include "Services/PlaylistManager.h"
#include "Services/RendererController.h"
#include "Services/WallpaperLibrary.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QTemporaryDir>
#include <QtTest>

class PlaylistManagerPersistenceTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_home.isValid());
        qputenv("HOME", m_home.path().toUtf8());
    }

    void migratesLegacyScreenKeysAndKeepsBackup() {
        // A numeric key must be rewritten to the current output identity while
        // the exact legacy bytes remain available for manual recovery.
        const QString path = Mirage::Paths::dataDir() + QStringLiteral("/playlists.json");
        const QJsonObject playlist{
            {QStringLiteral("name"), QStringLiteral("legacy")},
            {QStringLiteral("items"), QJsonArray{}},
        };
        const QJsonObject legacy{
            {QStringLiteral("currents"), QJsonObject{{QStringLiteral("0"), playlist}}},
            {QStringLiteral("saved"), QJsonArray{}},
            {QStringLiteral("lastApplied"), QJsonObject{{QStringLiteral("0"), QStringLiteral("wallpaper-a")}}},
        };
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray original = QJsonDocument(legacy).toJson(QJsonDocument::Compact);
        QCOMPARE(file.write(original), original.size());
        file.close();

        Mirage::GlobalSettingsService settings;
        Mirage::WallpaperLibrary library(&settings);
        Mirage::RendererController renderer(&settings);
        Mirage::PlaylistManager manager(&library, &renderer);

        const QString stableId = QGuiApplication::primaryScreen() == nullptr
            ? QStringLiteral("index:0")
            : Mirage::RendererController::stableOutputId(QGuiApplication::primaryScreen());
        QVERIFY(!stableId.isEmpty());
        QCOMPARE(manager.current(0).name, QStringLiteral("legacy"));
        QCOMPARE(manager.lastAppliedIDs().value(stableId), QStringLiteral("wallpaper-a"));

        QFile migrated(path);
        QVERIFY(migrated.open(QIODevice::ReadOnly));
        const QJsonObject migratedRoot = QJsonDocument::fromJson(migrated.readAll()).object();
        QVERIFY(migratedRoot.value(QStringLiteral("currents")).toObject().contains(stableId));
        QVERIFY(!migratedRoot.value(QStringLiteral("currents")).toObject().contains(QStringLiteral("0")));

        QFile backup(path + QStringLiteral(".pre-stable-id"));
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), original);
    }

private:
    QTemporaryDir m_home;
};

QTEST_MAIN(PlaylistManagerPersistenceTests)
#include "PlaylistManagerPersistenceTests.moc"

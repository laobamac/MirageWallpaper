#include "Services/GlobalSettingsService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class GlobalSettingsServiceTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_home.isValid());
        qputenv("HOME", m_home.path().toUtf8());
    }

    void roundTrip() {
        Mirage::GlobalSettings expected;
        expected.otherApplicationFocused = QStringLiteral("pause");
        expected.otherApplicationFullscreen = QStringLiteral("stop");
        expected.otherApplicationPlayingAudio = QStringLiteral("mute");
        expected.displayAsleep = QStringLiteral("stop");
        expected.laptopOnBattery = QStringLiteral("pause");
        expected.antiAliasing = QStringLiteral("msaa_x8");
        expected.postProcessing = QStringLiteral("ultra");
        expected.textureResolution = QStringLiteral("highQuality");
        expected.fps = 77;
        expected.wallpaperLoadSource = QStringLiteral("memory");
        expected.enableSpectrum = false;

        Mirage::GlobalSettingsService service;
        QString error;
        QVERIFY2(service.setSettings(expected, &error), qPrintable(error));

        Mirage::GlobalSettingsService reloaded;
        QCOMPARE(reloaded.settings(), expected);

        QFile file(m_home.path() + QStringLiteral("/.config/MirageQt/settings.json"));
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        QCOMPARE(document.object().value(QStringLiteral("fps")).toInt(), 77);
        QCOMPARE(document.object().value(QStringLiteral("textureResolution")).toString(),
                 QStringLiteral("highQuality"));
    }

    void sanitizesInvalidValues() {
        Mirage::GlobalSettings invalid;
        invalid.otherApplicationFocused = QStringLiteral("stop");
        invalid.otherApplicationPlayingAudio = QStringLiteral("stop");
        invalid.displayAsleep = QStringLiteral("mute");
        invalid.laptopOnBattery = QStringLiteral("mute");
        invalid.antiAliasing = QStringLiteral("bad");
        invalid.postProcessing = QStringLiteral("bad");
        invalid.textureResolution = QStringLiteral("bad");
        invalid.fps = 500;

        Mirage::GlobalSettingsService service;
        QVERIFY(service.setSettings(invalid));
        const Mirage::GlobalSettings& settings = service.settings();
        QCOMPARE(settings.otherApplicationFocused, QStringLiteral("keepRunning"));
        QCOMPARE(settings.otherApplicationPlayingAudio, QStringLiteral("keepRunning"));
        QCOMPARE(settings.displayAsleep, QStringLiteral("keepRunning"));
        QCOMPARE(settings.laptopOnBattery, QStringLiteral("keepRunning"));
        QCOMPARE(settings.antiAliasing, QStringLiteral("msaa_x2"));
        QCOMPARE(settings.postProcessing, QStringLiteral("disabled"));
        QCOMPARE(settings.textureResolution, QStringLiteral("automatic"));
        QCOMPARE(settings.fps, 120);
    }

    void migratesOriginalResolution() {
        const QString path = m_home.path() + QStringLiteral("/.config/MirageQt/settings.json");
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(QJsonObject{
            {QStringLiteral("textureResolution"), QStringLiteral("original")},
            {QStringLiteral("fps"), 42},
        }).toJson());
        file.close();

        Mirage::GlobalSettingsService service;
        QCOMPARE(service.settings().textureResolution, QStringLiteral("highQuality"));
        QCOMPARE(service.settings().fps, 42);

        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonDocument migrated = QJsonDocument::fromJson(file.readAll());
        QCOMPARE(migrated.object().value(QStringLiteral("textureResolution")).toString(),
                 QStringLiteral("highQuality"));
    }

private:
    QTemporaryDir m_home;
};

QTEST_GUILESS_MAIN(GlobalSettingsServiceTests)
#include "GlobalSettingsServiceTests.moc"

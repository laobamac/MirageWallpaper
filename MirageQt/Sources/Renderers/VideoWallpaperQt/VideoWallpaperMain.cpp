#include "Renderers/Common/ControlReader.h"
#include "Renderers/Common/DesktopWindowX11.h"
#include "Renderers/VideoWallpaperQt/VideoWallpaperWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("VideoWallpaperQt"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("MirageQt Linux video wallpaper placeholder"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("wallpaper-dir"), QStringLiteral("Wallpaper directory"));
    parser.addOptions({
        {"screen", QStringLiteral("Screen index"), QStringLiteral("N"), QStringLiteral("0")},
        {"volume", QStringLiteral("Volume"), QStringLiteral("V"), QStringLiteral("1.0")},
        {"fill", QStringLiteral("Fill mode: cover, contain or stretch"), QStringLiteral("MODE"), QStringLiteral("cover")},
        {"muted", QStringLiteral("Start muted")},
        {"control-stdin", QStringLiteral("Accept JSON control commands on stdin")},
    });
    parser.process(app);

    qWarning().noquote() << "VideoWallpaperQt is a placeholder: Linux video rendering is not implemented yet.";

    Mirage::VideoWallpaperWindow window;
    Mirage::DesktopWindowX11::makeNormalToolWindow(&window);
    window.show();

    Mirage::ControlReader reader;
    if (parser.isSet("control-stdin")) {
        QObject::connect(&reader, &Mirage::ControlReader::commandReceived, &window, &Mirage::VideoWallpaperWindow::handleCommand);
        QObject::connect(&reader, &Mirage::ControlReader::inputClosed, &app, &QCoreApplication::quit);
        reader.start();
    }

    return app.exec();
}

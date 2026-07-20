#include "Renderers/Common/ControlReader.h"
#include "Renderers/Common/DesktopWindowX11.h"
#include "Renderers/WebWallpaperQt/WebWallpaperWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WebWallpaperQt"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("MirageQt Linux web wallpaper placeholder"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("wallpaper-dir"), QStringLiteral("Wallpaper directory"));
    parser.addOptions({
        {{"f", "fps"}, QStringLiteral("Render FPS"), QStringLiteral("N"), QStringLiteral("30")},
        {"volume", QStringLiteral("Volume"), QStringLiteral("V"), QStringLiteral("1.0")},
        {"screen", QStringLiteral("Screen index"), QStringLiteral("N"), QStringLiteral("0")},
        {"control-stdin", QStringLiteral("Accept JSON control commands on stdin")},
        {"no-spectrum", QStringLiteral("Disable audio spectrum")},
        {"asset-overlay", QStringLiteral("Preset asset overlay directory"), QStringLiteral("DIR")},
    });
    parser.process(app);

    qWarning().noquote() << "WebWallpaperQt is a placeholder: Linux web rendering is not implemented yet.";

    Mirage::WebWallpaperWindow window;
    Mirage::DesktopWindowX11::makeNormalToolWindow(&window);
    window.show();

    Mirage::ControlReader reader;
    if (parser.isSet("control-stdin")) {
        QObject::connect(&reader, &Mirage::ControlReader::commandReceived, &window, &Mirage::WebWallpaperWindow::handleCommand);
        QObject::connect(&reader, &Mirage::ControlReader::inputClosed, &app, &QCoreApplication::quit);
        reader.start();
    }

    return app.exec();
}

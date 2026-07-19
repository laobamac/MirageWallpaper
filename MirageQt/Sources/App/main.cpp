#include "ContentView/MainWindow.h"
#include "App/MirageStyle.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("MirageQt"));
    QCoreApplication::setOrganizationName(QStringLiteral("Mirage"));
    Mirage::applyMirageStyle(app);

    Mirage::MainWindow window;
    window.show();
    return app.exec();
}

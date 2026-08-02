#include "ContentView/MainWindow.h"
#include "App/MirageStyle.h"
#include "Services/DisplayBrokerService.h"

#include <QApplication>
#include <QDebug>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("MirageQt"));
    QCoreApplication::setOrganizationName(QStringLiteral("Mirage"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    Mirage::applyMirageStyle(app);

    Mirage::DisplayBrokerService displayBroker;
    QString brokerError;
    if (!displayBroker.start(&brokerError)) {
        qWarning().noquote() << brokerError;
    }

    Mirage::MainWindow window;
    window.show();
    return app.exec();
}

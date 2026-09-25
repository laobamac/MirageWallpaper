#include "Services/ThumbnailGenerator.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>

namespace Mirage {

bool ThumbnailGenerator::generateForVideo(const QString& videoPath, const QString& destinationJpeg) {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) return false;

    QDir().mkpath(QFileInfo(destinationJpeg).absolutePath());
    QProcess process;
    process.setProgram(ffmpeg);
    process.setArguments({
        QStringLiteral("-y"),
        QStringLiteral("-ss"), QStringLiteral("1"),
        QStringLiteral("-i"), videoPath,
        QStringLiteral("-frames:v"), QStringLiteral("1"),
        QStringLiteral("-q:v"), QStringLiteral("3"),
        destinationJpeg,
    });
    process.start();
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0 &&
           QFileInfo(destinationJpeg).size() > 0;
}

bool ThumbnailGenerator::writePlaceholder(const QString& destinationJpeg, const QString& title) {
    QDir().mkpath(QFileInfo(destinationJpeg).absolutePath());

    QImage image(QSize(960, 540), QImage::Format_RGB32);
    image.fill(QColor(34, 37, 43));

    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    QLinearGradient gradient(0, 0, image.width(), image.height());
    gradient.setColorAt(0.0, QColor(31, 70, 100));
    gradient.setColorAt(0.55, QColor(56, 67, 78));
    gradient.setColorAt(1.0, QColor(115, 78, 58));
    painter.fillRect(image.rect(), gradient);

    painter.setPen(QColor(255, 255, 255, 235));
    QFont titleFont = painter.font();
    titleFont.setPointSize(34);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.drawText(image.rect().adjusted(64, 64, -64, -120),
                     Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap,
                     title.isEmpty() ? QStringLiteral("Mirage Wallpaper") : title);

    painter.setPen(QColor(255, 255, 255, 170));
    QFont subFont = painter.font();
    subFont.setPointSize(17);
    subFont.setBold(false);
    painter.setFont(subFont);
    painter.drawText(image.rect().adjusted(64, 0, -64, -56),
                     Qt::AlignLeft | Qt::AlignBottom,
                     QStringLiteral("Video wallpaper"));
    painter.end();

    return image.save(destinationJpeg, "JPG", 88);
}

} // namespace Mirage

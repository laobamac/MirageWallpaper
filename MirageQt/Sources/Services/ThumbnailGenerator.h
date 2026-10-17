#pragma once

#include <QString>

namespace Mirage {

class ThumbnailGenerator {
public:
    static bool generateForVideo(const QString& videoPath, const QString& destinationJpeg);
    static bool writePlaceholder(const QString& destinationJpeg, const QString& title);
};

} // namespace Mirage

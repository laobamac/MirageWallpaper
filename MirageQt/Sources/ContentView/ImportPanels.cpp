#include "ContentView/ImportPanels.h"

#include <QFileDialog>
#include <QFileInfo>

namespace Mirage {

QString ImportPanels::selectWallpaper(QWidget* parent) {
    QFileDialog dialog(parent, QStringLiteral("导入壁纸"));
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setOption(QFileDialog::DontUseNativeDialog, false);
    dialog.setNameFilters({
        QStringLiteral("Wallpaper or video (*.mp4 *.mov *.m4v *.webm *.mkv project.json)"),
        QStringLiteral("All files (*)"),
    });
    if (dialog.exec() != QDialog::Accepted) return {};
    const QString selected = dialog.selectedFiles().value(0);
    if (selected.endsWith("/project.json")) return QFileInfo(selected).absolutePath();
    return selected;
}

} // namespace Mirage

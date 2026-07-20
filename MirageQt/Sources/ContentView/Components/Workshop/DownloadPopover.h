#pragma once

#include "Services/WorkshopViewModel.h"

#include <QFrame>
#include <QMultiHash>
#include <QPointer>

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace Mirage {

class DownloadPopover final : public QFrame {
    Q_OBJECT

public:
    explicit DownloadPopover(WorkshopViewModel* viewModel,
                             SteamWebAPI* api,
                             QWidget* parent = nullptr);

    void showBelow(QWidget* anchor);

private:
    void rebuild();
    QWidget* buildRow(const WorkshopDownloadTask& task);
    void revealDownload(const QString& workshopId);

    WorkshopViewModel* m_viewModel = nullptr;
    SteamWebAPI* m_api = nullptr;
    QPushButton* m_clear = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_rows = nullptr;
    QVBoxLayout* m_rowsLayout = nullptr;
    QLabel* m_empty = nullptr;
    QLabel* m_footer = nullptr;
    QMultiHash<QUrl, QPointer<QLabel>> m_previewLabels;
};

} // namespace Mirage

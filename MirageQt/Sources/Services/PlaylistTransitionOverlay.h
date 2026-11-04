#pragma once

#include "Services/PlaylistModels.h"

#include <QHash>
#include <QObject>
#include <functional>

class QWidget;

namespace Mirage {

class RendererController;

class PlaylistTransitionOverlay : public QObject {
    Q_OBJECT

public:
    static PlaylistTransitionOverlay& instance();

    void present(int screen,
                 double durationSeconds,
                 PlaylistTransitionKind kind,
                 const std::function<void()>& apply,
                 RendererController* renderer = nullptr);
    void dismiss(int screen);

private:
    explicit PlaylistTransitionOverlay(QObject* parent = nullptr);

    QHash<int, QWidget*> m_windows;
};

} // namespace Mirage

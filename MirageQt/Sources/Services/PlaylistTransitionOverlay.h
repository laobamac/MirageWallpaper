#pragma once

#include "Services/PlaylistModels.h"

#include <QHash>
#include <QObject>
#include <functional>

class QWidget;

namespace Mirage {

class PlaylistTransitionOverlay : public QObject {
    Q_OBJECT

public:
    static PlaylistTransitionOverlay& instance();

    void present(int screen,
                 double durationSeconds,
                 PlaylistTransitionKind kind,
                 const std::function<void()>& apply);

private:
    explicit PlaylistTransitionOverlay(QObject* parent = nullptr);

    void dismiss(int screen);

    QHash<int, QWidget*> m_windows;
};

} // namespace Mirage

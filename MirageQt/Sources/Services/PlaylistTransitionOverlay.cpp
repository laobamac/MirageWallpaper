#include "Services/PlaylistTransitionOverlay.h"

#include <QGuiApplication>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QAbstractAnimation>

namespace Mirage {

PlaylistTransitionOverlay& PlaylistTransitionOverlay::instance() {
    static PlaylistTransitionOverlay overlay;
    return overlay;
}

PlaylistTransitionOverlay::PlaylistTransitionOverlay(QObject* parent)
    : QObject(parent) {}

void PlaylistTransitionOverlay::present(int screen,
                                        double durationSeconds,
                                        PlaylistTransitionKind kind,
                                        const std::function<void()>& apply) {
    const auto runApply = [apply] {
        if (apply) apply();
    };

    if (durationSeconds <= 0.05 || kind == PlaylistTransitionKind::Disabled) {
        runApply();
        return;
    }

    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screen < 0 || screen >= screens.size()) {
        runApply();
        return;
    }

    dismiss(screen);

    QScreen* target = screens.at(screen);
    auto* window = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    window->setAttribute(Qt::WA_TransparentForMouseEvents);
    window->setAttribute(Qt::WA_ShowWithoutActivating);
    window->setAttribute(Qt::WA_TranslucentBackground);
    window->setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    window->setGeometry(target->geometry());
    window->setStyleSheet(QStringLiteral("background-color: rgba(0, 0, 0, 255);"));
    window->setWindowOpacity(0.0);
    window->show();
    m_windows.insert(screen, window);

    const int halfMs = qMax(30, static_cast<int>(durationSeconds * 500.0));
    auto* fadeIn = new QPropertyAnimation(window, "windowOpacity", window);
    fadeIn->setDuration(halfMs);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);
    fadeIn->start(QAbstractAnimation::DeleteWhenStopped);

    QTimer::singleShot(halfMs, window, [this, screen, halfMs, runApply, window] {
        runApply();
        QTimer::singleShot(50, window, [this, screen, halfMs, window] {
            auto* fadeOut = new QPropertyAnimation(window, "windowOpacity", window);
            fadeOut->setDuration(halfMs);
            fadeOut->setStartValue(1.0);
            fadeOut->setEndValue(0.0);
            fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
            QTimer::singleShot(halfMs + 50, this, [this, screen] { dismiss(screen); });
        });
    });
}

void PlaylistTransitionOverlay::dismiss(int screen) {
    if (QWidget* window = m_windows.take(screen)) {
        window->hide();
        window->deleteLater();
    }
}

} // namespace Mirage

#include "Services/PlaylistTransitionOverlay.h"

#include <QGuiApplication>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QAbstractAnimation>
#include <QPointer>
#include <QDebug>
#include "Services/RendererController.h"
#ifdef Q_OS_WIN
#include "../../../Platform/Windows/DesktopHost.h"
#endif

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
                                        const std::function<void()>& apply,
                                        RendererController* renderer) {
#ifndef Q_OS_WIN
    Q_UNUSED(renderer)
#endif
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
#ifdef Q_OS_WIN
    auto* window = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool);
#else
    auto* window = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
#endif
    window->setAttribute(Qt::WA_TransparentForMouseEvents);
    window->setAttribute(Qt::WA_ShowWithoutActivating);
    window->setAttribute(Qt::WA_TranslucentBackground);
    window->setWindowFlag(Qt::WindowDoesNotAcceptFocus);
#ifdef Q_OS_WIN
    window->setWindowFlag(Qt::WindowTransparentForInput);
#endif
    window->setGeometry(target->geometry());
    window->setStyleSheet(QStringLiteral("background-color: rgba(0, 0, 0, 255);"));
    window->setWindowOpacity(0.0);
#ifdef Q_OS_WIN
    const HWND nativeWindow = reinterpret_cast<HWND>(window->winId());
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    DWORD attachError = ERROR_SUCCESS;
    const auto* nativeScreen = target->nativeInterface<QNativeInterface::QWindowsScreen>();
    if (!nativeScreen || !GetMonitorInfoW(nativeScreen->handle(), &monitor) ||
        !Windows::attachToDesktop(nativeWindow, monitor.rcMonitor, &attachError)) {
        qWarning() << "[Transition] Cannot attach desktop overlay:" << attachError;
        window->deleteLater();
        runApply();
        return;
    }
    SetWindowPos(nativeWindow, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
    window->show();
    m_windows.insert(screen, window);

    const int halfMs = qMax(30, static_cast<int>(durationSeconds * 500.0));
    auto* fadeIn = new QPropertyAnimation(window, "windowOpacity", window);
    fadeIn->setDuration(halfMs);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);
    fadeIn->start(QAbstractAnimation::DeleteWhenStopped);

#ifdef Q_OS_WIN
    connect(fadeIn, &QPropertyAnimation::finished, window,
        [this, screen, halfMs, runApply, window, renderer = QPointer<RendererController>(renderer)] {
            if (!renderer) {
                dismiss(screen);
                runApply();
                return;
            }
            window->setProperty("waitingForFrame", true);
            connect(renderer, &RendererController::firstFramePresented, window,
                [this, screen, halfMs, window](int readyScreen) {
                    if (readyScreen != screen || !window->property("waitingForFrame").toBool()) return;
                    window->setProperty("waitingForFrame", false);
                    auto* fadeOut = new QPropertyAnimation(window, "windowOpacity", window);
                    fadeOut->setDuration(halfMs);
                    fadeOut->setStartValue(1.0);
                    fadeOut->setEndValue(0.0);
                    connect(fadeOut, &QPropertyAnimation::finished, window,
                        [this, screen] { dismiss(screen); });
                    fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
                });
            connect(renderer, &RendererController::rendererExited, window,
                [this, screen, renderer](int exitedScreen, bool) {
                    if (exitedScreen == screen && renderer && !renderer->isRunningOnScreen(screen)) dismiss(screen);
                });
            QTimer::singleShot(15000, window, [this, screen, window] {
                if (!window->property("waitingForFrame").toBool()) return;
                qWarning() << "[Transition] Renderer did not report its first frame; removing stalled overlay";
                dismiss(screen);
            });
            runApply();
        });
#else
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
#endif
}

void PlaylistTransitionOverlay::dismiss(int screen) {
    if (QWidget* window = m_windows.take(screen)) {
        window->hide();
        window->deleteLater();
    }
}

} // namespace Mirage

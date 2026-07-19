#pragma once

#include "Services/GlobalSettingsService.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;

namespace Mirage {

class PerformancePage final : public QWidget {
    Q_OBJECT

public:
    explicit PerformancePage(GlobalSettings* draft, QWidget* parent = nullptr);
    void load();

signals:
    void changed();

private:
    void setQuality(const QString& quality);
    void updateFPSLabel(int fps);

    GlobalSettings* m_draft = nullptr;
    QComboBox* m_focused = nullptr;
    QComboBox* m_fullscreen = nullptr;
    QComboBox* m_audioPlaying = nullptr;
    QComboBox* m_displayAsleep = nullptr;
    QComboBox* m_battery = nullptr;
    QComboBox* m_antiAliasing = nullptr;
    QSlider* m_fps = nullptr;
    QLabel* m_fpsValue = nullptr;
    QLabel* m_fpsWarning = nullptr;
    QCheckBox* m_spectrum = nullptr;
};

} // namespace Mirage

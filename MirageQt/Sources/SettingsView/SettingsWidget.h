#pragma once

#include "Services/GlobalSettingsService.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QWidget>

namespace Mirage {

class SettingsWidget : public QWidget {
    Q_OBJECT

public:
    explicit SettingsWidget(GlobalSettingsService* settings, QWidget* parent = nullptr);

private:
    void load();
    void save();

    GlobalSettingsService* m_settings = nullptr;
    QComboBox* m_appearance = nullptr;
    QSpinBox* m_fps = nullptr;
    QDoubleSpinBox* m_volume = nullptr;
    QCheckBox* m_muted = nullptr;
    QCheckBox* m_spectrum = nullptr;
    QCheckBox* m_autoRefresh = nullptr;
    QCheckBox* m_autoStart = nullptr;
    QComboBox* m_endpoint = nullptr;
    QLineEdit* m_apiKey = nullptr;
    QLineEdit* m_workshopDir = nullptr;
    QLineEdit* m_importDir = nullptr;
};

} // namespace Mirage

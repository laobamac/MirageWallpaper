#pragma once

#include "Services/GlobalSettingsService.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSlider;

namespace Mirage {

class GeneralPage final : public QWidget {
    Q_OBJECT

public:
    explicit GeneralPage(GlobalSettings* draft, QWidget* parent = nullptr);
    void load();

signals:
    void changed();
    void resetRequested();

private:
    QWidget* directoryRow(const QString& title,
                          const QString& detail,
                          QLineEdit** edit,
                          bool workshopDirectory,
                          QWidget* parent);
    void chooseDirectory(QLineEdit* edit, bool workshopDirectory);
    void restoreDirectory(QLineEdit* edit, bool workshopDirectory);
    void revealDirectory(QLineEdit* edit);
    void updateDirectoryFields();
    void updateAPIKeyStatus();

    GlobalSettings* m_draft = nullptr;
    QCheckBox* m_autoStart = nullptr;
    QComboBox* m_appearance = nullptr;
    QSlider* m_volume = nullptr;
    QLabel* m_volumeValue = nullptr;
    QCheckBox* m_muted = nullptr;
    QCheckBox* m_autoRefresh = nullptr;
    QComboBox* m_endpoint = nullptr;
    QWidget* m_endpointGroup = nullptr;
    QLineEdit* m_apiKey = nullptr;
    QLabel* m_apiBanner = nullptr;
    QLabel* m_apiStatus = nullptr;
    QLineEdit* m_workshopDir = nullptr;
    QLineEdit* m_importedDir = nullptr;
    QCheckBox* m_verboseLog = nullptr;
};

} // namespace Mirage

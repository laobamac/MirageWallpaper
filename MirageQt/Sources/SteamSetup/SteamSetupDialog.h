#pragma once

#include "Services/SteamCMDManager.h"

#include <QDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>

namespace Mirage {

class SteamSetupDialog : public QDialog {
    Q_OBJECT

public:
    explicit SteamSetupDialog(SteamCMDManager* steamCMD, QWidget* parent = nullptr);

private:
    SteamCMDManager* m_steamCMD = nullptr;
    QProgressBar* m_progress = nullptr;
    QLineEdit* m_username = nullptr;
    QLineEdit* m_password = nullptr;
    QLineEdit* m_guard = nullptr;
    QPlainTextEdit* m_log = nullptr;
};

} // namespace Mirage

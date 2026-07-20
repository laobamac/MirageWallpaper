#pragma once

#include "Services/GlobalSettingsService.h"

#include <QWidget>

class QButtonGroup;
class QLabel;
class QStackedWidget;

namespace Mirage {

class GeneralPage;
class PerformancePage;

class SettingsView final : public QWidget {
    Q_OBJECT

public:
    explicit SettingsView(GlobalSettingsService* settings, QWidget* parent = nullptr);
    void selectPage(int index);

signals:
    void accepted();
    void cancelled();

private:
    void updateModifiedState();
    void resetDraft();
    void commit();

    GlobalSettingsService* m_settings = nullptr;
    GlobalSettings m_original;
    GlobalSettings m_draft;
    QButtonGroup* m_toolbar = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_modified = nullptr;
    PerformancePage* m_performance = nullptr;
    GeneralPage* m_general = nullptr;
};

} // namespace Mirage

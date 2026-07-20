#pragma once

#include <QButtonGroup>
#include <QWidget>

namespace Mirage {

class TopTabBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit TopTabBarWidget(QWidget* parent = nullptr);
    int currentIndex() const;

public slots:
    void setCurrentIndex(int index);
    void setActiveDownloadCount(int count);

signals:
    void tabChanged(int index);
    void mobileRequested();
    void displaySettingsRequested();
    void settingsRequested();

private:
    QButtonGroup* m_group = nullptr;
    QWidget* m_workshopBadge = nullptr;
};

} // namespace Mirage

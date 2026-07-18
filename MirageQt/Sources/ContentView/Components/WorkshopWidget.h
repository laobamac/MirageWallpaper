#pragma once

#include "Services/SteamCMDManager.h"
#include "Services/SteamWebAPI.h"
#include "Services/WallpaperLibrary.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

namespace Mirage {

class WorkshopWidget : public QWidget {
    Q_OBJECT

public:
    explicit WorkshopWidget(SteamWebAPI* api,
                            SteamCMDManager* steamCMD,
                            WallpaperLibrary* library,
                            QWidget* parent = nullptr);

    WorkshopItem currentItem() const;

public slots:
    void search();

signals:
    void itemSelected(const Mirage::WorkshopItem& item);
    void steamSetupRequested();
    void downloadRequested(const Mirage::WorkshopItem& item);

private:
    void rebuildList(const QVector<WorkshopItem>& items);
    void updateSteamBanner();

    SteamWebAPI* m_api = nullptr;
    SteamCMDManager* m_steamCMD = nullptr;
    WallpaperLibrary* m_library = nullptr;
    QVector<WorkshopItem> m_items;

    QLabel* m_apiKeyBanner = nullptr;
    QLabel* m_setupBanner = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_sort = nullptr;
    QComboBox* m_type = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_download = nullptr;
};

} // namespace Mirage

#pragma once

#include "DesktopHost.h"
#include "WebRendererEngine.h"
#include <QObject>
#include <QPointer>
#include <QTimer>

class WRDesktopHost final : public QObject {
public:
    explicit WRDesktopHost(WebRendererEngine* engine);
    ~WRDesktopHost() override;
    bool attach(int screenIndex, const QString& deviceName, QString* error);

private:
    static LRESULT CALLBACK mouseHook(int code, WPARAM message, LPARAM data);
    void forwardPointer(WPARAM message, const MSLLHOOKSTRUCT& event);
    bool desktopAt(POINT point) const;
    HWND refreshWindow() const;
    bool positionWindow(QString* error);
    QPointer<WebRendererEngine> m_engine;
    HHOOK m_hook = nullptr;
    HWND m_window = nullptr;
    std::wstring m_device;
    RECT m_bounds{};
    QTimer m_repair;
    bool m_inside = false;
    inline static WRDesktopHost* s_instance = nullptr;
};

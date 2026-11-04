#include "Services/WindowsPlaybackStateMonitor.h"

#ifdef Q_OS_WIN

#include <QFileInfo>
#include <QMutex>
#include <QWaitCondition>
#include <QDebug>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <iterator>
#include <optional>
#include <thread>

namespace Mirage {
namespace {

using Microsoft::WRL::ComPtr;

constexpr GUID kMonitorPowerOnSetting = {
    0x02731015, 0x4510, 0x4526, {0x99, 0xE6, 0xE5, 0xA1, 0x7E, 0xBD, 0x1A, 0xEA}
};
constexpr wchar_t kPowerWindowClass[] = L"MiragePlaybackPowerMonitorWindow";

std::optional<bool> isManagedRendererProcess(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return std::nullopt;

    wchar_t imagePath[32768]{};
    DWORD pathLength = static_cast<DWORD>(std::size(imagePath));
    const BOOL queried = QueryFullProcessImageNameW(process, 0, imagePath, &pathLength);
    CloseHandle(process);
    if (!queried) return std::nullopt;

    const QString executable = QFileInfo(QString::fromWCharArray(imagePath, static_cast<qsizetype>(pathLength)))
                                   .fileName();
    return executable.compare(QStringLiteral("SceneWallpaper.exe"), Qt::CaseInsensitive) == 0 ||
           executable.compare(QStringLiteral("WebWallpaper.exe"), Qt::CaseInsensitive) == 0 ||
           executable.compare(QStringLiteral("VideoWallpaper.exe"), Qt::CaseInsensitive) == 0;
}

bool scanAudioEndpoint(IMMDevice* endpoint, DWORD currentProcessId, bool* playing) {
    ComPtr<IAudioSessionManager2> sessionManager;
    HRESULT result = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(sessionManager.GetAddressOf()));
    if (FAILED(result)) return false;

    ComPtr<IAudioSessionEnumerator> sessions;
    result = sessionManager->GetSessionEnumerator(sessions.GetAddressOf());
    if (FAILED(result)) return false;

    int sessionCount = 0;
    result = sessions->GetCount(&sessionCount);
    if (FAILED(result)) return false;

    bool fullyEnumerated = true;
    for (int index = 0; index < sessionCount; ++index) {
        ComPtr<IAudioSessionControl> session;
        if (FAILED(sessions->GetSession(index, session.GetAddressOf()))) {
            fullyEnumerated = false;
            continue;
        }

        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(session->GetState(&state))) {
            fullyEnumerated = false;
            continue;
        }
        if (state != AudioSessionStateActive) continue;

        ComPtr<IAudioSessionControl2> sessionDetails;
        if (FAILED(session.As(&sessionDetails))) {
            fullyEnumerated = false;
            continue;
        }

        DWORD processId = 0;
        if (FAILED(sessionDetails->GetProcessId(&processId))) {
            fullyEnumerated = false;
            continue;
        }
        if (processId == currentProcessId) continue;

        const std::optional<bool> managed = isManagedRendererProcess(processId);
        if (!managed.has_value()) {
            fullyEnumerated = false;
            continue;
        }
        if (*managed) continue;

        *playing = true;
        return true;
    }
    return fullyEnumerated;
}

bool queryOtherApplicationAudio(bool* playing) {
    *playing = false;

    ComPtr<IMMDeviceEnumerator> devices;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(devices.GetAddressOf()));
    if (FAILED(result)) return false;

    ComPtr<IMMDeviceCollection> endpoints;
    result = devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, endpoints.GetAddressOf());
    if (FAILED(result)) return false;

    UINT endpointCount = 0;
    result = endpoints->GetCount(&endpointCount);
    if (FAILED(result)) return false;

    bool fullyEnumerated = true;
    const DWORD currentProcessId = GetCurrentProcessId();
    for (UINT index = 0; index < endpointCount; ++index) {
        ComPtr<IMMDevice> endpoint;
        if (FAILED(endpoints->Item(index, endpoint.GetAddressOf()))) {
            fullyEnumerated = false;
            continue;
        }
        if (!scanAudioEndpoint(endpoint.Get(), currentProcessId, playing)) {
            fullyEnumerated = false;
            continue;
        }
        if (*playing) return true;
    }
    return fullyEnumerated;
}

} // namespace

struct WindowsPlaybackStateMonitor::Impl {
    explicit Impl(WindowsPlaybackStateMonitor* monitor) : owner(monitor) {}

    void start() {
        QMutexLocker lock(&startupMutex);
        if (worker.joinable()) return;
        startupComplete = false;
        worker = std::thread([this] { run(); });
        while (!startupComplete) startupCondition.wait(&startupMutex);
    }

    void stop() {
        DWORD id = 0;
        {
            QMutexLocker lock(&startupMutex);
            id = threadId;
        }
        if (worker.joinable()) {
            if (id != 0) PostThreadMessageW(id, WM_QUIT, 0, 0);
            worker.join();
        }
    }

    void run() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        audioComAvailable = SUCCEEDED(comResult);
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        {
            QMutexLocker lock(&startupMutex);
            threadId = GetCurrentThreadId();
        }

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::windowProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kPowerWindowClass;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            qWarning() << "[PlaybackState] Cannot register Windows power notification window:" << GetLastError();
        } else {
            window = CreateWindowExW(0, kPowerWindowClass, L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, windowClass.hInstance, this);
        }

        if (window) {
            powerNotification = RegisterPowerSettingNotification(
                window, &kMonitorPowerOnSetting, DEVICE_NOTIFY_WINDOW_HANDLE);
            SetTimer(window, 1, 1000, nullptr);
        }
        {
            QMutexLocker lock(&startupMutex);
            startupComplete = true;
            startupCondition.wakeAll();
        }

        if (window) {
            refreshAudioState();
            refreshBatteryState();
        }
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (powerNotification) UnregisterPowerSettingNotification(powerNotification);
        if (window) DestroyWindow(window);
        if (SUCCEEDED(comResult)) CoUninitialize();
    }

    static LRESULT CALLBACK windowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

        if (message == WM_TIMER) {
            self->refreshAudioState();
            self->refreshBatteryState();
            return 0;
        }
        if (message == WM_POWERBROADCAST) {
            if (wParam == PBT_POWERSETTINGCHANGE && lParam != 0) {
                const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                if (IsEqualGUID(setting->PowerSetting, kMonitorPowerOnSetting) &&
                    setting->DataLength >= sizeof(DWORD)) {
                    const DWORD monitorOn = *reinterpret_cast<const DWORD*>(setting->Data);
                    if (monitorOn == 0 || monitorOn == 1) {
                        self->publishDisplayPower(true, monitorOn == 0);
                    } else {
                        self->publishDisplayPower(false, false);
                    }
                }
            } else if (wParam == PBT_APMPOWERSTATUSCHANGE) {
                self->refreshBatteryState();
            }
            return TRUE;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void refreshAudioState() {
        bool playing = false;
        const bool known = audioComAvailable && queryOtherApplicationAudio(&playing);
        if (!audioStateInitialized || known != lastAudioKnown || playing != lastAudioPlaying) {
            audioStateInitialized = true;
            lastAudioKnown = known;
            lastAudioPlaying = playing;
            emit owner->otherApplicationAudioChanged(known, playing);
        }
    }

    void refreshBatteryState() {
        SYSTEM_POWER_STATUS status{};
        const bool queried = GetSystemPowerStatus(&status) != FALSE;
        const bool known = queried && (status.ACLineStatus == 0 || status.ACLineStatus == 1);
        const bool onBattery = known && status.ACLineStatus == 0 && status.BatteryFlag != 128;
        if (!batteryStateInitialized || known != lastBatteryKnown || onBattery != lastOnBattery) {
            batteryStateInitialized = true;
            lastBatteryKnown = known;
            lastOnBattery = onBattery;
            emit owner->batteryStateChanged(known, onBattery);
        }
    }

    void publishDisplayPower(bool known, bool asleep) {
        if (!displayStateInitialized || known != lastDisplayKnown || asleep != lastDisplayAsleep) {
            displayStateInitialized = true;
            lastDisplayKnown = known;
            lastDisplayAsleep = asleep;
            emit owner->displayPowerChanged(known, asleep);
        }
    }

    WindowsPlaybackStateMonitor* owner;
    QMutex startupMutex;
    QWaitCondition startupCondition;
    bool startupComplete = false;
    DWORD threadId = 0;
    std::thread worker;
    HWND window = nullptr;
    HPOWERNOTIFY powerNotification = nullptr;
    bool audioComAvailable = false;
    bool audioStateInitialized = false;
    bool lastAudioKnown = false;
    bool lastAudioPlaying = false;
    bool displayStateInitialized = false;
    bool lastDisplayKnown = false;
    bool lastDisplayAsleep = false;
    bool batteryStateInitialized = false;
    bool lastBatteryKnown = false;
    bool lastOnBattery = false;
};

WindowsPlaybackStateMonitor::WindowsPlaybackStateMonitor(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(this)) {}

WindowsPlaybackStateMonitor::~WindowsPlaybackStateMonitor() {
    stop();
}

void WindowsPlaybackStateMonitor::start() {
    m_impl->start();
}

void WindowsPlaybackStateMonitor::stop() {
    m_impl->stop();
}

} // namespace Mirage

#endif // Q_OS_WIN

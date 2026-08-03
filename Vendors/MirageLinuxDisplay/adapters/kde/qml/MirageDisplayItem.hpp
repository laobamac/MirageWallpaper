#pragma once

#include "MiragePointerForwarder.hpp"

#include <mirage_display.h>
#include <mirage_display_egl.h>
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
#include <mirage_display_vulkan.h>
#include <mirage_display_vulkan_blit.h>
#endif

#include <QColor>
#include <QByteArray>
#include <QPointer>
#include <QMutex>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <QVector>
#include <array>
#include <atomic>
#include <cstdint>
#include <qqml.h>

class QEvent;
class QSGNode;
class QSGSimpleTextureNode;
class QSGTexture;

class MirageDisplayItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath NOTIFY socketPathChanged)
    Q_PROPERTY(QString defaultSocketPath READ defaultSocketPath CONSTANT)
    Q_PROPERTY(QString outputStableId READ outputStableId WRITE setOutputStableId NOTIFY outputChanged)
    Q_PROPERTY(QString outputName READ outputName WRITE setOutputName NOTIFY outputChanged)
    Q_PROPERTY(int physicalWidth READ physicalWidth WRITE setPhysicalWidth NOTIFY outputChanged)
    Q_PROPERTY(int physicalHeight READ physicalHeight WRITE setPhysicalHeight NOTIFY outputChanged)
    Q_PROPERTY(int logicalWidth READ logicalWidth WRITE setLogicalWidth NOTIFY outputChanged)
    Q_PROPERTY(int logicalHeight READ logicalHeight WRITE setLogicalHeight NOTIFY outputChanged)
    Q_PROPERTY(int scale120 READ scale120 WRITE setScale120 NOTIFY outputChanged)
    Q_PROPERTY(int refreshMhz READ refreshMhz WRITE setRefreshMhz NOTIFY outputChanged)
    Q_PROPERTY(OutputTransform outputTransform READ outputTransform WRITE setOutputTransform NOTIFY outputChanged)
    Q_PROPERTY(bool pointerForwarding READ pointerForwarding WRITE setPointerForwarding NOTIFY pointerForwardingChanged)
    Q_PROPERTY(quint32 windowStateFlags READ windowStateFlags WRITE setWindowStateFlags NOTIFY windowStateFlagsChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(qulonglong outputId READ outputId NOTIFY outputIdChanged)
    Q_PROPERTY(qulonglong framesReceived READ framesReceived NOTIFY framesReceivedChanged)
    Q_PROPERTY(QColor clearColor READ clearColor NOTIFY clearColorChanged)
    Q_PROPERTY(RendererBackend rendererBackend READ rendererBackend NOTIFY rendererBackendChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(qulonglong importedGeneration READ importedGeneration NOTIFY importedGenerationChanged)

public:
    enum OutputTransform {
        TransformNormal = MD_TRANSFORM_NORMAL,
        Transform90 = MD_TRANSFORM_90,
        Transform180 = MD_TRANSFORM_180,
        Transform270 = MD_TRANSFORM_270,
        TransformFlipped = MD_TRANSFORM_FLIPPED,
        TransformFlipped90 = MD_TRANSFORM_FLIPPED_90,
        TransformFlipped180 = MD_TRANSFORM_FLIPPED_180,
        TransformFlipped270 = MD_TRANSFORM_FLIPPED_270,
    };
    Q_ENUM(OutputTransform)

    enum RendererBackend {
        BackendNone = 0,
        BackendOpenGLEGL = 1,
        BackendVulkan = 2,
    };
    Q_ENUM(RendererBackend)

    explicit MirageDisplayItem(QQuickItem* parent = nullptr);
    ~MirageDisplayItem() override;

    QString socketPath() const { return m_socketPath; }
    QString defaultSocketPath() const { return m_defaultSocketPath; }
    void setSocketPath(const QString& value);

    QString outputStableId() const { return m_outputStableId; }
    void setOutputStableId(const QString& value);
    QString outputName() const { return m_outputName; }
    void setOutputName(const QString& value);
    int physicalWidth() const { return m_physicalWidth; }
    void setPhysicalWidth(int value);
    int physicalHeight() const { return m_physicalHeight; }
    void setPhysicalHeight(int value);
    int logicalWidth() const { return m_logicalWidth; }
    void setLogicalWidth(int value);
    int logicalHeight() const { return m_logicalHeight; }
    void setLogicalHeight(int value);
    int scale120() const { return m_scale120; }
    void setScale120(int value);
    int refreshMhz() const { return m_refreshMhz; }
    void setRefreshMhz(int value);
    OutputTransform outputTransform() const { return m_outputTransform; }
    void setOutputTransform(OutputTransform value);

    bool pointerForwarding() const { return m_pointerForwarding; }
    void setPointerForwarding(bool value);
    quint32 windowStateFlags() const { return m_windowStateFlags; }
    void setWindowStateFlags(quint32 value);

    bool connected() const { return m_connected; }
    qulonglong outputId() const { return m_outputId; }
    qulonglong framesReceived() const { return m_framesReceived; }
    QColor clearColor() const { return m_clearColor; }
    RendererBackend rendererBackend() const { return m_rendererBackend.load(); }
    QString lastError() const { return m_lastError; }
    qulonglong importedGeneration() const {
        return static_cast<qulonglong>(m_importedGeneration.load());
    }

    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void socketPathChanged();
    void outputChanged();
    void pointerForwardingChanged();
    void windowStateFlagsChanged();
    void connectedChanged();
    void outputIdChanged();
    void framesReceivedChanged();
    void clearColorChanged();
    void rendererBackendChanged();
    void lastErrorChanged();
    void importedGenerationChanged();

protected:
    void componentComplete() override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private slots:
    void handleWindowChanged(QQuickWindow* window);
    void startConnection();
    void advanceHandshake();
    void dispatchSocket();
    void flushSocket();
    void pushOutputUpdate();
    void finishDeferredUnbind(qulonglong generation);

private:
    struct PendingFrame {
        bool valid = false;
        md_frame_t value {};
    };

    using GlEglImageTargetTexture2D = void (*)(unsigned int target, void* image);

    static void onConnected(void* userData, uint64_t outputId);
    static void onBuffersReady(void* userData, const md_buffer_pool_t* pool);
    static void onBuffersReleasing(void* userData, const md_buffer_pool_t* pool);
    static void onConfig(void* userData, const md_display_config_t* config);
    static void onFrame(void* userData, const md_frame_t* frame);
    static void onDisconnected(void* userData, md_result_t reason, const char* message);

    void initializeRenderer();
    void invalidateRenderer();
    bool initializeOpenGLRenderer();
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    bool initializeVulkanRenderer();
#endif
    bool importPendingPool(const md_buffer_pool_t& pool);
    void releaseRenderPool();
    void releaseAfterRendering();
    void closeConnection();
    void handleConnectionFailure();
    void armWritable();
    void scheduleReconnect();
    md_output_info_t makeOutputInfo(QByteArray& stableId, QByteArray& name) const;
    static void dropFrame(PendingFrame& frame);
    static uint64_t monotonicTimestampUs();
    void releasePointerState(uint64_t timestamp);
    void forwardPointerEvent(const MiragePointerForwarder::Event& event);
    void setRendererBackend(RendererBackend backend);
    void setLastError(const QString& error);
    void setImportedGeneration(uint64_t generation);

    QString m_socketPath;
    QString m_defaultSocketPath;
    QString m_outputStableId { QStringLiteral("kde:unknown") };
    QString m_outputName { QStringLiteral("KDE wallpaper") };
    int m_physicalWidth = 1920;
    int m_physicalHeight = 1080;
    int m_logicalWidth = 1920;
    int m_logicalHeight = 1080;
    int m_scale120 = 120;
    int m_refreshMhz = 60000;
    OutputTransform m_outputTransform = TransformNormal;
    bool m_pointerForwarding = true;
    MiragePointerForwarder m_pointer;
    quint32 m_windowStateFlags = 0;
    bool m_connected = false;
    qulonglong m_outputId = 0;
    qulonglong m_framesReceived = 0;
    QColor m_clearColor { Qt::black };
    std::atomic<RendererBackend> m_rendererBackend { BackendNone };
    QString m_lastError;
    std::atomic_uint32_t m_drmRenderMajor { 0 };
    std::atomic_uint32_t m_drmRenderMinor { 0 };

    md_display_t* m_display = nullptr;
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    QTimer m_reconnectTimer;
    QTimer m_outputUpdateTimer;
    QPointer<QQuickWindow> m_filteredWindow;

    QMutex m_stateMutex;
    md_buffer_pool_t m_pendingPool {};
    bool m_hasPendingPool = false;
    PendingFrame m_pendingFrame;
    md_display_config_t m_config {};
    bool m_hasConfig = false;
    uint64_t m_releaseGeneration = 0;
    bool m_releaseNeedsFinish = false;

    std::atomic_bool m_rendererReady { false };
    md_egl_importer_t* m_importer = nullptr;
    GlEglImageTargetTexture2D m_imageTargetTexture = nullptr;
    std::atomic_uint64_t m_importedGeneration { 0 };
    QVector<unsigned int> m_glTextures;
    QVector<QSGTexture*> m_qsgTextures;
    int m_currentBuffer = -1;
    int m_activeReleaseFd = -1;

#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    md_vk_importer_t* m_vkImporter = nullptr;
    md_vk_blitter_t* m_vkBlitter = nullptr;
    VkDevice m_vkDevice = VK_NULL_HANDLE;
    QVector<md_format_cap_t> m_vkFormats;
    std::array<uint8_t, VK_UUID_SIZE> m_vkDeviceUuid {};
    std::array<uint8_t, VK_UUID_SIZE> m_vkDriverUuid {};
#endif
};

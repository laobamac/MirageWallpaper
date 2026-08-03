#include "MirageDisplayItem.hpp"

#include <EGL/egl.h>
#include <fcntl.h>
#include <QByteArray>
#include <QEvent>
#include <QMouseEvent>
#include <QMatrix4x4>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QScreen>
#include <QThread>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTransformNode>
#include <QWheelEvent>
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
#include <QVulkanInstance>
#endif
#include <QtGui/qopenglcontext_platform.h>
#include <QtCore/qnativeinterface.h>
#include <QtQuick/qsgtexture_platform.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8u) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16u) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

constexpr uint32_t DrmFormatXrgb8888 = fourcc('X', 'R', '2', '4');
constexpr uint32_t DrmFormatArgb8888 = fourcc('A', 'R', '2', '4');
constexpr uint32_t DrmFormatXbgr8888 = fourcc('X', 'B', '2', '4');
constexpr uint32_t DrmFormatAbgr8888 = fourcc('A', 'B', '2', '4');

class FunctionJob final : public QRunnable {
public:
    explicit FunctionJob(std::function<void()> function): m_function(std::move(function)) {}

    void run() override {
        if (m_function) m_function();
    }

private:
    std::function<void()> m_function;
};

uint32_t positiveU32(int value, uint32_t fallback) {
    if (value <= 0) return fallback;
    return static_cast<uint32_t>(value);
}

} // namespace

MirageDisplayItem::MirageDisplayItem(QQuickItem* parent): QQuickItem(parent) {
    setFlag(ItemHasContents, true);

    m_pointer.setSink([this](const MiragePointerForwarder::Event& event) {
        forwardPointerEvent(event);
    });

    const QString runtimeDirectory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtimeDirectory.isEmpty()) {
        m_defaultSocketPath = runtimeDirectory +
                              QStringLiteral("/mirage-wallpaper/display-v1.sock");
        m_socketPath = m_defaultSocketPath;
    }

    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(2000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &MirageDisplayItem::startConnection);

    m_outputUpdateTimer.setSingleShot(true);
    m_outputUpdateTimer.setInterval(25);
    connect(&m_outputUpdateTimer, &QTimer::timeout, this, &MirageDisplayItem::pushOutputUpdate);
    connect(this, &QQuickItem::windowChanged, this, &MirageDisplayItem::handleWindowChanged);
}

MirageDisplayItem::~MirageDisplayItem() {
    m_reconnectTimer.stop();
    m_outputUpdateTimer.stop();
    if (m_filteredWindow) m_filteredWindow->removeEventFilter(this);
    closeConnection();
}

void MirageDisplayItem::componentComplete() {
    QQuickItem::componentComplete();
    if (window()) handleWindowChanged(window());
}

void MirageDisplayItem::handleWindowChanged(QQuickWindow* quickWindow) {
    if (m_filteredWindow && m_filteredWindow != quickWindow) {
        m_filteredWindow->removeEventFilter(this);
        disconnect(m_filteredWindow, nullptr, this, nullptr);
    }
    if (quickWindow == nullptr) return;
    quickWindow->installEventFilter(this);
    m_filteredWindow = quickWindow;

#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    if (!quickWindow->isSceneGraphInitialized()) {
        QQuickGraphicsConfiguration configuration = quickWindow->graphicsConfiguration();
        configuration.setDeviceExtensions({
            QByteArrayLiteral("VK_KHR_external_memory"),
            QByteArrayLiteral("VK_KHR_external_memory_fd"),
            QByteArrayLiteral("VK_EXT_external_memory_dma_buf"),
            QByteArrayLiteral("VK_EXT_queue_family_foreign"),
            QByteArrayLiteral("VK_EXT_image_drm_format_modifier"),
            QByteArrayLiteral("VK_KHR_external_semaphore"),
            QByteArrayLiteral("VK_KHR_external_semaphore_fd"),
            QByteArrayLiteral("VK_KHR_sampler_ycbcr_conversion"),
            QByteArrayLiteral("VK_KHR_bind_memory2"),
            QByteArrayLiteral("VK_KHR_get_memory_requirements2"),
        });
        quickWindow->setGraphicsConfiguration(configuration);
    }
#endif

    QPointer<MirageDisplayItem> guard(this);
    connect(quickWindow, &QQuickWindow::sceneGraphInitialized, this, [guard]() {
        if (guard) guard->initializeRenderer();
    }, Qt::DirectConnection);
    connect(quickWindow, &QQuickWindow::afterRendering, this, [guard]() {
        if (guard) guard->releaseAfterRendering();
    }, Qt::DirectConnection);
    connect(quickWindow, &QQuickWindow::sceneGraphInvalidated, this, [guard]() {
        if (guard) guard->invalidateRenderer();
    }, Qt::DirectConnection);

    if (quickWindow->isSceneGraphInitialized()) {
        quickWindow->scheduleRenderJob(new FunctionJob([guard]() {
            if (guard) guard->initializeRenderer();
        }), QQuickWindow::BeforeSynchronizingStage);
        quickWindow->update();
    }
}

void MirageDisplayItem::initializeRenderer() {
    if (m_rendererReady.load()) return;
    if (window() == nullptr || window()->rendererInterface() == nullptr) return;

    bool initialized = false;
    switch (window()->rendererInterface()->graphicsApi()) {
    case QSGRendererInterface::OpenGL:
        initialized = initializeOpenGLRenderer();
        break;
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    case QSGRendererInterface::Vulkan:
        initialized = initializeVulkanRenderer();
        break;
#endif
    default:
        setLastError(QStringLiteral("Unsupported Qt Quick graphics API"));
        return;
    }
    if (!initialized) return;

    m_rendererReady.store(true);
    QMetaObject::invokeMethod(this, &MirageDisplayItem::startConnection, Qt::QueuedConnection);
}

bool MirageDisplayItem::initializeOpenGLRenderer() {
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        setLastError(QStringLiteral("Qt Quick did not expose an OpenGL context"));
        return false;
    }
    auto* eglContext = context->nativeInterface<QNativeInterface::QEGLContext>();
    if (eglContext == nullptr || eglContext->display() == EGL_NO_DISPLAY) {
        setLastError(QStringLiteral("OpenGL scene graph is not using EGL"));
        return false;
    }

    md_egl_context_t importerContext {
        .display = eglContext->display(),
        .get_proc_address = nullptr,
    };
    m_importer = md_egl_importer_new(&importerContext);
    if (m_importer == nullptr) {
        setLastError(QStringLiteral("EGL DMA-BUF import is unavailable"));
        return false;
    }

    m_imageTargetTexture = reinterpret_cast<GlEglImageTargetTexture2D>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (m_imageTargetTexture == nullptr) {
        md_egl_importer_free(m_importer);
        m_importer = nullptr;
        setLastError(QStringLiteral("glEGLImageTargetTexture2DOES is unavailable"));
        return false;
    }
    setRendererBackend(BackendOpenGLEGL);
    setLastError({});
    return true;
}

#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
bool MirageDisplayItem::initializeVulkanRenderer() {
    QVulkanInstance* qtInstance = window()->vulkanInstance();
    QSGRendererInterface* renderer = window()->rendererInterface();
    if (qtInstance == nullptr || !qtInstance->isValid() || renderer == nullptr) {
        setLastError(QStringLiteral("Qt Quick did not expose a Vulkan instance"));
        return false;
    }
    auto* physicalPointer = static_cast<VkPhysicalDevice*>(
        renderer->getResource(window(), QSGRendererInterface::PhysicalDeviceResource));
    auto* devicePointer = static_cast<VkDevice*>(
        renderer->getResource(window(), QSGRendererInterface::DeviceResource));
    auto* queuePointer = static_cast<VkQueue*>(
        renderer->getResource(window(), QSGRendererInterface::CommandQueueResource));
    auto* familyPointer = static_cast<uint32_t*>(
        renderer->getResource(window(), QSGRendererInterface::GraphicsQueueFamilyIndexResource));
    if (physicalPointer == nullptr || devicePointer == nullptr || queuePointer == nullptr ||
        *physicalPointer == VK_NULL_HANDLE || *devicePointer == VK_NULL_HANDLE ||
        *queuePointer == VK_NULL_HANDLE) {
        setLastError(QStringLiteral("Qt Quick Vulkan device resources are incomplete"));
        return false;
    }
    const uint32_t queueFamily = familyPointer != nullptr ? *familyPointer : 0u;
    md_vk_context_t importerContext {
        .instance = qtInstance->vkInstance(),
        .physical_device = *physicalPointer,
        .device = *devicePointer,
        .queue_family_index = queueFamily,
        .image_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    m_vkImporter = md_vk_importer_new(&importerContext);
    if (m_vkImporter == nullptr) {
        setLastError(QStringLiteral("Vulkan DMA-BUF importer initialization failed"));
        return false;
    }
    md_vk_blit_context_t blitContext {
        .physical_device = *physicalPointer,
        .device = *devicePointer,
        .queue = *queuePointer,
        .queue_family_index = queueFamily,
    };
    m_vkBlitter = md_vk_blitter_new(&blitContext);
    if (m_vkBlitter == nullptr) {
        md_vk_importer_free(m_vkImporter);
        m_vkImporter = nullptr;
        setLastError(QStringLiteral("Vulkan relay initialization failed"));
        return false;
    }
    m_vkDevice = *devicePointer;
    VkPhysicalDeviceDrmPropertiesEXT drmProperties {};
    drmProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceIDProperties idProperties {};
    idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    idProperties.pNext = &drmProperties;
    VkPhysicalDeviceProperties2 physicalProperties {};
    physicalProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    physicalProperties.pNext = &idProperties;
    vkGetPhysicalDeviceProperties2(*physicalPointer, &physicalProperties);
    std::copy(std::begin(idProperties.deviceUUID), std::end(idProperties.deviceUUID),
              m_vkDeviceUuid.begin());
    std::copy(std::begin(idProperties.driverUUID), std::end(idProperties.driverUUID),
              m_vkDriverUuid.begin());
    if (drmProperties.hasRender == VK_TRUE && drmProperties.renderMajor >= 0 &&
        drmProperties.renderMinor >= 0) {
        m_drmRenderMajor = static_cast<uint32_t>(drmProperties.renderMajor);
        m_drmRenderMinor = static_cast<uint32_t>(drmProperties.renderMinor);
    }
    m_vkFormats.clear();
    const uint32_t fourccs[] = {
        DrmFormatXrgb8888, DrmFormatArgb8888,
        DrmFormatXbgr8888, DrmFormatAbgr8888,
    };
    for (uint32_t fourccValue : fourccs) {
        uint32_t count = 0;
        if (md_vk_query_format_caps(*physicalPointer, fourccValue,
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,
                                    nullptr, 0, &count) != MD_OK || count == 0) {
            continue;
        }
        QVector<md_format_cap_t> formats(static_cast<qsizetype>(count));
        if (md_vk_query_format_caps(*physicalPointer, fourccValue,
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,
                                    formats.data(), count, &count) == MD_OK) {
            formats.resize(static_cast<qsizetype>(count));
            m_vkFormats += formats;
        }
    }
    if (m_vkFormats.isEmpty()) {
        md_vk_blitter_free(m_vkBlitter);
        md_vk_importer_free(m_vkImporter);
        m_vkBlitter = nullptr;
        m_vkImporter = nullptr;
        m_vkDevice = VK_NULL_HANDLE;
        setLastError(QStringLiteral("Vulkan device exposes no importable RGB modifiers"));
        return false;
    }
    setRendererBackend(BackendVulkan);
    setLastError({});
    return true;
}
#endif

void MirageDisplayItem::invalidateRenderer() {
    if (!m_rendererReady.exchange(false) && m_importer == nullptr
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
        && m_vkImporter == nullptr
#endif
        ) return;

    uint64_t releaseGeneration = 0;
    bool finishRelease = false;
    {
        QMutexLocker locker(&m_stateMutex);
        releaseGeneration = m_releaseGeneration;
        finishRelease = m_releaseNeedsFinish;
        m_releaseGeneration = 0;
        m_releaseNeedsFinish = false;
    }

    releaseRenderPool();
    md_egl_importer_free(m_importer);
    m_importer = nullptr;
    m_imageTargetTexture = nullptr;
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    md_vk_blitter_free(m_vkBlitter);
    md_vk_importer_free(m_vkImporter);
    m_vkBlitter = nullptr;
    m_vkImporter = nullptr;
    m_vkDevice = VK_NULL_HANDLE;
    m_vkFormats.clear();
    m_vkDeviceUuid.fill(0);
    m_vkDriverUuid.fill(0);
#endif
    m_drmRenderMajor = 0;
    m_drmRenderMinor = 0;
    setRendererBackend(BackendNone);

    if (finishRelease && releaseGeneration != 0) {
        QMetaObject::invokeMethod(this, [this, releaseGeneration]() {
            finishDeferredUnbind(static_cast<qulonglong>(releaseGeneration));
        }, Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(this, &MirageDisplayItem::closeConnection, Qt::QueuedConnection);
}

void MirageDisplayItem::setSocketPath(const QString& value) {
    if (m_socketPath == value) return;
    m_socketPath = value;
    emit socketPathChanged();
    closeConnection();
    scheduleReconnect();
}

void MirageDisplayItem::setOutputStableId(const QString& value) {
    if (m_outputStableId == value) return;
    m_outputStableId = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setOutputName(const QString& value) {
    if (m_outputName == value) return;
    m_outputName = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setPhysicalWidth(int value) {
    value = std::max(value, 1);
    if (m_physicalWidth == value) return;
    m_physicalWidth = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setPhysicalHeight(int value) {
    value = std::max(value, 1);
    if (m_physicalHeight == value) return;
    m_physicalHeight = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setLogicalWidth(int value) {
    value = std::max(value, 1);
    if (m_logicalWidth == value) return;
    m_logicalWidth = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setLogicalHeight(int value) {
    value = std::max(value, 1);
    if (m_logicalHeight == value) return;
    m_logicalHeight = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setScale120(int value) {
    value = std::max(value, 1);
    if (m_scale120 == value) return;
    m_scale120 = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setRefreshMhz(int value) {
    value = std::max(value, 1);
    if (m_refreshMhz == value) return;
    m_refreshMhz = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setOutputTransform(OutputTransform value) {
    if (m_outputTransform == value) return;
    m_outputTransform = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setPointerForwarding(bool value) {
    if (m_pointerForwarding == value) return;
    if (!value) releasePointerState(monotonicTimestampUs());
    m_pointerForwarding = value;
    emit pointerForwardingChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setWindowStateFlags(quint32 value) {
    if (m_windowStateFlags == value) return;
    m_windowStateFlags = value;
    emit windowStateFlagsChanged();
    if (m_display != nullptr && md_display_connection_state(m_display) == MD_CONNECTION_READY) {
        (void)md_display_send_window_state(m_display, static_cast<uint32_t>(value));
        armWritable();
    }
}

void MirageDisplayItem::setRendererBackend(RendererBackend backend) {
    const RendererBackend previous = m_rendererBackend.exchange(backend);
    if (previous == backend) return;
    if (QThread::currentThread() == thread()) emit rendererBackendChanged();
    else QMetaObject::invokeMethod(this, [this]() { emit rendererBackendChanged(); },
                                   Qt::QueuedConnection);
}

void MirageDisplayItem::setLastError(const QString& error) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, error]() { setLastError(error); },
                                  Qt::QueuedConnection);
        return;
    }
    if (m_lastError == error) return;
    m_lastError = error;
    emit lastErrorChanged();
}

void MirageDisplayItem::setImportedGeneration(uint64_t generation) {
    const uint64_t previous = m_importedGeneration.exchange(generation);
    if (previous == generation) return;
    if (QThread::currentThread() == thread()) {
        emit importedGenerationChanged();
    } else {
        QMetaObject::invokeMethod(this, [this]() { emit importedGenerationChanged(); },
                                  Qt::QueuedConnection);
    }
}

md_output_info_t MirageDisplayItem::makeOutputInfo(QByteArray& stableId, QByteArray& name) const {
    stableId = m_outputStableId.trimmed().toUtf8();
    name = m_outputName.trimmed().toUtf8();
    if (stableId.isEmpty()) stableId = QByteArrayLiteral("kde:unknown");
    if (name.isEmpty()) name = QByteArrayLiteral("KDE wallpaper");

    uint32_t refreshMhz = positiveU32(m_refreshMhz, 60000);
    if (window() != nullptr && window()->screen() != nullptr &&
        window()->screen()->refreshRate() > 0.0) {
        const qreal screenRefresh = window()->screen()->refreshRate() * 1000.0;
        if (screenRefresh > 0.0 && screenRefresh < static_cast<qreal>(std::numeric_limits<uint32_t>::max())) {
            refreshMhz = static_cast<uint32_t>(screenRefresh);
        }
    }

    return md_output_info_t {
        .stable_id = stableId.constData(),
        .name = name.constData(),
        .physical_width = positiveU32(m_physicalWidth, 1),
        .physical_height = positiveU32(m_physicalHeight, 1),
        .logical_width = positiveU32(m_logicalWidth, 1),
        .logical_height = positiveU32(m_logicalHeight, 1),
        .scale_120 = positiveU32(m_scale120, 120),
        .refresh_mhz = refreshMhz,
        .transform = static_cast<md_transform_t>(m_outputTransform),
        .drm_render_major = m_drmRenderMajor.load(),
        .drm_render_minor = m_drmRenderMinor.load(),
        .input_caps = m_pointerForwarding
                          ? MD_INPUT_POINTER_ENTER_LEAVE | MD_INPUT_POINTER_MOTION |
                                MD_INPUT_POINTER_BUTTON | MD_INPUT_POINTER_AXIS |
                                MD_INPUT_NON_CONSUMING
                          : UINT64_C(0),
    };
}

void MirageDisplayItem::startConnection() {
    if (!isComponentComplete() || !m_rendererReady.load() || m_display != nullptr ||
        m_socketPath.isEmpty()) {
        return;
    }

    md_display_callbacks_t callbacks {
        .on_connected = &MirageDisplayItem::onConnected,
        .on_buffers_ready = &MirageDisplayItem::onBuffersReady,
        .on_buffers_releasing = &MirageDisplayItem::onBuffersReleasing,
        .on_config = &MirageDisplayItem::onConfig,
        .on_frame = &MirageDisplayItem::onFrame,
        .on_disconnected = &MirageDisplayItem::onDisconnected,
        .user_data = this,
    };
    m_display = md_display_new(&callbacks);
    if (m_display == nullptr) {
        setLastError(QStringLiteral("Cannot allocate display protocol client"));
        scheduleReconnect();
        return;
    }

    const md_format_cap_t eglFormats[] {
        {.fourcc = DrmFormatXrgb8888, .plane_count = 1, .modifier = 0},
        {.fourcc = DrmFormatArgb8888, .plane_count = 1, .modifier = 0},
        {.fourcc = DrmFormatXbgr8888, .plane_count = 1, .modifier = 0},
        {.fourcc = DrmFormatAbgr8888, .plane_count = 1, .modifier = 0},
    };
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    const md_format_cap_t* formats = eglFormats;
    uint32_t formatCount = static_cast<uint32_t>(std::size(eglFormats));
    uint64_t featureBits = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_POINTER_AXIS |
                           MD_FEATURE_WINDOW_STATE;
    if (m_rendererBackend.load() == BackendVulkan && !m_vkFormats.isEmpty()) {
        formats = m_vkFormats.constData();
        formatCount = static_cast<uint32_t>(m_vkFormats.size());
        featureBits |= MD_FEATURE_DRM_MODIFIERS;
    }
#else
    const md_format_cap_t* formats = eglFormats;
    const uint32_t formatCount = static_cast<uint32_t>(std::size(eglFormats));
    const uint64_t featureBits = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_POINTER_AXIS |
                                 MD_FEATURE_WINDOW_STATE;
#endif
    md_consumer_caps_t capabilities {
        .features = featureBits,
        .sync_caps = 1,
        .color_caps = 0,
        .max_width = 16384,
        .max_height = 16384,
        .device_uuid = {},
        .driver_uuid = {},
        .formats = formats,
        .format_count = formatCount,
    };
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    if (m_rendererBackend.load() == BackendVulkan) {
        std::copy(m_vkDeviceUuid.begin(), m_vkDeviceUuid.end(), capabilities.device_uuid);
        std::copy(m_vkDriverUuid.begin(), m_vkDriverUuid.end(), capabilities.driver_uuid);
    }
#endif
    QByteArray stableId;
    QByteArray outputNameBytes;
    md_output_info_t output = makeOutputInfo(stableId, outputNameBytes);
    const QByteArray socketBytes = m_socketPath.toUtf8();

    int result = md_display_begin_connect(m_display, socketBytes.constData(),
                                          "mirage-plasma", "0.1.0",
                                          &output, &capabilities);
    if (result != MD_OK) {
        setLastError(QStringLiteral("Cannot connect to Mirage display broker"));
        closeConnection();
        scheduleReconnect();
        return;
    }

    int fd = md_display_get_fd(m_display);
    if (fd < 0) {
        setLastError(QStringLiteral("Display broker connection has no socket"));
        closeConnection();
        scheduleReconnect();
        return;
    }
    m_readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    m_writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    connect(m_readNotifier, &QSocketNotifier::activated,
            this, &MirageDisplayItem::advanceHandshake);
    connect(m_writeNotifier, &QSocketNotifier::activated,
            this, &MirageDisplayItem::advanceHandshake);
    advanceHandshake();
}

void MirageDisplayItem::advanceHandshake() {
    if (m_display == nullptr) return;
    for (int iteration = 0; iteration < 16; ++iteration) {
        int result = md_display_advance_handshake(m_display);
        if (result == MD_HANDSHAKE_PROGRESS) continue;
        if (result == MD_HANDSHAKE_DONE) {
            disconnect(m_readNotifier, nullptr, this, nullptr);
            disconnect(m_writeNotifier, nullptr, this, nullptr);
            connect(m_readNotifier, &QSocketNotifier::activated,
                    this, &MirageDisplayItem::dispatchSocket);
            connect(m_writeNotifier, &QSocketNotifier::activated,
                    this, &MirageDisplayItem::flushSocket);
            m_readNotifier->setEnabled(true);
            m_writeNotifier->setEnabled(false);
            (void)md_display_send_window_state(m_display,
                                                static_cast<uint32_t>(m_windowStateFlags));
            armWritable();
            return;
        }
        if (result == MD_HANDSHAKE_NEED_READ || result == MD_HANDSHAKE_NEED_WRITE) {
            m_readNotifier->setEnabled(result == MD_HANDSHAKE_NEED_READ);
            m_writeNotifier->setEnabled(result == MD_HANDSHAKE_NEED_WRITE);
            return;
        }
        handleConnectionFailure();
        return;
    }
    handleConnectionFailure();
}

void MirageDisplayItem::dispatchSocket() {
    if (m_display == nullptr) return;
    int result = md_display_dispatch(m_display);
    if (result < 0) {
        handleConnectionFailure();
        return;
    }
    armWritable();
}

void MirageDisplayItem::flushSocket() {
    if (m_display == nullptr) return;
    if (md_display_handle_writable(m_display) < 0) {
        handleConnectionFailure();
        return;
    }
    armWritable();
}

void MirageDisplayItem::armWritable() {
    if (m_writeNotifier != nullptr && m_display != nullptr) {
        m_writeNotifier->setEnabled(md_display_wants_writable(m_display));
    }
}

void MirageDisplayItem::pushOutputUpdate() {
    if (m_display == nullptr || md_display_connection_state(m_display) != MD_CONNECTION_READY) {
        return;
    }
    QByteArray stableId;
    QByteArray outputNameBytes;
    md_output_info_t output = makeOutputInfo(stableId, outputNameBytes);
    if (md_display_update_output(m_display, &output) != MD_OK) return;
    armWritable();
}

void MirageDisplayItem::finishDeferredUnbind(qulonglong generation) {
    if (m_display == nullptr || generation == 0) return;
    if (md_display_finish_unbind(m_display, static_cast<uint64_t>(generation)) == MD_OK) {
        armWritable();
    }
}

void MirageDisplayItem::closeConnection() {
    releasePointerState(monotonicTimestampUs());
    if (m_readNotifier != nullptr) {
        delete m_readNotifier;
        m_readNotifier = nullptr;
    }
    if (m_writeNotifier != nullptr) {
        delete m_writeNotifier;
        m_writeNotifier = nullptr;
    }
    if (m_display != nullptr) {
        md_display_free(m_display);
        m_display = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
    if (m_outputId != 0) {
        m_outputId = 0;
        emit outputIdChanged();
    }
}

void MirageDisplayItem::handleConnectionFailure() {
    closeConnection();
    scheduleReconnect();
}

void MirageDisplayItem::scheduleReconnect() {
    if (isComponentComplete() && m_rendererReady.load() && !m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void MirageDisplayItem::onConnected(void* userData, uint64_t outputIdValue) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    self->m_connected = true;
    self->m_outputId = static_cast<qulonglong>(outputIdValue);
    self->setLastError({});
    emit self->connectedChanged();
    emit self->outputIdChanged();
}

void MirageDisplayItem::onBuffersReady(void* userData, const md_buffer_pool_t* pool) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    {
        QMutexLocker locker(&self->m_stateMutex);
        self->m_pendingPool = *pool;
        self->m_hasPendingPool = true;
    }
    self->update();
}

void MirageDisplayItem::onBuffersReleasing(void* userData, const md_buffer_pool_t* pool) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    bool deferred = self->m_display != nullptr &&
                    md_display_defer_unbind(self->m_display) == MD_OK;
    {
        QMutexLocker locker(&self->m_stateMutex);
        self->m_releaseGeneration = pool->generation;
        self->m_releaseNeedsFinish = deferred;
    }
    self->update();
    if (self->window()) self->window()->update();
}

void MirageDisplayItem::onConfig(void* userData, const md_display_config_t* config) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    {
        QMutexLocker locker(&self->m_stateMutex);
        self->m_config = *config;
        self->m_hasConfig = true;
    }
    QColor next = QColor::fromRgbF(config->clear_color[0], config->clear_color[1],
                                   config->clear_color[2], config->clear_color[3]);
    if (next != self->m_clearColor) {
        self->m_clearColor = next;
        emit self->clearColorChanged();
    }
    self->update();
}

void MirageDisplayItem::dropFrame(PendingFrame& frame) {
    if (!frame.valid) return;
    if (frame.value.acquire_sync_fd >= 0) close(frame.value.acquire_sync_fd);
    if (frame.value.release_syncobj_fd >= 0) {
        (void)md_display_signal_release_syncobj(frame.value.release_syncobj_fd);
    }
    frame = PendingFrame {};
}

void MirageDisplayItem::onFrame(void* userData, const md_frame_t* frame) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    {
        QMutexLocker locker(&self->m_stateMutex);
        dropFrame(self->m_pendingFrame);
        self->m_pendingFrame.valid = true;
        self->m_pendingFrame.value = *frame;
    }
    ++self->m_framesReceived;
    emit self->framesReceivedChanged();
    self->update();
}

void MirageDisplayItem::onDisconnected(void* userData, md_result_t reason, const char* message) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    self->releasePointerState(monotonicTimestampUs());
    self->setLastError(QStringLiteral("Disconnected (%1): %2")
                           .arg(static_cast<int>(reason))
                           .arg(QString::fromUtf8(message != nullptr ? message : "unknown error")));
    if (self->m_connected) {
        self->m_connected = false;
        emit self->connectedChanged();
    }
}

bool MirageDisplayItem::importPendingPool(const md_buffer_pool_t& pool) {
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    if (m_rendererBackend.load() == BackendVulkan) {
        if (m_vkImporter == nullptr || md_vk_importer_import_pool(m_vkImporter, &pool) != MD_OK) {
            setLastError(QStringLiteral("Vulkan DMA-BUF pool import failed"));
            return false;
        }
        setImportedGeneration(pool.generation);
        setLastError({});
        return true;
    }
#endif
    if (m_importer == nullptr || m_imageTargetTexture == nullptr || window() == nullptr) {
        return false;
    }
    if (md_egl_importer_import_pool(m_importer, &pool) != MD_OK) return false;

    const md_egl_imported_pool_t* imported = md_egl_importer_pool(m_importer);
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (imported == nullptr || context == nullptr) {
        md_egl_importer_release_pool(m_importer);
        return false;
    }

    QOpenGLFunctions* functions = context->functions();
    m_glTextures.resize(static_cast<qsizetype>(imported->buffer_count));
    functions->glGenTextures(static_cast<int>(imported->buffer_count), m_glTextures.data());
    const bool hasAlpha = imported->fourcc == DrmFormatArgb8888;
    const auto options = hasAlpha ? QQuickWindow::TextureHasAlphaChannel
                                  : QQuickWindow::CreateTextureOptions {};

    for (uint32_t index = 0; index < imported->buffer_count; ++index) {
        unsigned int texture = m_glTextures[static_cast<qsizetype>(index)];
        functions->glBindTexture(GL_TEXTURE_2D, texture);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_imageTargetTexture(GL_TEXTURE_2D, imported->images[index]);
        QSGTexture* wrapper = QNativeInterface::QSGOpenGLTexture::fromNative(
            texture, window(), QSize(static_cast<int>(imported->width),
                                     static_cast<int>(imported->height)), options);
        if (wrapper == nullptr) {
            releaseRenderPool();
            return false;
        }
        m_qsgTextures.append(wrapper);
    }
    functions->glBindTexture(GL_TEXTURE_2D, 0);
    setImportedGeneration(pool.generation);
    setLastError({});
    return true;
}

void MirageDisplayItem::releaseRenderPool() {
    if (m_activeReleaseFd >= 0) {
        if (QOpenGLContext::currentContext() != nullptr) {
            QOpenGLContext::currentContext()->functions()->glFinish();
        }
        (void)md_display_signal_release_syncobj(m_activeReleaseFd);
        m_activeReleaseFd = -1;
    }

    {
        QMutexLocker locker(&m_stateMutex);
        dropFrame(m_pendingFrame);
        m_hasPendingPool = false;
    }

    qDeleteAll(m_qsgTextures);
    m_qsgTextures.clear();
    if (!m_glTextures.isEmpty() && QOpenGLContext::currentContext() != nullptr) {
        QOpenGLContext::currentContext()->functions()->glDeleteTextures(
            static_cast<int>(m_glTextures.size()), m_glTextures.constData());
    }
    m_glTextures.clear();
    if (m_importer != nullptr) md_egl_importer_release_pool(m_importer);
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    if (m_vkDevice != VK_NULL_HANDLE) (void)vkDeviceWaitIdle(m_vkDevice);
    if (m_vkImporter != nullptr) md_vk_importer_release_pool(m_vkImporter);
#endif
    setImportedGeneration(0);
    m_currentBuffer = -1;
}

void MirageDisplayItem::releaseAfterRendering() {
    if (m_activeReleaseFd < 0) return;
    int releaseFd = m_activeReleaseFd;
    m_activeReleaseFd = -1;
    if (m_importer == nullptr ||
        md_egl_release_after_current_context(m_importer, releaseFd) != MD_OK) {
        return;
    }
}

QSGNode* MirageDisplayItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data);

    uint64_t releaseGeneration = 0;
    bool finishRelease = false;
    {
        QMutexLocker locker(&m_stateMutex);
        releaseGeneration = m_releaseGeneration;
        finishRelease = m_releaseNeedsFinish;
        if (releaseGeneration != 0) {
            m_releaseGeneration = 0;
            m_releaseNeedsFinish = false;
        }
    }
    if (releaseGeneration != 0) {
        delete oldNode;
        oldNode = nullptr;
        releaseRenderPool();
        if (finishRelease) {
            QMetaObject::invokeMethod(this, [this, releaseGeneration]() {
                finishDeferredUnbind(static_cast<qulonglong>(releaseGeneration));
            }, Qt::QueuedConnection);
        }
        return nullptr;
    }

    md_buffer_pool_t pendingPool {};
    bool importPool = false;
    if (m_importer != nullptr
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
        || m_vkImporter != nullptr
#endif
        ) {
        QMutexLocker locker(&m_stateMutex);
        if (m_hasPendingPool) {
            pendingPool = m_pendingPool;
            m_hasPendingPool = false;
            importPool = true;
        }
    }
    if (importPool && !importPendingPool(pendingPool)) {
        delete oldNode;
        oldNode = nullptr;
    }

    PendingFrame frame;
    md_display_config_t config {};
    bool hasConfig = false;
    {
        QMutexLocker locker(&m_stateMutex);
        frame = m_pendingFrame;
        m_pendingFrame = PendingFrame {};
        config = m_config;
        hasConfig = m_hasConfig;
    }

    if (frame.valid) {
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
        if (m_rendererBackend.load() == BackendVulkan) {
            const md_vk_imported_pool_t* imported = md_vk_importer_pool(m_vkImporter);
            bool valid = imported != nullptr && m_vkBlitter != nullptr &&
                         frame.value.buffer_generation == m_importedGeneration.load() &&
                         frame.value.buffer_index < imported->buffer_count;
            if (!valid) {
                dropFrame(frame);
            } else {
                const bool shadowChanged = md_vk_blitter_image(m_vkBlitter) != VK_NULL_HANDLE &&
                    (md_vk_blitter_width(m_vkBlitter) != imported->width ||
                     md_vk_blitter_height(m_vkBlitter) != imported->height ||
                     md_vk_blitter_format(m_vkBlitter) != imported->format);
                if (shadowChanged) {
                    delete oldNode;
                    oldNode = nullptr;
                    qDeleteAll(m_qsgTextures);
                    m_qsgTextures.clear();
                }
                VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
                int acquireFd = frame.value.acquire_sync_fd;
                frame.value.acquire_sync_fd = -1;
                int rc = md_vk_import_acquire_sync(m_vkImporter, frame.value.buffer_index,
                                                   acquireFd, &acquireSemaphore);
                if (rc != MD_OK) {
                    if (frame.value.release_syncobj_fd >= 0) {
                        (void)md_display_signal_release_syncobj(frame.value.release_syncobj_fd);
                        frame.value.release_syncobj_fd = -1;
                    }
                    setLastError(QStringLiteral("Vulkan acquire sync import failed"));
                } else {
                    /* The producer's release object is a DRM syncobj fd, not a
                     * Vulkan opaque semaphore, so it must be signalled with
                     * md_display_signal_release_syncobj once this consumer's GPU
                     * has finished reading the buffer. The blit is synchronous;
                     * on a fence timeout the submission is already in flight, so
                     * drain the device before releasing the slot. */
                    int releaseFd = frame.value.release_syncobj_fd;
                    frame.value.release_syncobj_fd = -1;
                    rc = md_vk_blitter_blit(m_vkBlitter, imported,
                                            frame.value.buffer_index,
                                            acquireSemaphore, VK_NULL_HANDLE);
                    if (rc == MD_ERR_WOULD_BLOCK && m_vkDevice != VK_NULL_HANDLE) {
                        if (vkDeviceWaitIdle(m_vkDevice) == VK_SUCCESS) rc = MD_OK;
                    }
                    if (rc == MD_OK) {
                        if (releaseFd >= 0) {
                            (void)md_display_signal_release_syncobj(releaseFd);
                        }
                        m_currentBuffer = 0;
                        setLastError({});
                    } else {
                        if (releaseFd >= 0) {
                            (void)md_display_signal_release_syncobj(releaseFd);
                        }
                        setLastError(QStringLiteral("Vulkan frame relay failed"));
                    }
                }
            }
        } else
#endif
        {
        bool valid = frame.value.buffer_generation == m_importedGeneration.load() &&
                     frame.value.buffer_index < static_cast<uint32_t>(m_qsgTextures.size());
        const int waitResult = m_importer != nullptr
                                   ? md_egl_wait_acquire_sync(m_importer, frame.value.acquire_sync_fd)
                                   : MD_ERR_INVALID;
        if (!valid || waitResult != MD_OK) {
            frame.value.acquire_sync_fd = -1;
            if (frame.value.release_syncobj_fd >= 0) {
                (void)md_display_signal_release_syncobj(frame.value.release_syncobj_fd);
            }
        } else {
            frame.value.acquire_sync_fd = -1;
            if (m_activeReleaseFd >= 0) {
                QOpenGLContext::currentContext()->functions()->glFinish();
                (void)md_display_signal_release_syncobj(m_activeReleaseFd);
            }
            m_activeReleaseFd = frame.value.release_syncobj_fd;
            frame.value.release_syncobj_fd = -1;
            m_currentBuffer = static_cast<int>(frame.value.buffer_index);
        }
        }
    }

#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    if (m_rendererBackend.load() == BackendVulkan && m_vkBlitter != nullptr &&
        md_vk_blitter_has_content(m_vkBlitter) && m_qsgTextures.isEmpty() && window() != nullptr) {
        QSGTexture* wrapper = QNativeInterface::QSGVulkanTexture::fromNative(
            md_vk_blitter_image(m_vkBlitter), md_vk_blitter_layout(m_vkBlitter), window(),
            QSize(static_cast<int>(md_vk_blitter_width(m_vkBlitter)),
                  static_cast<int>(md_vk_blitter_height(m_vkBlitter))));
        if (wrapper != nullptr) m_qsgTextures.append(wrapper);
    }
#endif

    if (m_currentBuffer < 0 || m_currentBuffer >= m_qsgTextures.size()) {
        delete oldNode;
        return nullptr;
    }

    QSGTransformNode* transformNode = nullptr;
    QSGSimpleTextureNode* node = nullptr;
    if (oldNode != nullptr && oldNode->type() == QSGNode::TransformNodeType) {
        transformNode = static_cast<QSGTransformNode*>(oldNode);
        node = static_cast<QSGSimpleTextureNode*>(transformNode->firstChild());
    } else {
        delete oldNode;
        transformNode = new QSGTransformNode();
        node = new QSGSimpleTextureNode();
        node->setFiltering(QSGTexture::Linear);
        node->setOwnsTexture(false);
        transformNode->appendChildNode(node);
    }
    node->setTexture(m_qsgTextures[m_currentBuffer]);

    const QRectF bounds = boundingRect();
    uint32_t importedWidth = 0;
    uint32_t importedHeight = 0;
    if (m_importer != nullptr) {
        const md_egl_imported_pool_t* imported = md_egl_importer_pool(m_importer);
        if (imported != nullptr) {
            importedWidth = imported->width;
            importedHeight = imported->height;
        }
    }
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    if (m_vkBlitter != nullptr) {
        importedWidth = md_vk_blitter_width(m_vkBlitter);
        importedHeight = md_vk_blitter_height(m_vkBlitter);
    }
#endif
    if (hasConfig && config.source.width > 0.0f && config.source.height > 0.0f) {
        node->setSourceRect(QRectF(config.source.x, config.source.y,
                                   config.source.width, config.source.height));
    } else if (importedWidth > 0 && importedHeight > 0) {
        node->setSourceRect(QRectF(0.0, 0.0, importedWidth, importedHeight));
    }

    if (hasConfig && config.destination.width > 0.0f && config.destination.height > 0.0f &&
        m_physicalWidth > 0 && m_physicalHeight > 0) {
        const qreal scaleX = bounds.width() / static_cast<qreal>(m_physicalWidth);
        const qreal scaleY = bounds.height() / static_cast<qreal>(m_physicalHeight);
        node->setRect(QRectF(config.destination.x * scaleX,
                             config.destination.y * scaleY,
                             config.destination.width * scaleX,
                             config.destination.height * scaleY));
    } else {
        node->setRect(bounds);
    }

    const uint32_t transform = hasConfig ? static_cast<uint32_t>(config.transform)
                                         : static_cast<uint32_t>(m_outputTransform);
    const bool swapsDimensions = transform == MD_TRANSFORM_90 ||
                                 transform == MD_TRANSFORM_270 ||
                                 transform == MD_TRANSFORM_FLIPPED_90 ||
                                 transform == MD_TRANSFORM_FLIPPED_270;
    const qreal preWidth = swapsDimensions ? bounds.height() : bounds.width();
    const qreal preHeight = swapsDimensions ? bounds.width() : bounds.height();
    QMatrix4x4 matrix;
    if (transform != MD_TRANSFORM_NORMAL) {
        matrix.translate(static_cast<float>(bounds.width() / 2.0),
                         static_cast<float>(bounds.height() / 2.0));
        switch (transform) {
        case MD_TRANSFORM_90: matrix.rotate(90.0f, 0.0f, 0.0f, 1.0f); break;
        case MD_TRANSFORM_180: matrix.rotate(180.0f, 0.0f, 0.0f, 1.0f); break;
        case MD_TRANSFORM_270: matrix.rotate(270.0f, 0.0f, 0.0f, 1.0f); break;
        case MD_TRANSFORM_FLIPPED: matrix.scale(-1.0f, 1.0f, 1.0f); break;
        case MD_TRANSFORM_FLIPPED_90:
            matrix.rotate(90.0f, 0.0f, 0.0f, 1.0f);
            matrix.scale(-1.0f, 1.0f, 1.0f);
            break;
        case MD_TRANSFORM_FLIPPED_180:
            matrix.rotate(180.0f, 0.0f, 0.0f, 1.0f);
            matrix.scale(-1.0f, 1.0f, 1.0f);
            break;
        case MD_TRANSFORM_FLIPPED_270:
            matrix.rotate(270.0f, 0.0f, 0.0f, 1.0f);
            matrix.scale(-1.0f, 1.0f, 1.0f);
            break;
        default: break;
        }
        matrix.translate(static_cast<float>(-preWidth / 2.0),
                         static_cast<float>(-preHeight / 2.0));
    }
    if (transformNode->matrix() != matrix) {
        transformNode->setMatrix(matrix);
        transformNode->markDirty(QSGNode::DirtyMatrix);
    }
    return transformNode;
}

uint64_t MirageDisplayItem::monotonicTimestampUs() {
    struct timespec value {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<uint64_t>(value.tv_sec) * UINT64_C(1000000) +
           static_cast<uint64_t>(value.tv_nsec) / UINT64_C(1000);
}

void MirageDisplayItem::releasePointerState(uint64_t timestamp) {
    m_pointer.reset(timestamp);
}

void MirageDisplayItem::forwardPointerEvent(const MiragePointerForwarder::Event& event) {
    if (m_display == nullptr || md_display_connection_state(m_display) != MD_CONNECTION_READY) {
        return;
    }
    switch (event.type) {
    case MiragePointerForwarder::Event::Type::Enter:
        (void)md_display_send_pointer_enter(m_display, event.x, event.y, event.timestamp);
        break;
    case MiragePointerForwarder::Event::Type::Leave:
        (void)md_display_send_pointer_leave(m_display, event.timestamp);
        break;
    case MiragePointerForwarder::Event::Type::Motion:
        (void)md_display_send_pointer_motion(m_display, event.x, event.y,
                                              event.timestamp, event.modifiers);
        break;
    case MiragePointerForwarder::Event::Type::Button:
        (void)md_display_send_pointer_button(m_display, event.x, event.y, event.button,
                                             event.buttonState, event.timestamp,
                                             event.modifiers);
        break;
    case MiragePointerForwarder::Event::Type::Axis:
        (void)md_display_send_pointer_axis(m_display, event.x, event.y, event.deltaX,
                                           event.deltaY, event.axisSource, event.timestamp,
                                           event.modifiers);
        break;
    }
    armWritable();
}

bool MirageDisplayItem::eventFilter(QObject* watched, QEvent* event) {
    if (!m_pointerForwarding || watched != window() || m_display == nullptr ||
        md_display_connection_state(m_display) != MD_CONNECTION_READY) {
        return false;
    }

    m_pointer.setGeometry(boundingRect(), m_physicalWidth, m_physicalHeight);

    switch (event->type()) {
    case QEvent::MouseMove: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const uint64_t timestamp = monotonicTimestampUs();
        (void)m_pointer.handleMove(mapFromScene(mouse->scenePosition()), mouse->modifiers(),
                                   timestamp);
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const bool pressed = event->type() == QEvent::MouseButtonPress;
        const uint64_t timestamp = monotonicTimestampUs();
        (void)m_pointer.handleButton(mapFromScene(mouse->scenePosition()), mouse->button(),
                                     pressed, mouse->modifiers(), timestamp);
        break;
    }
    case QEvent::Wheel: {
        auto* wheel = static_cast<QWheelEvent*>(event);
        const uint64_t timestamp = monotonicTimestampUs();
        (void)m_pointer.handleWheel(mapFromScene(wheel->scenePosition()), wheel->angleDelta(),
                                    wheel->pixelDelta(), wheel->modifiers(), timestamp);
        break;
    }
    case QEvent::Leave:
        (void)m_pointer.handleLeave(monotonicTimestampUs());
        break;
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
        releasePointerState(monotonicTimestampUs());
        break;
    default:
        return false;
    }
    armWritable();
    return false;
}

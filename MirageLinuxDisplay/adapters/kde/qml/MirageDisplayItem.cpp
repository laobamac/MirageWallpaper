#include "MirageDisplayItem.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <QByteArray>
#include <QDebug>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
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
#include <QtGui/qguiapplication_platform.h>
#include <QtCore/qnativeinterface.h>
#include <QtQuick/qsgtexture_platform.h>
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <functional>
#include <filesystem>
#include <limits>
#include <poll.h>
#include <string>
#include <system_error>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/*
 * Implementation of MirageDisplayItem.
 *
 * The EGL/Vulkan import paths are selected per scene-graph backend.  Pool
 * replacement crosses from the protocol event thread into the render thread via
 * md_display_defer_unbind, and the event filter returns false so Plasma keeps
 * desktop clicks, context menus, drag-and-drop, and wheel events.
 */

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

#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
/* Formats a DRM fourcc as a printable four-character code ("XBGR"). */
QString fourccString(uint32_t fourccValue) {
    QByteArray bytes(4, Qt::Uninitialized);
    for (int index = 0; index < 4; ++index) {
        const char value = static_cast<char>((fourccValue >> (index * 8)) & 0xFFU);
        bytes[index] = (value >= 0x20 && value < 0x7F) ? value : '.';
    }
    return QString::fromLatin1(bytes);
}
#endif

#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
/*
 * Turns the importer's structured failure record into a one-line diagnostic
 * for the KDE wallpaper overlay, e.g.:
 *   image creation failed (VkResult=VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT), fourcc=XBGR modifier=0x0, buffer=0
 * The stage phrase plus VkResult/errno pin down which Vulkan call rejected the
 * pool, which is what the previous single generic message could not tell.
 */
QString describeVkImportFailure(const md_vk_import_error_t* error) {
    if (error == nullptr) return QStringLiteral("no failure record");
    QString description = QString::fromLatin1(md_vk_import_stage_string(error->stage));
    if (error->vk_result != VK_SUCCESS) {
        description += QStringLiteral(" (VkResult=%1)")
                           .arg(QString::fromLatin1(md_vk_result_string(error->vk_result)));
    } else if (error->sys_errno != 0) {
        description += QStringLiteral(" (errno=%1: %2)")
                           .arg(error->sys_errno)
                           .arg(QString::fromUtf8(strerror(error->sys_errno)));
    }
    if (error->candidate_count >= 0) {
        description += QStringLiteral(", memory candidates tried=%1")
                           .arg(error->candidate_count);
    }
    description += QStringLiteral(", fourcc=%1 modifier=0x%2")
                       .arg(fourccString(error->fourcc))
                       .arg(static_cast<qulonglong>(error->modifier), 0, 16);
    if (error->buffer_index != UINT32_MAX) {
        description += QStringLiteral(", buffer=%1").arg(error->buffer_index);
    }
    if (error->plane_index != UINT32_MAX) {
        description += QStringLiteral(", plane=%1").arg(error->plane_index);
    }
    if (error->stage == MD_VK_IMPORT_STAGE_MEMORY_PROPERTIES &&
        error->vk_result == VK_ERROR_EXTENSION_NOT_PRESENT) {
        // Defensive fallback: initializeVulkanRenderer() should already have
        // intercepted this case, but a late EXTENSION_NOT_PRESENT still needs
        // the actionable hint instead of a bare result code.
        description += QStringLiteral(
            " (VK_EXT_external_memory_dma_buf is not enabled on the scene-graph "
            "device; enable it via QT_VULKAN_DEVICE_EXTENSIONS or use the "
            "OpenGL render backend)");
    }
    return description;
}
#endif

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

/* Renderer initialization retry pacing. The first attempts are quick because
 * the common case is a short startup race, then the interval grows so a setup
 * that will never work stops costing render-thread wakeups. */
constexpr int kRendererRetryBaseIntervalMs = 250;
constexpr int kRendererRetryMaxIntervalMs = 4000;
constexpr int kRendererRetryMaxAttempts = 20;

} // namespace


/*
 * Sets up the reconnect/output-update timers, derives the default broker
 * socket path from $XDG_RUNTIME_DIR, and wires the pointer forwarder sink so Qt
 * pointer events reach the display session.
 */
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

    connect(&m_brokerWatcher, &QFileSystemWatcher::directoryChanged,
            this, &MirageDisplayItem::brokerDirectoryChanged);
    if (!runtimeDirectory.isEmpty()) {
        if (!m_brokerWatcher.addPath(runtimeDirectory)) {
            qWarning() << "[KDE wallpaper] Cannot watch broker directory:" << runtimeDirectory;
        }
    }
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(2000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &MirageDisplayItem::startConnection);

    /* A broker that accepts the socket but never completes the handshake (it
     * can still be starting up) would otherwise leave m_display non-null
     * forever, and that pointer is what blocks every reconnect attempt. */
    m_handshakeTimeoutTimer.setSingleShot(true);
    m_handshakeTimeoutTimer.setInterval(5000);
    connect(&m_handshakeTimeoutTimer, &QTimer::timeout,
            this, &MirageDisplayItem::abortStalledHandshake);

    m_outputUpdateTimer.setSingleShot(true);
    m_outputUpdateTimer.setInterval(25);
    connect(&m_outputUpdateTimer, &QTimer::timeout, this, &MirageDisplayItem::pushOutputUpdate);

    /* The scene graph emits sceneGraphInitialized only once, so a renderer
     * initialization that fails because the GPU context is not usable yet (a
     * real race for the first wallpaper created during Plasma startup) would
     * otherwise never be retried, leaving the item permanently disconnected.
     * The interval backs off and stops after kRendererRetryMaxAttempts so a
     * genuinely unsupported GPU setup does not wake the render thread forever. */
    m_rendererRetryTimer.setSingleShot(true);
    m_rendererRetryTimer.setInterval(kRendererRetryBaseIntervalMs);
    connect(&m_rendererRetryTimer, &QTimer::timeout, this, [this]() {
        QQuickWindow* quickWindow = window();
        if (quickWindow == nullptr || m_rendererReady.load()) return;
        QPointer<MirageDisplayItem> guard(this);
        quickWindow->scheduleRenderJob(new FunctionJob([guard]() {
            if (guard) guard->initializeRenderer();
        }), QQuickWindow::BeforeSynchronizingStage);
        quickWindow->update();
    });
    connect(this, &QQuickItem::windowChanged, this, &MirageDisplayItem::handleWindowChanged);
}

MirageDisplayItem::~MirageDisplayItem() {
    m_reconnectTimer.stop();
    m_outputUpdateTimer.stop();
    m_rendererRetryTimer.stop();
    if (m_filteredWindow) m_filteredWindow->removeEventFilter(this);
    closeConnection();
}

void MirageDisplayItem::componentComplete() {
    QQuickItem::componentComplete();
    if (window()) handleWindowChanged(window());
}


/*
 * Tracks the owning QQuickWindow: installs the pointer event filter, requests
 * the Vulkan device extensions the importer needs before the scene graph
 * initializes, and connects the scene-graph lifecycle signals (render-thread
 * jobs are executed with DirectConnection because they must run on the render
 * thread).
 */
void MirageDisplayItem::handleWindowChanged(QQuickWindow* quickWindow) {
    if (m_filteredWindow && m_filteredWindow != quickWindow) {
        m_filteredWindow->removeEventFilter(this);
        disconnect(m_filteredWindow, nullptr, this, nullptr);
    }
    if (quickWindow == nullptr) {
        if (m_filteredWindow != nullptr) {
            disconnect(m_filteredWindow, nullptr, this, nullptr);
        }
        m_reconnectTimer.stop();
        m_rendererRetryTimer.stop();
        const QStringList failedPaths = m_brokerWatcher.removePaths(m_brokerWatcher.directories());
        if (!failedPaths.isEmpty()) {
            qWarning() << "[KDE wallpaper] Cannot remove broker directory watches:" << failedPaths;
        }
        m_filteredWindow = nullptr;
        return;
    }
    quickWindow->installEventFilter(this);
    m_filteredWindow = quickWindow;
    m_rendererRetryAttempts = 0;

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
    } else {
        /* sceneGraphInitialized may already have been emitted before this item
         * observed the window; the retry timer covers that lost edge. */
        scheduleRendererRetry();
    }
}


/*
 * Requests another renderer initialization attempt.  Called from the render
 * thread when the GPU context is not usable yet; the timer lives on the main
 * thread, so starting it is marshalled there.
 */
void MirageDisplayItem::scheduleRendererRetry() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, &MirageDisplayItem::scheduleRendererRetry,
                                  Qt::QueuedConnection);
        return;
    }
    if (m_rendererReady.load() || window() == nullptr) return;
    if (m_rendererRetryTimer.isActive()) return;
    if (m_rendererRetryAttempts >= kRendererRetryMaxAttempts) {
        setLastError(QStringLiteral(
            "Renderer initialization kept failing; recreate the wallpaper or "
            "restart plasmashell once the GPU stack is available."));
        return;
    }
    ++m_rendererRetryAttempts;
    m_rendererRetryTimer.setInterval(
        std::min(kRendererRetryBaseIntervalMs * m_rendererRetryAttempts,
                 kRendererRetryMaxIntervalMs));
    m_rendererRetryTimer.start();
}


/*
 * Selects the import backend from the Qt Quick graphics API once the scene
 * graph is initialized, then starts the broker connection on the main thread.
 * Called from the render thread via BeforeSynchronizingStage.
 */
void MirageDisplayItem::initializeRenderer() {
    if (m_rendererReady.load()) return;
    if (window() == nullptr || window()->rendererInterface() == nullptr) {
        scheduleRendererRetry();
        return;
    }

    const QSGRendererInterface::GraphicsApi graphicsApi =
        window()->rendererInterface()->graphicsApi();
    switch (m_rendererBackendPreference) {
    case BackendPreferenceAuto:
        break;
    case BackendPreferenceOpenGL:
        if (graphicsApi != QSGRendererInterface::OpenGL) {
            setLastError(QStringLiteral(
                "OpenGL backend was requested, but plasmashell is using a different "
                "Qt Quick graphics API; restart plasmashell with the OpenGL backend."));
            return;
        }
        break;
    case BackendPreferenceVulkan:
        if (graphicsApi != QSGRendererInterface::Vulkan) {
            setLastError(QStringLiteral(
                "Vulkan backend was requested, but plasmashell is using a different "
                "Qt Quick graphics API; restart plasmashell with the Vulkan backend."));
            return;
        }
        break;
    default:
        setLastError(QStringLiteral("Unsupported renderer backend preference"));
        return;
    }

    bool initialized = false;
    switch (graphicsApi) {
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
    if (!initialized) {
        /* The backend importers fail when the GPU context is not fully usable
         * yet, which is transient during Plasma startup; retry so the item is
         * not stuck without a renderer (and therefore without a connection). */
        scheduleRendererRetry();
        return;
    }

    m_rendererReady.store(true);
    /* m_rendererRetryAttempts belongs to the main thread (scheduleRendererRetry
     * reads and writes it there), and this function runs on the render thread,
     * so the reset is marshalled instead of assigned directly. */
    QMetaObject::invokeMethod(this, [this]() {
        m_rendererRetryAttempts = 0;
        m_rendererRetryTimer.stop();
        startConnection();
    }, Qt::QueuedConnection);
}


/*
 * Creates the EGL importer from the current QOpenGLContext.  Qt Quick uses
 * EGL on Wayland, but Plasma X11 may use GLX.  In the latter case the EGL
 * display is initialized from Qt's X11 connection solely for DMA-BUF image
 * creation; GL textures still belong to the GLX scene graph context.
 */
bool MirageDisplayItem::initializeOpenGLRenderer() {
    m_glDiagnostics = qEnvironmentVariableIsSet("MIRAGE_DISPLAY_DIAGNOSTICS");
    m_glDiagnosticFrames = 0;
    m_glAcquireWaitUs = 0;
    m_glPoolImportUs = 0;
    m_glReleaseUs = 0;
    m_glDroppedFrames = 0;
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        setLastError(QStringLiteral("Qt Quick did not expose an OpenGL context"));
        return false;
    }
    auto* eglContext = context->nativeInterface<QNativeInterface::QEGLContext>();
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    bool ownsEglDisplay = false;
    if (eglContext != nullptr) {
        eglDisplay = eglContext->display();
        if (eglDisplay == EGL_NO_DISPLAY) {
            setLastError(QStringLiteral("Qt Quick EGL context has no display"));
            return false;
        }
    } else {
        auto* glxContext = context->nativeInterface<QNativeInterface::QGLXContext>();
        QGuiApplication* application = qApp;
        auto* x11Application = application != nullptr
                                   ? application->nativeInterface<QNativeInterface::QX11Application>()
                                   : nullptr;
        if (glxContext == nullptr || glxContext->nativeContext() == nullptr ||
            x11Application == nullptr || x11Application->display() == nullptr) {
            setLastError(QStringLiteral("OpenGL scene graph has no EGL or GLX/X11 display"));
            return false;
        }
        eglDisplay = eglGetDisplay(x11Application->display());
        if (eglDisplay == EGL_NO_DISPLAY) {
            setLastError(QStringLiteral("Cannot obtain EGL display for the GLX scene graph"));
            return false;
        }
        EGLint majorVersion = 0;
        EGLint minorVersion = 0;
        if (eglInitialize(eglDisplay, &majorVersion, &minorVersion) != EGL_TRUE) {
            setLastError(QStringLiteral("Cannot initialize EGL display for the GLX scene graph"));
            return false;
        }
        ownsEglDisplay = true;
    }

    /* The GLX EGL display is owned only during this initialization attempt.
     * Every later failure must terminate it, while Qt retains ownership of an
     * EGL display obtained from QEGLContext. */
    const auto failInitialization = [this, eglDisplay, ownsEglDisplay](const QString& error) {
        if (ownsEglDisplay && eglTerminate(eglDisplay) != EGL_TRUE) {
            qWarning() << "[KDE wallpaper] Cannot terminate GLX EGL display";
        }
        setLastError(error);
        return false;
    };

    /* The EGL display targets the consumer scene-graph GPU. Report its DRM
     * render node before connecting, so the broker can make producers create
     * resources on the same device. */
    const auto queryDisplayAttrib = std::bit_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDisplayAttribEXT"));
    const auto queryDeviceString = std::bit_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
        eglGetProcAddress("eglQueryDeviceStringEXT"));
    if (queryDisplayAttrib == nullptr || queryDeviceString == nullptr) {
        return failInitialization(
            QStringLiteral("EGL cannot report the consumer DRM render node"));
    }
    EGLAttrib deviceValue = 0;
    if (queryDisplayAttrib(eglDisplay, EGL_DEVICE_EXT, &deviceValue) != EGL_TRUE ||
        deviceValue == 0) {
        return failInitialization(
            QStringLiteral("EGL display has no associated consumer device"));
    }
    const EGLDeviceEXT device = std::bit_cast<EGLDeviceEXT>(deviceValue);
    const char* const renderNode = queryDeviceString(device, EGL_DRM_RENDER_NODE_FILE_EXT);
    QByteArray nodePath;
    if (renderNode != nullptr) {
        nodePath = QByteArray(renderNode);
    } else {
        /* MTT's EGL implementation exposes EGL_EXT_device_drm but omits the
         * render-node string. Resolve the render node through the same DRM
         * device's sysfs directory; this preserves the producer/consumer GPU
         * identity instead of guessing a renderD number. */
        const char* const drmDevice = queryDeviceString(device, EGL_DRM_DEVICE_FILE_EXT);
        if (drmDevice == nullptr) {
            return failInitialization(
                QStringLiteral("EGL consumer device exposes no DRM device path"));
        }
        struct stat drmDeviceStat {};
        if (::stat(drmDevice, &drmDeviceStat) != 0 || !S_ISCHR(drmDeviceStat.st_mode)) {
            return failInitialization(
                QStringLiteral("cannot identify EGL consumer DRM device"));
        }
        const QByteArray sysfsPath = QByteArrayLiteral("/sys/dev/char/") +
                                     QByteArray::number(major(drmDeviceStat.st_rdev)) +
                                     QByteArrayLiteral(":") +
                                     QByteArray::number(minor(drmDeviceStat.st_rdev));
        const std::filesystem::path drmDirectory =
            std::filesystem::path(sysfsPath.constData()) / "device" / "drm";
        std::error_code filesystemError;
        std::filesystem::directory_iterator iterator(drmDirectory, filesystemError);
        if (filesystemError) {
            return failInitialization(
                QStringLiteral("cannot inspect EGL consumer DRM device"));
        }
        const std::filesystem::directory_iterator end;
        while (iterator != end) {
            const std::string entryName = iterator->path().filename().string();
            if (entryName.rfind("renderD", 0U) == 0U) {
                const QByteArray candidate = QByteArrayLiteral("/dev/dri/") +
                                             QByteArray::fromStdString(entryName);
                struct stat candidateStat {};
                if (::stat(candidate.constData(), &candidateStat) == 0 &&
                    S_ISCHR(candidateStat.st_mode) &&
                    minor(candidateStat.st_rdev) >= 128U &&
                    minor(candidateStat.st_rdev) <= 255U) {
                    nodePath = candidate;
                    break;
                }
            }
            iterator.increment(filesystemError);
            if (filesystemError) {
                return failInitialization(
                    QStringLiteral("cannot inspect EGL consumer DRM device"));
            }
        }
        if (nodePath.isEmpty()) {
            return failInitialization(
                QStringLiteral("EGL consumer device has no DRM render node"));
        }
    }
    const int nodeFd = ::open(nodePath.constData(), O_RDONLY | O_CLOEXEC);
    if (nodeFd < 0) {
        return failInitialization(
            QStringLiteral("cannot open EGL consumer DRM render node"));
    }
    struct stat nodeStat {};
    const int statResult = fstat(nodeFd, &nodeStat);
    const int closeResult = ::close(nodeFd);
    if (statResult != 0 || closeResult != 0 || !S_ISCHR(nodeStat.st_mode) ||
        minor(nodeStat.st_rdev) < 128U || minor(nodeStat.st_rdev) > 255U) {
        return failInitialization(
            QStringLiteral("cannot identify EGL consumer DRM render node"));
    }
    m_drmRenderMajor = static_cast<uint32_t>(major(nodeStat.st_rdev));
    m_drmRenderMinor = static_cast<uint32_t>(minor(nodeStat.st_rdev));

    /* EGL import is not restricted to linear DMA-BUFs.  Advertising only
     * modifier zero forced a 4096x2304@60 video producer to render every frame
     * into uncached linear storage on Intel.  Enumerate the exact RGB layouts
     * that this EGL display can sample as ordinary GL_TEXTURE_2D images;
     * external-only modifiers are excluded because QSGOpenGLTexture cannot
     * represent GL_TEXTURE_EXTERNAL_OES. */
    const auto queryDmaBufModifiers =
        std::bit_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
            eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (queryDmaBufModifiers == nullptr) {
        return failInitialization(
            QStringLiteral("EGL cannot enumerate DMA-BUF modifiers"));
    }
    QVector<md_format_cap_t> eglFormats;
    const std::array<uint32_t, 4U> fourccs {
        DrmFormatXrgb8888,
        DrmFormatArgb8888,
        DrmFormatXbgr8888,
        DrmFormatAbgr8888,
    };
    for (const uint32_t fourccValue : fourccs) {
        EGLint modifierCount = 0;
        if (queryDmaBufModifiers(eglDisplay, static_cast<EGLint>(fourccValue), 0,
                                nullptr, nullptr, &modifierCount) != EGL_TRUE ||
            modifierCount < 0) {
            return failInitialization(
                QStringLiteral("EGL DMA-BUF modifier enumeration failed"));
        }
        if (modifierCount == 0) continue;
        QVector<EGLuint64KHR> modifiers(static_cast<qsizetype>(modifierCount));
        QVector<EGLBoolean> externalOnly(static_cast<qsizetype>(modifierCount));
        EGLint writtenCount = 0;
        if (queryDmaBufModifiers(eglDisplay, static_cast<EGLint>(fourccValue),
                                modifierCount, modifiers.data(), externalOnly.data(),
                                &writtenCount) != EGL_TRUE ||
            writtenCount < 0 || writtenCount > modifierCount) {
            return failInitialization(
                QStringLiteral("EGL DMA-BUF modifier enumeration failed"));
        }
        for (EGLint index = 0; index < writtenCount; ++index) {
            const qsizetype vectorIndex = static_cast<qsizetype>(index);
            if (externalOnly[vectorIndex] == EGL_FALSE) {
                eglFormats.append({fourccValue, 1U, modifiers[vectorIndex]});
            }
        }
    }
    if (eglFormats.isEmpty()) {
        return failInitialization(
            QStringLiteral("EGL exposes no texture-compatible RGB DMA-BUF modifiers"));
    }
    m_eglFormats = std::move(eglFormats);
    if (m_glDiagnostics) {
        qInfo() << "[KDE wallpaper] EGL DMA-BUF import candidates="
                << m_eglFormats.size();
        for (const md_format_cap_t& format : m_eglFormats) {
            qInfo() << "[KDE wallpaper] EGL candidate fourcc=" << format.fourcc
                    << "modifier=" << Qt::hex << format.modifier << Qt::dec;
        }
    }

    md_egl_context_t importerContext {
        .display = eglDisplay,
    };
    m_importer = md_egl_importer_new(&importerContext);
    if (m_importer == nullptr) {
        return failInitialization(QStringLiteral("EGL DMA-BUF import is unavailable"));
    }

    /* The entry point must be resolved through Qt's current context: on GLX it
     * belongs to the GLX dispatch table, whereas the EGL scene graph resolves
     * the same extension through its EGL-backed OpenGL context. */
    m_imageTargetTexture = std::bit_cast<GlEglImageTargetTexture2D>(
        context->getProcAddress("glEGLImageTargetTexture2DOES"));
    if (m_imageTargetTexture == nullptr) {
        md_egl_importer_free(m_importer);
        m_importer = nullptr;
        return failInitialization(
            QStringLiteral("glEGLImageTargetTexture2DOES is unavailable"));
    }
    if (ownsEglDisplay) m_glxEglDisplay = eglDisplay;
    setRendererBackend(BackendOpenGLEGL);
    setLastError({});
    return true;
}

#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN

/*
 * Renders a probe failure into an actionable, user-visible message. The
 * message names the exact remediation instead of a generic import error:
 * driver-side gaps (e.g. NVIDIA without nvidia-drm modeset) versus a scene
 * graph that simply never enabled the extensions (plasmashell: the Qt Quick
 * device is created before any plugin can register device extensions, and the
 * supported workaround is the QT_VULKAN_DEVICE_EXTENSIONS environment
 * variable).
 */
static QString describeDmaBufImportUnavailable(const md_vk_dma_buf_import_state_t state,
                                               const char* const missingExtensions) {
    switch (state) {
    case MD_VK_DMA_BUF_IMPORT_DRIVER_UNSUPPORTED:
        return QStringLiteral(
                   "Vulkan DMA-BUF import unavailable: the driver does not expose %1. "
                   "For NVIDIA GPUs enable nvidia-drm modeset=1 (kernel parameter or "
                   "/etc/modprobe.d option, then update-initramfs and reboot); ensure "
                   "your user can access /dev/dri/renderD* (the 'render' group); or "
                   "use the OpenGL render backend (EGL DMA-BUF import).")
            .arg(QString::fromUtf8(missingExtensions != nullptr && missingExtensions[0] != '\0'
                                       ? missingExtensions
                                       : "the required extensions"));
    case MD_VK_DMA_BUF_IMPORT_DEVICE_NOT_ENABLED:
        return QStringLiteral(
                   "Vulkan DMA-BUF import unavailable: the scene-graph device did not "
                   "enable VK_EXT_external_memory_dma_buf (device extensions must be "
                   "requested before the scene graph initializes). Start plasmashell with "
                   "QT_VULKAN_DEVICE_EXTENSIONS=\"VK_KHR_external_memory;"
                   "VK_KHR_external_memory_fd;VK_EXT_external_memory_dma_buf;"
                   "VK_EXT_queue_family_foreign;VK_EXT_image_drm_format_modifier;"
                   "VK_KHR_external_semaphore;VK_KHR_external_semaphore_fd;"
                   "VK_KHR_sampler_ycbcr_conversion;VK_KHR_bind_memory2;"
                   "VK_KHR_get_memory_requirements2\", or use the OpenGL render "
                   "backend (EGL DMA-BUF import).");
    case MD_VK_DMA_BUF_IMPORT_UNAVAILABLE:
        return QStringLiteral(
                   "Vulkan DMA-BUF import unavailable: the required entry points or "
                   "extension enumeration are not available on this device.");
    case MD_VK_DMA_BUF_IMPORT_OK:
    default:
        return QStringLiteral("Vulkan DMA-BUF import unavailable");
    }
}

/*
 * Creates the Vulkan importer and blitter from Qt Quick's device resources,
 * reads the device/driver UUIDs and DRM render node from the physical device,
 * and enumerates importable RGB modifiers to advertise as consumer caps.
 */
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
    // Fail fast with an actionable message before any pool arrives: if the
    // driver lacks the external-memory extensions (NVIDIA without modeset) or
    // the Qt Quick device never enabled them (plasmashell scene graph), every
    // import would otherwise fail only after the first frame with a black
    // screen.
    md_vk_dma_buf_import_state_t importState = MD_VK_DMA_BUF_IMPORT_UNAVAILABLE;
    char missingExtensions[256] = {};
    if (md_vk_query_dma_buf_import_support(*physicalPointer, *devicePointer, &importState,
                                           missingExtensions,
                                           sizeof(missingExtensions)) != MD_OK) {
        setLastError(QStringLiteral("Vulkan DMA-BUF import support probe failed"));
        return false;
    }
    if (importState != MD_VK_DMA_BUF_IMPORT_OK) {
        setLastError(describeDmaBufImportUnavailable(importState, missingExtensions));
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
    /* QSGVulkanTexture::fromNative() accepts an image and layout but no VkFormat.
     * Qt's native-texture contract therefore requires the protocol image to use
     * RGBA component order. XBGR/ABGR map to VK_FORMAT_R8G8B8A8_UNORM, whereas
     * XRGB/ARGB map to B8G8R8A8 and would make red and blue appear exchanged.
     * Advertising only the two representable wire formats lets the broker make
     * an exact format choice without a shader swizzle or CPU conversion pass. */
    const uint32_t fourccs[] = {
        DrmFormatXbgr8888,
        DrmFormatAbgr8888,
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
    m_eglFormats.clear();
    if (m_glxEglDisplay != EGL_NO_DISPLAY) {
        if (eglTerminate(m_glxEglDisplay) != EGL_TRUE) {
            qWarning() << "[KDE wallpaper] Cannot terminate GLX EGL display";
        }
        m_glxEglDisplay = EGL_NO_DISPLAY;
    }
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
    /* Renderer readiness is restored by the next sceneGraphInitialized signal;
     * the retry timer covers the case where that signal does not come again. */
    QMetaObject::invokeMethod(this, [this]() {
        closeConnection();
        ++m_connectionGeneration;
        m_rendererRetryAttempts = 0;
        scheduleRendererRetry();
    }, Qt::QueuedConnection);
}

void MirageDisplayItem::setSocketPath(const QString& value) {
    if (m_socketPath == value) return;
    m_socketPath = value;
    emit socketPathChanged();
    closeConnection();
    ++m_connectionGeneration;
    m_socketDevice = 0;
    m_socketInode = 0;
    emit connectionDiagnosticsChanged();
    scheduleReconnect();
}

/* Re-attempts immediately when the runtime broker directory changes; the
 * reconnect timer remains the fallback when inotify cannot observe it. */
void MirageDisplayItem::brokerDirectoryChanged(const QString& path) {
    if (m_socketPath.isEmpty()) return;
    const QString brokerDirectory = QFileInfo(m_socketPath).absolutePath();
    if (path == brokerDirectory || path == QFileInfo(brokerDirectory).absolutePath()) {
        if (QFileInfo::exists(brokerDirectory) &&
            !m_brokerWatcher.directories().contains(brokerDirectory)) {
            const bool watching = m_brokerWatcher.addPath(brokerDirectory);
            if (!watching) scheduleReconnect();
        }
        startConnection();
    }
}

/*
 * Stores the requested backend for the next scene-graph initialization.  Qt
 * Quick creates the graphics device before this item can safely replace it,
 * so changing the preference does not mutate a live renderer; Plasma must
 * recreate the wallpaper or plasmashell for the new value to take effect.
 */
void MirageDisplayItem::setRendererBackendPreference(const RendererBackendPreference value) {
    if (m_rendererBackendPreference == value) return;
    m_rendererBackendPreference = value;
    emit rendererBackendPreferenceChanged();
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

void MirageDisplayItem::setLogicalX(int value) {
    if (m_logicalX == value) return;
    m_logicalX = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setLogicalY(int value) {
    if (m_logicalY == value) return;
    m_logicalY = value;
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


/*
 * Enables or disables pointer forwarding; disabling releases the current
 * pointer state (enter/leave) so the renderer does not keep a stale cursor.
 */
void MirageDisplayItem::setPointerForwarding(bool value) {
    if (m_pointerForwarding == value) return;
    if (!value) releasePointerState(monotonicTimestampUs());
    m_pointerForwarding = value;
    emit pointerForwardingChanged();
    m_outputUpdateTimer.start();
}


/*
 * Forwards the DE-computed window-state flags to the broker immediately when
 * the session is READY; the value is also cached so it can be replayed after a
 * reconnect.
 */
void MirageDisplayItem::setWindowStateFlags(quint32 value) {
    if (m_windowStateFlags == value) return;
    m_windowStateFlags = value;
    emit windowStateFlagsChanged();
    if (m_display != nullptr && md_display_connection_state(m_display) == MD_CONNECTION_READY) {
        const md_result_t result = md_display_send_window_state(
            m_display, static_cast<uint32_t>(value));
        if (result == MD_OK || result == MD_ERR_WOULD_BLOCK) {
            armWritable();
        } else {
            handleConnectionFailure();
        }
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


/*
 * Derives the stable output identity from QScreen/Qt properties: the
 * stable_id is the trimmed configured value (falling back to kde:unknown), the
 * refresh rate prefers the QScreen value, and input_caps reflects whether pointer
 * forwarding is enabled.  The returned struct borrows the byte arrays.
 */
md_output_info_t MirageDisplayItem::makeOutputInfo(QByteArray& stableId, QByteArray& name) const {
    stableId = m_outputStableId.trimmed().toUtf8();
    name = m_outputName.trimmed().toUtf8();
    if (stableId.isEmpty()) stableId = QByteArrayLiteral("kde:unknown");
    if (name.isEmpty()) name = QByteArrayLiteral("KDE wallpaper");

    uint32_t refreshMhz = positiveU32(m_refreshMhz, 60000);
    int32_t logicalX = static_cast<int32_t>(m_logicalX);
    int32_t logicalY = static_cast<int32_t>(m_logicalY);
    if (window() != nullptr && window()->screen() != nullptr &&
        window()->screen()->refreshRate() > 0.0) {
        const qreal screenRefresh = window()->screen()->refreshRate() * 1000.0;
        if (screenRefresh > 0.0 && screenRefresh < static_cast<qreal>(std::numeric_limits<uint32_t>::max())) {
        refreshMhz = static_cast<uint32_t>(screenRefresh);
        }
        /* QScreen::geometry() is expressed in the virtual desktop coordinate
         * space; its origin therefore carries the negative/offset monitor
         * position required by mirage-display v1.2. */
        logicalX = static_cast<int32_t>(window()->screen()->geometry().x());
        logicalY = static_cast<int32_t>(window()->screen()->geometry().y());
    }

    return md_output_info_t {
        .stable_id = stableId.constData(),
        .name = name.constData(),
        .physical_width = positiveU32(m_physicalWidth, 1),
        .physical_height = positiveU32(m_physicalHeight, 1),
        .logical_width = positiveU32(m_logicalWidth, 1),
        .logical_height = positiveU32(m_logicalHeight, 1),
        .logical_x = logicalX,
        .logical_y = logicalY,
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


/*
 * Creates the display session, advertises the backend-specific formats and
 * feature bits, and starts a nonblocking connection with QSocketNotifier-driven
 * handshake.  Any failure closes the session and schedules a reconnect.
 */
void MirageDisplayItem::startConnection() {
    if (!isComponentComplete() || !m_rendererReady.load()) {
        scheduleReconnect();
        return;
    }
    if (m_socketPath.isEmpty()) {
        const QString runtimeDirectory = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (!runtimeDirectory.isEmpty()) {
            const QString nextDefaultPath = runtimeDirectory +
                                            QStringLiteral("/mirage-wallpaper/display-v1.sock");
            if (m_defaultSocketPath != nextDefaultPath) {
                m_defaultSocketPath = nextDefaultPath;
                emit defaultSocketPathChanged();
            }
            m_socketPath = m_defaultSocketPath;
            emit socketPathChanged();
        }
    }
    if (m_socketPath.isEmpty()) {
        scheduleReconnect();
        return;
    }

    /* QFileSystemWatcher and the timer both enter here.  If the broker replaced
     * the pathname, invalidate the old session before the m_display guard; if
     * the endpoint is unchanged, keep READY sessions and let the handshake
     * timeout own in-progress sessions.  Re-arming the single-shot timer here
     * prevents either guard from silently terminating the reconnect loop. */
    if (refreshSocketIdentity() && m_display != nullptr) {
        closeConnection();
        ++m_connectionGeneration;
    }
    if (m_display != nullptr) {
        if (md_display_connection_state(m_display) != MD_CONNECTION_READY) {
            scheduleReconnect();
        }
        return;
    }

    const QString brokerDirectory = QFileInfo(m_socketPath).absolutePath();
    if (QFileInfo::exists(brokerDirectory) &&
        !m_brokerWatcher.directories().contains(brokerDirectory)) {
        const bool watching = m_brokerWatcher.addPath(brokerDirectory);
        if (!watching) scheduleReconnect();
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
    ++m_connectionGeneration;
    ++m_reconnectAttempts;
    emit connectionDiagnosticsChanged();

    const md_format_cap_t* formats = m_eglFormats.constData();
    uint32_t formatCount = static_cast<uint32_t>(m_eglFormats.size());
    uint64_t featureBits = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_POINTER_AXIS |
                           MD_FEATURE_WINDOW_STATE | MD_FEATURE_TARGET_GPU_BINDING |
                           MD_FEATURE_DRM_MODIFIERS;
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    if (m_rendererBackend.load() == BackendVulkan && !m_vkFormats.isEmpty()) {
        formats = m_vkFormats.constData();
        formatCount = static_cast<uint32_t>(m_vkFormats.size());
    }
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

    errno = 0;
    int result = md_display_begin_connect(m_display, socketBytes.constData(),
                                          "mirage-plasma", "0.2.0",
                                          &output, &capabilities);
    const int connectErrno = errno;
    if (result != MD_OK) {
        QString error = QStringLiteral(
            "Cannot connect to Mirage display broker (result=%1, socket=%2)")
                            .arg(result)
                            .arg(m_socketPath);
        if (connectErrno != 0) {
            error += QStringLiteral(" (errno=%1: %2)")
                         .arg(connectErrno)
                         .arg(QString::fromLocal8Bit(strerror(connectErrno)));
        }
        setLastError(error);
        /* Some begin_connect() failures occur before the transport can notify
         * onDisconnected(), so tear down here. Bumping the generation also
         * invalidates the teardown that a synchronous onDisconnected() already
         * queued for this very session, so it cannot close a later one. */
        closeConnection();
        ++m_connectionGeneration;
        scheduleReconnect();
        return;
    }

    int fd = md_display_get_fd(m_display);
    if (fd < 0) {
        setLastError(QStringLiteral("Display broker connection has no socket"));
        closeConnection();
        ++m_connectionGeneration;
        scheduleReconnect();
        return;
    }
    m_readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    m_writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    connect(m_readNotifier, &QSocketNotifier::activated,
            this, &MirageDisplayItem::advanceHandshake);
    connect(m_writeNotifier, &QSocketNotifier::activated,
            this, &MirageDisplayItem::advanceHandshake);
    m_handshakeTimeoutTimer.start();
    advanceHandshake();
}


/*
 * Drives the nonblocking handshake from socket-notifier events; once READY,
 * re-wires the notifiers to dispatch/flush and replays the cached window state.
 */
void MirageDisplayItem::advanceHandshake() {
    if (m_display == nullptr) return;
    for (int iteration = 0; iteration < 16; ++iteration) {
        int result = md_display_advance_handshake(m_display);
        if (result == MD_HANDSHAKE_PROGRESS) continue;
        if (result == MD_HANDSHAKE_DONE) {
            m_handshakeTimeoutTimer.stop();
            disconnect(m_readNotifier, nullptr, this, nullptr);
            disconnect(m_writeNotifier, nullptr, this, nullptr);
            connect(m_readNotifier, &QSocketNotifier::activated,
                    this, &MirageDisplayItem::dispatchSocket);
            connect(m_writeNotifier, &QSocketNotifier::activated,
                    this, &MirageDisplayItem::flushSocket);
            m_readNotifier->setEnabled(true);
            m_writeNotifier->setEnabled(false);
            const md_result_t stateResult = md_display_send_window_state(
                m_display, static_cast<uint32_t>(m_windowStateFlags));
            if (stateResult != MD_OK && stateResult != MD_ERR_WOULD_BLOCK) {
                handleConnectionFailure();
                return;
            }
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


/*
 * Main-thread packet dispatch: drains readable packets, then re-arms the
 * write notifier if the outbox has pending messages.
 */
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
    const md_result_t result = md_display_update_output(m_display, &output);
    if (result != MD_OK && result != MD_ERR_WOULD_BLOCK) {
        handleConnectionFailure();
        return;
    }
    armWritable();
}


/*
 * Completes a deferred UNBIND on the protocol event thread after the render
 * thread has destroyed GPU references (see onBuffersReleasing / updatePaintNode).
 */
void MirageDisplayItem::finishDeferredUnbind(qulonglong generation) {
    if (m_display == nullptr || generation == 0) return;
    if (md_display_finish_unbind(m_display, static_cast<uint64_t>(generation)) == MD_OK) {
        armWritable();
    }
}


/*
 * Tears down the session: releases pointer state, deletes socket notifiers,
 * frees the display, and resets the connected/output Q_PROPERTY state.
 */
void MirageDisplayItem::closeConnection() {
    m_handshakeTimeoutTimer.stop();
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

/*
 * Records the pathname socket identity and reports whether it replaced the
 * endpoint this item previously observed.  The broker unlinks and re-binds the
 * pathname when it starts, so st_dev + st_ino are the only stable evidence that
 * an existing session belongs to an obsolete endpoint.  A missing path is not
 * a replacement: the normal reconnect timer waits for the broker to create it.
 */
bool MirageDisplayItem::refreshSocketIdentity() {
    if (m_socketPath.isEmpty() || m_socketPath.startsWith(QLatin1Char('@'))) return false;
    const QByteArray socketBytes = m_socketPath.toUtf8();
    struct stat socketStat {};
    if (::stat(socketBytes.constData(), &socketStat) != 0) return false;
    const qulonglong device = static_cast<qulonglong>(socketStat.st_dev);
    const qulonglong inode = static_cast<qulonglong>(socketStat.st_ino);
    const bool replaced = m_socketInode != 0 &&
                          (m_socketDevice != device || m_socketInode != inode);
    if (m_socketDevice != device || m_socketInode != inode) {
        m_socketDevice = device;
        m_socketInode = inode;
        emit connectionDiagnosticsChanged();
    }
    return replaced;
}

void MirageDisplayItem::handleConnectionFailure() {
    closeConnection();
    ++m_connectionGeneration;
    scheduleReconnect();
}


/*
 * Drops a session whose handshake never reached READY.  Without this the
 * non-null m_display would block startConnection() forever, so the wallpaper
 * would stay disconnected even after the broker becomes healthy.
 */
void MirageDisplayItem::abortStalledHandshake() {
    if (m_display == nullptr) return;
    if (md_display_connection_state(m_display) == MD_CONNECTION_READY) return;
    setLastError(QStringLiteral(
        "Mirage display broker did not finish the handshake; retrying."));
    closeConnection();
    ++m_connectionGeneration;
    scheduleReconnect();
}

void MirageDisplayItem::scheduleReconnect() {
    if (isComponentComplete() && m_rendererReady.load() && !m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}


/*
 * Protocol callbacks below run on the Qt main thread; they copy callback
 * payloads into members guarded by m_stateMutex and call update() so the render
 * thread picks them up on the next scene-graph pass.  Frame/release descriptors
 * are owned by the callback path and consumed exactly once.
 */
void MirageDisplayItem::onConnected(void* userData, uint64_t outputIdValue) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    self->m_reconnectTimer.stop();
    self->m_reconnectAttempts = 0;
    if (self->refreshSocketIdentity()) {
        QPointer<MirageDisplayItem> guard(self);
        const quint64 generation = self->m_connectionGeneration;
        QMetaObject::invokeMethod(self, [guard, generation]() {
            if (guard == nullptr || guard->m_connectionGeneration != generation) return;
            guard->closeConnection();
            ++guard->m_connectionGeneration;
            guard->scheduleReconnect();
        }, Qt::QueuedConnection);
        return;
    }
    emit self->connectionDiagnosticsChanged();
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


/*
 * Broker requested UNBIND.  Defers the unbind so the render thread can destroy
 * its GPU references, records the generation, and requests a repaint; the render
 * thread later calls finishDeferredUnbind on the event thread.
 */
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


/*
 * Releases a frame that will not be sampled: closes the acquire sync_file and
 * signals the release syncobj so the producer's slot is never blocked.
 */
void MirageDisplayItem::dropFrame(PendingFrame& frame) {
    if (!frame.valid) return;
    if (frame.value.acquire_sync_fd >= 0) close(frame.value.acquire_sync_fd);
    if (frame.value.release_syncobj_fd >= 0) {
        const md_result_t result = md_display_signal_release_syncobj(
            frame.value.release_syncobj_fd);
        if (result != MD_OK) {
            qWarning() << "[KDE wallpaper] Failed to signal dropped frame release syncobj:" << result;
        }
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
    if (self->m_outputId != 0) {
        self->m_outputId = 0;
        emit self->outputIdChanged();
    }
    /* A broker can vanish at any time; the library may fail the session from
     * a send path where no QSocketNotifier event follows. Tear down the dead
     * session and resume the reconnect loop so the wallpaper recovers as soon
     * as the broker comes back.
     *
     * The generation guard matters for the synchronous failure inside
     * md_display_begin_connect(): this callback runs before startConnection()
     * returns, so by the time the queued lambda executes, a later attempt may
     * already own a fresh session. Closing that one would drop the wallpaper
     * out of the reconnect loop for good. */
    QPointer<MirageDisplayItem> guard(self);
    const quint64 generation = self->m_connectionGeneration;
    QMetaObject::invokeMethod(self, [guard, generation]() {
        if (guard == nullptr || guard->m_connectionGeneration != generation) return;
        guard->closeConnection();
        ++guard->m_connectionGeneration;
        guard->scheduleReconnect();
    }, Qt::QueuedConnection);
}


/*
 * Imports the current pool on the render thread: Vulkan imports through the
 * importer, EGL creates one GL texture per EGLImage and wraps them as QSGTexture.
 * Runs with the OpenGL context current.
 */
bool MirageDisplayItem::importPendingPool(const md_buffer_pool_t& pool) {
#ifdef MIRAGE_DISPLAY_QML_WITH_VULKAN
    if (m_rendererBackend.load() == BackendVulkan) {
        if (m_vkImporter == nullptr) {
            setLastError(QStringLiteral("Vulkan DMA-BUF pool import failed: importer unavailable"));
            return false;
        }
        if (md_vk_importer_import_pool(m_vkImporter, &pool) != MD_OK) {
            setLastError(QStringLiteral("Vulkan DMA-BUF pool import failed: %1")
                             .arg(describeVkImportFailure(md_vk_importer_last_error(m_vkImporter))));
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


/*
 * Releases the current pool's GPU resources on the render thread: finishes
 * outstanding GL work, signals the release syncobj for the sampled frame, deletes
 * textures and importer images, and resets the imported generation.
 */
void MirageDisplayItem::releaseRenderPool() {
    if (m_activeReleaseFd >= 0) {
        const uint64_t releaseStart = m_glDiagnostics ? monotonicTimestampUs() : 0U;
        QOpenGLContext* context = QOpenGLContext::currentContext();
        md_result_t result = MD_ERR_STATE;
        if (m_glxEglDisplay != EGL_NO_DISPLAY) {
            /* GLX commands are not ordered by an EGL native fence.  Keep the
             * blocking finish on this backend before signalling the producer. */
            if (context != nullptr) context->functions()->glFinish();
            result = md_display_signal_release_syncobj(m_activeReleaseFd);
        } else if (m_importer != nullptr && context != nullptr) {
            /* Wayland EGL can publish a GPU-side native fence.  Duplicate the
             * release descriptor so a failed EGL/DRM bridge still has an
             * explicit CPU signal path and cannot strand the producer slot. */
            const int fallbackFd = fcntl(m_activeReleaseFd, F_DUPFD_CLOEXEC, 0);
            if (fallbackFd < 0) {
                const int duplicateErrno = errno;
                /* The original descriptor is still ours when duplication
                 * fails.  Finish before the CPU signal so producer reuse
                 * cannot race outstanding scene-graph reads. */
                context->functions()->glFinish();
                result = md_display_signal_release_syncobj(m_activeReleaseFd);
                qWarning() << "[KDE wallpaper] Failed to duplicate EGL release FD;"
                           << "using synchronous signal, errno=" << duplicateErrno;
            } else {
                result = md_egl_release_after_current_context(m_importer, m_activeReleaseFd);
                if (result != MD_OK) {
                    context->functions()->glFinish();
                    const md_result_t fallbackResult = md_display_signal_release_syncobj(fallbackFd);
                    if (fallbackResult == MD_OK) result = MD_OK;
                } else if (close(fallbackFd) != 0) {
                    qWarning() << "[KDE wallpaper] Failed to close EGL release fallback FD";
                }
            }
        } else {
            result = md_display_signal_release_syncobj(m_activeReleaseFd);
        }
        if (result != MD_OK) {
            qWarning() << "[KDE wallpaper] Failed to signal active release syncobj:" << result;
        }
        if (m_glDiagnostics && releaseStart != 0U) {
            m_glReleaseUs += monotonicTimestampUs() - releaseStart;
        }
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


/*
 * After each frame is presented, attaches a fence from the current GL context
 * to the release syncobj so the producer can recycle the sampled buffer.
 */
void MirageDisplayItem::releaseAfterRendering() {
    if (m_activeReleaseFd < 0) return;
    int releaseFd = m_activeReleaseFd;
    m_activeReleaseFd = -1;
    const uint64_t releaseStart = m_glDiagnostics ? monotonicTimestampUs() : 0U;
    if (m_glxEglDisplay != EGL_NO_DISPLAY) {
        /* A native EGL fence is not connected to a GLX command stream. The
         * explicit finish is therefore required before the protocol release
         * syncobj is signalled on this backend. */
        QOpenGLContext* context = QOpenGLContext::currentContext();
        if (context != nullptr) context->functions()->glFinish();
        if (md_display_signal_release_syncobj(releaseFd) != MD_OK) {
            qWarning() << "[KDE wallpaper] Failed to signal GLX release syncobj";
        }
        if (m_glDiagnostics && releaseStart != 0U) {
            m_glReleaseUs += monotonicTimestampUs() - releaseStart;
        }
        return;
    }
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (m_importer == nullptr || context == nullptr) {
        const md_result_t result = md_display_signal_release_syncobj(releaseFd);
        if (result != MD_OK) {
            qWarning() << "[KDE wallpaper] Failed to signal EGL release syncobj:" << result;
        }
    } else {
        /* EGL native fences are non-blocking on the host.  Keep a duplicate so
         * an import/DRM bridge error still releases the producer-owned slot. */
        const int fallbackFd = fcntl(releaseFd, F_DUPFD_CLOEXEC, 0);
        md_result_t result = MD_ERR_IO;
        if (fallbackFd < 0) {
            const int duplicateErrno = errno;
            /* The original FD remains available when duplication fails.  A
             * synchronous finish makes the direct CPU signal safe. */
            context->functions()->glFinish();
            result = md_display_signal_release_syncobj(releaseFd);
            qWarning() << "[KDE wallpaper] Failed to duplicate EGL release FD;"
                       << "using synchronous signal, errno=" << duplicateErrno;
        } else {
            result = md_egl_release_after_current_context(m_importer, releaseFd);
            if (result != MD_OK) {
                context->functions()->glFinish();
                const md_result_t fallbackResult = md_display_signal_release_syncobj(fallbackFd);
                if (fallbackResult != MD_OK) {
                    qWarning() << "[KDE wallpaper] Failed to signal EGL release fallback syncobj"
                               << fallbackResult;
                }
            } else if (close(fallbackFd) != 0) {
                qWarning() << "[KDE wallpaper] Failed to close EGL release fallback FD";
            }
        }
        if (result != MD_OK) {
            qWarning() << "[KDE wallpaper] EGL release fence failed:" << result;
        }
    }
    if (m_glDiagnostics && releaseStart != 0U) {
        m_glReleaseUs += monotonicTimestampUs() - releaseStart;
    }
}


/*
 * Builds the scene-graph node on the render thread: first completes any pending
 * pool release (and deferred unbind), then imports a pending pool, samples the
 * latest frame with backend-specific sync handling, and applies the configured
 * transform.
 */
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
    if (importPool) {
        const uint64_t importStart = m_glDiagnostics ? monotonicTimestampUs() : 0U;
        const bool imported = importPendingPool(pendingPool);
        if (m_glDiagnostics && importStart != 0U) {
            const uint64_t importEnd = monotonicTimestampUs();
            if (importEnd >= importStart) m_glPoolImportUs += importEnd - importStart;
        }
        if (!imported) {
            delete oldNode;
            oldNode = nullptr;
        }
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
                        const md_result_t signalResult = md_display_signal_release_syncobj(
                            frame.value.release_syncobj_fd);
                        if (signalResult != MD_OK) {
                            qWarning() << "[KDE wallpaper] Failed to signal Vulkan acquire-failure release syncobj:"
                                       << signalResult;
                        }
                        frame.value.release_syncobj_fd = -1;
                    }
                    setLastError(QStringLiteral("Vulkan acquire sync import failed: %1")
                                     .arg(describeVkImportFailure(
                                         md_vk_importer_last_error(m_vkImporter))));
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
                            const md_result_t signalResult = md_display_signal_release_syncobj(releaseFd);
                            if (signalResult != MD_OK) {
                                setLastError(QStringLiteral("Vulkan release sync signal failed (%1)")
                                                 .arg(static_cast<int>(signalResult)));
                            }
                        }
                        m_currentBuffer = 0;
                        setLastError({});
                    } else {
                        if (releaseFd >= 0) {
                            const md_result_t signalResult = md_display_signal_release_syncobj(releaseFd);
                            if (signalResult != MD_OK) {
                                setLastError(QStringLiteral("Vulkan release sync signal failed (%1)")
                                                 .arg(static_cast<int>(signalResult)));
                            }
                        }
                        setLastError(QStringLiteral("Vulkan frame relay failed (rc=%1)")
                                         .arg(static_cast<int>(rc)));
                    }
                }
            }
        } else
#endif
        {
            bool valid = frame.value.buffer_generation == m_importedGeneration.load() &&
                         frame.value.buffer_index < static_cast<uint32_t>(m_qsgTextures.size());
            int waitResult = MD_ERR_INVALID;
            const uint64_t waitStart = m_glDiagnostics ? monotonicTimestampUs() : 0U;
            if (frame.value.acquire_sync_fd >= 0) {
                if (!valid) {
                    if (::close(frame.value.acquire_sync_fd) != 0) {
                        qWarning() << "[KDE wallpaper] Failed to close dropped EGL acquire sync FD";
                    }
                } else if (m_glxEglDisplay != EGL_NO_DISPLAY) {
                    /* EGL syncs cannot order commands issued by a GLX
                     * context. Wait on the sync_file before sampling, then
                     * use glFinish for the release boundary. */
                    struct pollfd descriptor {
                        .fd = frame.value.acquire_sync_fd,
                        .events = POLLIN,
                        .revents = 0,
                    };
                    int pollResult = 0;
                    do {
                        pollResult = ::poll(&descriptor, 1U, -1);
                    } while (pollResult < 0 && errno == EINTR);
                    if (pollResult == 1 &&
                        (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0) {
                        waitResult = MD_OK;
                    } else {
                        waitResult = MD_ERR_IO;
                    }
                    if (::close(frame.value.acquire_sync_fd) != 0) {
                        qWarning() << "[KDE wallpaper] Failed to close GLX acquire sync FD";
                    }
                } else if (m_importer != nullptr) {
                    waitResult = md_egl_wait_acquire_sync(
                        m_importer, frame.value.acquire_sync_fd);
                }
            }
            if (m_glDiagnostics && waitStart != 0U) {
                const uint64_t waitEnd = monotonicTimestampUs();
                if (waitEnd >= waitStart) m_glAcquireWaitUs += waitEnd - waitStart;
            }
            if (!valid || waitResult != MD_OK) {
                if (m_glDiagnostics) ++m_glDroppedFrames;
                frame.value.acquire_sync_fd = -1;
                if (frame.value.release_syncobj_fd >= 0) {
                    const md_result_t signalResult = md_display_signal_release_syncobj(
                        frame.value.release_syncobj_fd);
                    if (signalResult != MD_OK) {
                        setLastError(QStringLiteral("EGL release sync signal failed (%1)")
                                         .arg(static_cast<int>(signalResult)));
                    }
                }
            } else {
                frame.value.acquire_sync_fd = -1;
                if (m_activeReleaseFd >= 0) {
                    const uint64_t releaseStart = m_glDiagnostics ? monotonicTimestampUs() : 0U;
                    QOpenGLContext* context = QOpenGLContext::currentContext();
                    md_result_t releaseResult = MD_ERR_STATE;
                    if (m_glxEglDisplay != EGL_NO_DISPLAY) {
                        /* GLX has no ordering edge with the EGL acquire
                         * object, so finish before returning the old slot. */
                        if (context != nullptr) context->functions()->glFinish();
                        releaseResult = md_display_signal_release_syncobj(m_activeReleaseFd);
                    } else if (m_importer != nullptr && context != nullptr) {
                        const int fallbackFd = fcntl(m_activeReleaseFd, F_DUPFD_CLOEXEC, 0);
                        if (fallbackFd < 0) {
                            const int duplicateErrno = errno;
                            /* Keep the old descriptor for a safe synchronous
                             * signal when duplication cannot be acquired. */
                            context->functions()->glFinish();
                            releaseResult = md_display_signal_release_syncobj(m_activeReleaseFd);
                            qWarning() << "[KDE wallpaper] Failed to duplicate EGL release FD;"
                                       << "using synchronous signal, errno=" << duplicateErrno;
                        } else {
                            releaseResult = md_egl_release_after_current_context(
                                m_importer, m_activeReleaseFd);
                            if (releaseResult != MD_OK) {
                                context->functions()->glFinish();
                                const md_result_t fallbackResult =
                                    md_display_signal_release_syncobj(fallbackFd);
                                if (fallbackResult == MD_OK) releaseResult = MD_OK;
                            } else if (close(fallbackFd) != 0) {
                                qWarning() << "[KDE wallpaper] Failed to close EGL release fallback FD";
                            }
                        }
                    } else {
                        releaseResult = md_display_signal_release_syncobj(m_activeReleaseFd);
                    }
                    if (releaseResult != MD_OK) {
                        setLastError(QStringLiteral("EGL release sync signal failed (%1)")
                                         .arg(static_cast<int>(releaseResult)));
                    }
                    if (m_glDiagnostics && releaseStart != 0U) {
                        const uint64_t releaseEnd = monotonicTimestampUs();
                        if (releaseEnd >= releaseStart) m_glReleaseUs += releaseEnd - releaseStart;
                    }
                }
                m_activeReleaseFd = frame.value.release_syncobj_fd;
                frame.value.release_syncobj_fd = -1;
                m_currentBuffer = static_cast<int>(frame.value.buffer_index);
                if (m_glDiagnostics) {
                    ++m_glDiagnosticFrames;
                    if ((m_glDiagnosticFrames % 120U) == 0U) {
                        const char* const backend = m_glxEglDisplay != EGL_NO_DISPLAY
                            ? "glx" : "egl";
                        qInfo() << "[KDE wallpaper] OpenGL diagnostics backend=" << backend
                                << "frames=" << m_glDiagnosticFrames
                                << "acquire_wait_us=" << m_glAcquireWaitUs
                                << "pool_import_us=" << m_glPoolImportUs
                                << "release_us=" << m_glReleaseUs
                                << "dropped=" << m_glDroppedFrames;
                        m_glAcquireWaitUs = 0;
                        m_glPoolImportUs = 0;
                        m_glReleaseUs = 0;
                        m_glDroppedFrames = 0;
                    }
                }
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


/*
 * Monotonic microsecond timestamp used by all pointer messages, matching the
 * protocol's clock requirement.
 */
uint64_t MirageDisplayItem::monotonicTimestampUs() {
    struct timespec value {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<uint64_t>(value.tv_sec) * UINT64_C(1000000) +
           static_cast<uint64_t>(value.tv_nsec) / UINT64_C(1000);
}

void MirageDisplayItem::releasePointerState(uint64_t timestamp) {
    m_pointer.reset(timestamp);
}


/*
 * Maps a normalized pointer-forwarder event to the matching md_display_send_*
 * call when the session is READY, then re-arms the write notifier.
 */
void MirageDisplayItem::forwardPointerEvent(const MiragePointerForwarder::Event& event) {
    if (m_display == nullptr || md_display_connection_state(m_display) != MD_CONNECTION_READY) {
        return;
    }
    md_result_t result = MD_OK;
    switch (event.type) {
    case MiragePointerForwarder::Event::Type::Enter:
        result = md_display_send_pointer_enter(m_display, event.x, event.y, event.timestamp);
        break;
    case MiragePointerForwarder::Event::Type::Leave:
        result = md_display_send_pointer_leave(m_display, event.timestamp);
        break;
    case MiragePointerForwarder::Event::Type::Motion:
        result = md_display_send_pointer_motion(m_display, event.x, event.y,
                                                event.timestamp, event.modifiers);
        break;
    case MiragePointerForwarder::Event::Type::Button:
        result = md_display_send_pointer_button(m_display, event.x, event.y, event.button,
                                                event.buttonState, event.timestamp,
                                                event.modifiers);
        break;
    case MiragePointerForwarder::Event::Type::Axis:
        result = md_display_send_pointer_axis(m_display, event.x, event.y, event.deltaX,
                                              event.deltaY, event.axisSource, event.timestamp,
                                              event.modifiers);
        break;
    }
    if (result != MD_OK && result != MD_ERR_WOULD_BLOCK) {
        handleConnectionFailure();
        return;
    }
    armWritable();
}


/*
 * Observes pointer events on the window and feeds them to the forwarder.
 * Always returns false so Plasma keeps desktop clicks, context menus,
 * drag-and-drop, and wheel events; drag is reconstructed from motion plus button
 * state.
 */
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

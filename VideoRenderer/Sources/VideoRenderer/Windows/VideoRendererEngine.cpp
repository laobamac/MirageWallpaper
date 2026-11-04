#include "VideoRendererEngine.h"
#include "VideoManifest.h"

#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QSet>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSaveFile>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

struct VRVideoRendererEngine::MemoryVideo {
    QByteArray bytes;
    struct Cursor { const QByteArray* bytes; qint64 position = 0; };
    static int open(void* context, char*, mpv_stream_cb_info* info) {
        auto* video = static_cast<MemoryVideo*>(context);
        *info = {};
        info->cookie = new Cursor{&video->bytes};
        info->read_fn = [](void* cookie, char* buffer, uint64_t count) -> int64_t {
            auto& cursor = *static_cast<Cursor*>(cookie);
            const qint64 remaining = cursor.bytes->size() - cursor.position;
            const qint64 copied = static_cast<qint64>(std::min<uint64_t>(count, static_cast<uint64_t>(remaining)));
            std::memcpy(buffer, cursor.bytes->constData() + cursor.position, static_cast<size_t>(copied));
            cursor.position += copied;
            return copied;
        };
        info->seek_fn = [](void* cookie, int64_t offset) -> int64_t {
            auto& cursor = *static_cast<Cursor*>(cookie);
            if (offset < 0 || offset > cursor.bytes->size()) return MPV_ERROR_GENERIC;
            return cursor.position = offset;
        };
        info->size_fn = [](void* cookie) -> int64_t { return static_cast<Cursor*>(cookie)->bytes->size(); };
        info->close_fn = [](void* cookie) { delete static_cast<Cursor*>(cookie); };
        return 0;
    }
};

VRVideoRendererEngine::VRVideoRendererEngine(VRVideoEngineConfig config, Callbacks callbacks)
    : QOpenGLWindow(NoPartialUpdate), m_config(config), m_callbacks(std::move(callbacks)) {
    setFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
    m_frameTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_frameTimer, &QTimer::timeout, this, [this] {
        if (m_pendingFrame) { m_pendingFrame = false; update(); }
    });
    connect(this, &QOpenGLWindow::frameSwapped, this, [this] {
        if (m_render && m_presentedFrame) {
            mpv_render_context_report_swap(m_render);
            if (!m_firstPresented) {
                m_firstPresented = true;
                emitEvent({{QStringLiteral("event"), QStringLiteral("first-frame-presented")}});
            }
        }
    });
    setFps(60);
}
VRVideoRendererEngine::~VRVideoRendererEngine() { stop(); }

bool VRVideoRendererEngine::openWallpaper(const VRVideoManifest& manifest, QString* error) {
    m_videoPath = QString::fromStdWString(manifest.videoPath().native());
    m_mpv = mpv_create();
    auto reject = [this, error](const QString& text) {
        if (error) *error = text;
        stop();
        return false;
    };
    if (!m_mpv) return reject(QStringLiteral("cannot create libmpv"));
    const auto option = [this](const char* name, const char* value) {
        return mpv_set_option_string(m_mpv, name, value);
    };
    const std::pair<const char*, const char*> options[] = {
        {"config", "no"}, {"terminal", "no"}, {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"}, {"osc", "no"}, {"vo", "libmpv"},
        {"hwdec", "auto"}, {"loop-file", "inf"}, {"keep-open", "yes"},
        {"audio-display", "no"}, {"video-sync", "audio"},
    };
    for (const auto& [name, value] : options) {
        const int result = option(name, value);
        if (result < 0) return reject(QStringLiteral("libmpv option %1: %2").arg(QString::fromLatin1(name), QString::fromUtf8(mpv_error_string(result))));
    }
    if (m_config.loadFromMemory) {
        QFile file(m_videoPath);
        if (!file.open(QIODevice::ReadOnly)) return reject(file.errorString());
        m_memory = std::make_unique<MemoryVideo>();
        m_memory->bytes = file.readAll();
        if (file.error() != QFileDevice::NoError) return reject(file.errorString());
    }
    int result = mpv_initialize(m_mpv);
    if (result < 0) return reject(QString::fromUtf8(mpv_error_string(result)));
    if (m_memory) {
        result = mpv_stream_cb_add_ro(m_mpv, "mirage-memory", m_memory.get(), &MemoryVideo::open);
        if (result < 0) return reject(QString::fromUtf8(mpv_error_string(result)));
    }
    mpv_request_log_messages(m_mpv, "info");
    mpv_set_wakeup_callback(m_mpv, wakeup, this);
    mpv_observe_property(m_mpv, 1, "video-out-params", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, 2, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 3, "time-pos", MPV_FORMAT_DOUBLE);
    property("volume", static_cast<double>(VRClampVideoVolume(m_config.initialVolume)) * 100.0);
    property("speed", std::clamp(m_config.initialSpeed, 0.05, 100.0));
    property("mute", m_config.muted);
    property("pause", !m_config.autoplay);
    applyFill();
    return true;
}

void VRVideoRendererEngine::stop() {
    m_frameTimer.stop();
    while (!m_snapshots.isEmpty()) {
        const SnapshotRequest request = m_snapshots.dequeue();
        emitEvent({{QStringLiteral("event"), QStringLiteral("snapshot-done")},
            {QStringLiteral("token"), request.token}, {QStringLiteral("ok"), false}});
    }
    if (m_mpv) mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
    if (m_render) {
        makeCurrent();
        mpv_render_context_set_update_callback(m_render, nullptr, nullptr);
        mpv_render_context_free(m_render);
        m_render = nullptr;
        doneCurrent();
    }
    if (m_mpv) { mpv_terminate_destroy(m_mpv); m_mpv = nullptr; }
    m_memory.reset();
}

void* VRVideoRendererEngine::resolveGL(void*, const char* name) {
    auto* current = QOpenGLContext::currentContext();
    return current ? reinterpret_cast<void*>(current->getProcAddress(name)) : nullptr;
}
void VRVideoRendererEngine::initializeGL() {
    if (!m_mpv) return;
    if (!context() || !context()->isValid() || QOpenGLContext::currentContext() != context()) {
        fail(QStringLiteral("native GPU OpenGL context is unavailable"));
        return;
    }
    const auto* renderer = context()->functions()->glGetString(GL_RENDERER);
    const QByteArray rendererName = renderer ? QByteArray(reinterpret_cast<const char*>(renderer)) : QByteArray();
    const QByteArray lower = rendererName.toLower();
    if (rendererName.isEmpty() || lower.contains("gdi generic") || lower.contains("llvmpipe")
        || lower.contains("softpipe") || lower.contains("swiftshader")) {
        fail(QStringLiteral("native GPU rendering is required; OpenGL renderer: %1").arg(QString::fromLatin1(rendererName)));
        return;
    }
    mpv_opengl_init_params gl{resolveGL, this};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    const int result = mpv_render_context_create(&m_render, m_mpv, params);
    if (result < 0) { fail(QStringLiteral("libmpv OpenGL initialization: %1").arg(QString::fromUtf8(mpv_error_string(result)))); return; }
    std::fprintf(stderr, "VideoWallpaper: OpenGL renderer: %s\n", rendererName.constData());
    mpv_render_context_set_update_callback(m_render, renderUpdate, this);
    handleCommand({{QStringLiteral("cmd"), QStringLiteral("capabilities")}});
    const QByteArray path = m_config.loadFromMemory ? QByteArrayLiteral("mirage-memory://video") : m_videoPath.toUtf8();
    const char* command[] = {"loadfile", path.constData(), "replace", nullptr};
    const int loadResult = mpv_command_async(m_mpv, 0, command);
    if (loadResult < 0) fail(QString::fromUtf8(mpv_error_string(loadResult)));
}
void VRVideoRendererEngine::paintGL() {
    if (!m_render) return;
    auto* gl = context()->functions();
    gl->glClearColor(0, 0, 0, 1);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    mpv_opengl_fbo fbo{static_cast<int>(defaultFramebufferObject()),
        qRound(width() * devicePixelRatio()), qRound(height() * devicePixelRatio()), 0};
    int flip = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo}, {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    const uint64_t flags = mpv_render_context_update(m_render);
    const int result = mpv_render_context_render(m_render, params);
    if (result < 0) fail(QString::fromUtf8(mpv_error_string(result)));
    else if (m_loaded && (flags & MPV_RENDER_UPDATE_FRAME)) m_presentedFrame = true;
    if (!m_snapshots.isEmpty()) {
        // NoPartialUpdate 只能在 swap 前回读当前 backbuffer；命令接收时不可直接 grabFramebuffer。
        const QImage image = result >= 0 ? grabFramebuffer() : QImage();
        while (!m_snapshots.isEmpty()) {
            const SnapshotRequest request = m_snapshots.dequeue();
            QSaveFile output(request.path);
            const bool ok = !image.isNull() && output.open(QIODevice::WriteOnly)
                && image.save(&output, "PNG") && output.commit();
            emitEvent({{QStringLiteral("event"), QStringLiteral("snapshot-done")},
                {QStringLiteral("token"), request.token}, {QStringLiteral("ok"), ok}});
        }
    }
}
void VRVideoRendererEngine::resizeGL(int, int) { emitPositionAvailability(); }
void VRVideoRendererEngine::wakeup(void* context) {
    auto* self = static_cast<VRVideoRendererEngine*>(context);
    QMetaObject::invokeMethod(self, [self] { self->processEvents(); }, Qt::QueuedConnection);
}
void VRVideoRendererEngine::renderUpdate(void* context) {
    auto* self = static_cast<VRVideoRendererEngine*>(context);
    QMetaObject::invokeMethod(self, [self] { self->requestFrame(); }, Qt::QueuedConnection);
}
void VRVideoRendererEngine::requestFrame() { if (m_render) m_pendingFrame = true; }

void VRVideoRendererEngine::processEvents() {
    if (!m_mpv) return;
    for (;;) {
        const mpv_event* event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) break;
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            const auto* message = static_cast<mpv_event_log_message*>(event->data);
            std::fprintf(stderr, "[mpv/%s] %s", message->prefix, message->text);
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            m_loaded = true;
            applyFill();
            emitPositionAvailability();
        } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            m_restarted = true;
            m_seekPending = false;
        } else if (event->event_id == MPV_EVENT_SEEK) {
            // 自然循环也可能产生 SEEK；只有用户 seek 命令设置排除标记。
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            const auto* end = static_cast<mpv_event_end_file*>(event->data);
            if (end->reason == MPV_END_FILE_REASON_ERROR) fail(QString::fromUtf8(mpv_error_string(end->error)));
            else if (end->reason == MPV_END_FILE_REASON_EOF)
                emitEvent({{QStringLiteral("event"), QStringLiteral("video-did-end")}});
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            const auto* change = static_cast<mpv_event_property*>(event->data);
            if (!change->data) continue;
            if (std::strcmp(change->name, "duration") == 0) m_duration = *static_cast<double*>(change->data);
            else if (std::strcmp(change->name, "time-pos") == 0) {
                const double position = *static_cast<double*>(change->data);
                // loop-file=inf 不保证 END_FILE。检测自然圈尾归零，避免把 pause/resume 的 restart 当作结束。
                if (m_restarted && !m_seekPending && m_duration > 0
                    && m_lastPosition >= std::max(m_duration * 0.5, m_duration - 0.3)
                    && position < std::min(m_duration * 0.5, 0.3))
                    emitEvent({{QStringLiteral("event"), QStringLiteral("video-did-end")}});
                m_lastPosition = position;
            } else if (std::strcmp(change->name, "video-out-params") == 0) {
                const auto* node = static_cast<mpv_node*>(change->data);
                if (node->format == MPV_FORMAT_NODE_MAP) {
                    for (int i = 0; i < node->u.list->num; ++i) {
                        const auto& value = node->u.list->values[i];
                        if (value.format != MPV_FORMAT_INT64) continue;
                        if (std::strcmp(node->u.list->keys[i], "dw") == 0) m_videoWidth = static_cast<int>(value.u.int64);
                        if (std::strcmp(node->u.list->keys[i], "dh") == 0) m_videoHeight = static_cast<int>(value.u.int64);
                    }
                    emitPositionAvailability();
                }
            }
        } else if (event->event_id == MPV_EVENT_COMMAND_REPLY || event->event_id == MPV_EVENT_SET_PROPERTY_REPLY) {
            if (event->error < 0) {
                std::fprintf(stderr, "VideoWallpaper: control: %s\n", mpv_error_string(event->error));
                emitEvent({{QStringLiteral("event"), QStringLiteral("command-error")},
                    {QStringLiteral("error"), QString::fromUtf8(mpv_error_string(event->error))}});
            }
        }
    }
}
void VRVideoRendererEngine::property(const char* name, const char* value) {
    if (m_mpv) mpv_set_property_async(m_mpv, 0, name, MPV_FORMAT_STRING, &value);
}
void VRVideoRendererEngine::property(const char* name, double value) {
    if (m_mpv) mpv_set_property_async(m_mpv, 0, name, MPV_FORMAT_DOUBLE, &value);
}
void VRVideoRendererEngine::property(const char* name, bool value) {
    int flag = value ? 1 : 0;
    if (m_mpv) mpv_set_property_async(m_mpv, 0, name, MPV_FORMAT_FLAG, &flag);
}
void VRVideoRendererEngine::applyFill() {
    property("keepaspect", m_config.fillMode != VRVideoFillModeStretch);
    property("panscan", m_config.fillMode == VRVideoFillModeCover ? 1.0 : 0.0);
    property("video-align-x", 2.0 * m_config.positionX - 1.0);
    property("video-align-y", 2.0 * m_config.positionY - 1.0);
    emitPositionAvailability();
    requestFrame();
}
void VRVideoRendererEngine::emitPositionAvailability() {
    const bool cover = m_config.fillMode == VRVideoFillModeCover && m_videoWidth > 0 && m_videoHeight > 0 && height() > 0;
    const double sourceAspect = m_videoHeight > 0 ? static_cast<double>(m_videoWidth) / m_videoHeight : 0;
    const double targetAspect = height() > 0 ? static_cast<double>(width()) / height() : 0;
    emitEvent({{QStringLiteral("event"), QStringLiteral("position-availability")},
        {QStringLiteral("x"), cover && sourceAspect > targetAspect + 0.0001},
        {QStringLiteral("y"), cover && sourceAspect + 0.0001 < targetAspect}});
}
void VRVideoRendererEngine::setFps(double fps) {
    if (!std::isfinite(fps) || fps <= 0) return;
    m_fps = std::clamp(fps, 1.0, 240.0);
    m_frameTimer.start(std::max(1, qRound(1000.0 / m_fps)));
}
void VRVideoRendererEngine::handleCommand(const QJsonObject& command) {
    const QString cmd = command.value(QStringLiteral("cmd")).toString();
    static const QSet<QString> supported = {
        QStringLiteral("pause"), QStringLiteral("resume"), QStringLiteral("play"), QStringLiteral("volume"),
        QStringLiteral("muted"), QStringLiteral("fillmode"), QStringLiteral("position"), QStringLiteral("speed"),
        QStringLiteral("fps"), QStringLiteral("loop"), QStringLiteral("seek"), QStringLiteral("power"),
        QStringLiteral("snapshot"), QStringLiteral("capabilities"),
    };
    if (cmd == QStringLiteral("capabilities")) {
        QJsonArray names;
        QStringList sorted = supported.values();
        sorted.sort();
        for (const auto& name : sorted) names.append(name);
        names.append(QStringLiteral("quit"));
        emitEvent({{QStringLiteral("event"), QStringLiteral("renderer-capabilities")},
            {QStringLiteral("renderer"), QStringLiteral("video")}, {QStringLiteral("commands"), names},
            {QStringLiteral("gpu"), true}, {QStringLiteral("audio"), true}});
        return;
    }
    if (!supported.contains(cmd)) {
        emitEvent({{QStringLiteral("event"), QStringLiteral("unsupported-command")},
            {QStringLiteral("cmd"), cmd}, {QStringLiteral("supported"), false}});
        return;
    }
    const auto invalid = [&] {
        emitEvent({{QStringLiteral("event"), QStringLiteral("command-error")},
            {QStringLiteral("cmd"), cmd}, {QStringLiteral("error"), QStringLiteral("invalid command value")}});
    };
    const QJsonValue value = command.value(QStringLiteral("value"));
    if (cmd == QStringLiteral("pause")) property("pause", true);
    else if (cmd == QStringLiteral("resume") || cmd == QStringLiteral("play")) property("pause", false);
    else if (cmd == QStringLiteral("volume") && value.isDouble()) property("volume", static_cast<double>(VRClampVideoVolume(static_cast<float>(value.toDouble()))) * 100);
    else if (cmd == QStringLiteral("muted") && value.isBool()) property("mute", value.toBool());
    else if (cmd == QStringLiteral("fillmode") && value.isString()) {
        VRVideoFillMode mode;
        if (VRParseVideoFillMode(value.toString().toStdString(), mode)) { m_config.fillMode = mode; applyFill(); }
        else invalid();
    } else if (cmd == QStringLiteral("position")) {
        const auto x = command.value(QStringLiteral("x"));
        const auto y = command.value(QStringLiteral("y"));
        if ((!x.isUndefined() && (!x.isDouble() || !std::isfinite(x.toDouble())))
            || (!y.isUndefined() && (!y.isDouble() || !std::isfinite(y.toDouble())))
            || (x.isUndefined() && y.isUndefined())) { invalid(); return; }
        if (x.isDouble() && std::isfinite(x.toDouble())) m_config.positionX = std::clamp(x.toDouble(), 0.0, 1.0);
        if (y.isDouble() && std::isfinite(y.toDouble())) m_config.positionY = std::clamp(y.toDouble(), 0.0, 1.0);
        applyFill();
    } else if (cmd == QStringLiteral("speed") && value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0) {
        property("speed", std::clamp(value.toDouble(), 0.05, 100.0));
    } else if (cmd == QStringLiteral("fps") && value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0) setFps(value.toDouble());
    else if (cmd == QStringLiteral("loop") && value.isBool()) property("loop-file", value.toBool() ? "inf" : "no");
    else if (cmd == QStringLiteral("seek") && value.isDouble() && std::isfinite(value.toDouble()) && m_mpv) {
        const QByteArray position = QByteArray::number(std::max(0.0, value.toDouble()));
        const char* args[] = {"seek", position.constData(), "absolute+exact", nullptr};
        m_seekPending = true;
        m_lastPosition = std::max(0.0, value.toDouble());
        mpv_command_async(m_mpv, 0, args);
    } else if (cmd == QStringLiteral("power")) {
        const QString state = command.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("pause")) property("pause", true);
        else if (state == QStringLiteral("run") || state == QStringLiteral("throttle")) property("pause", false);
        else invalid();
    } else if (cmd == QStringLiteral("snapshot")) {
        const QString path = command.value(QStringLiteral("path")).toString();
        const QString token = command.value(QStringLiteral("token")).toString();
        if (m_firstPresented && m_render && !path.isEmpty() && !token.isEmpty() && m_snapshots.size() < 32) {
            m_snapshots.enqueue({path, token});
            requestFrame();
        } else {
            emitEvent({{QStringLiteral("event"), QStringLiteral("snapshot-done")},
                {QStringLiteral("token"), token}, {QStringLiteral("ok"), false}});
        }
    } else invalid();
}
void VRVideoRendererEngine::emitEvent(const QJsonObject& event) { if (m_callbacks.event) m_callbacks.event(event); }
void VRVideoRendererEngine::fail(const QString& error) { if (m_callbacks.playbackError) m_callbacks.playbackError(error); }

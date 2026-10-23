#include "WebRendererEngine.h"

#include "WallpaperManifest.h"
#include "WRAudioTap.h"

#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCoreApplication>
#include <QBuffer>
#include <QFile>
#include <QMimeDatabase>
#include <QMimeType>
#include <QMouseEvent>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlSchemeHandler>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QtMath>

#include <chrono>
#include <cstdio>
#include <cstdlib>

// Serves a web wallpaper through one controlled origin so preset overlays and
// optional in-memory loading share the same path resolution and containment
// rules. The profile owns this handler for the lifetime of the web view.
class WallpaperSchemeHandler final : public QWebEngineUrlSchemeHandler {
public:
    explicit WallpaperSchemeHandler(QObject* parent = nullptr)
        : QWebEngineUrlSchemeHandler(parent) {}

    void configure(const QString& baseDirectory, const QStringList& overlays, bool cacheFiles) {
        m_baseDirectory = baseDirectory;
        m_overlayDirectories = overlays;
        m_cacheFiles = cacheFiles;
        m_cache.clear();
    }

    void requestStarted(QWebEngineUrlRequestJob* job) override {
        const QString relative = QUrl::fromPercentEncoding(job->requestUrl().path().toUtf8())
                                     .section('/', 1);
        const QString path = resolvePath(relative);
        if (std::getenv("WR_DEBUG") != nullptr) {
            qInfo("WebRenderer: resource %s -> %s", qPrintable(job->requestUrl().toString()),
                  path.isEmpty() ? "not found" : qPrintable(path));
        }
        if (path.isEmpty()) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }

        const QMimeType mime = QMimeDatabase().mimeTypeForFile(path);
        if (m_cacheFiles) {
            QByteArray data = m_cache.value(path);
            if (data.isEmpty() && QFileInfo(path).size() != 0) {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) {
                    job->fail(QWebEngineUrlRequestJob::RequestAborted);
                    return;
                }
                data = file.readAll();
                m_cache.insert(path, data);
            }
            // QWebEngine consumes replies asynchronously. Parenting the
            // device to the request keeps it alive for the complete transfer
            // and lets Qt release it when the request finishes.
            auto* buffer = new QBuffer(job);
            buffer->setData(data);
            buffer->open(QIODevice::ReadOnly);
            job->reply(mime.name().toLatin1(), buffer);
            return;
        }

        // The request owns the open file for the same asynchronous reply
        // contract; no other component closes or deletes this device.
        auto* file = new QFile(path, job);
        if (!file->open(QIODevice::ReadOnly)) {
            delete file;
            job->fail(QWebEngineUrlRequestJob::RequestAborted);
            return;
        }
        job->reply(mime.name().toLatin1(), file);
    }

private:
    QString resolvePath(const QString& relative) const {
        if (relative.isEmpty() || QDir::isAbsolutePath(relative)) return {};
        QStringList roots = m_overlayDirectories;
        roots.append(m_baseDirectory);
        for (const QString& rootValue : roots) {
            const QString root = QFileInfo(rootValue).canonicalFilePath();
            if (root.isEmpty()) continue;
            const QString candidate = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
            if (candidate.isEmpty() || candidate == root || !candidate.startsWith(root + QDir::separator())) continue;
            if (QFileInfo(candidate).isFile()) return candidate;
        }
        return {};
    }

    QString m_baseDirectory;
    QStringList m_overlayDirectories;
    bool m_cacheFiles = false;
    QHash<QString, QByteArray> m_cache;
};

namespace {

QString WebEngineShim() {
    return QStringLiteral(R"JS(
(function(){
  if(window.__mirage_web_shim)return;
  window.__mirage_web_shim=true;
  window.chrome=window.chrome||{runtime:{}};
  window.__mirage_paused=false;
  window.__mirage_volume=1.0;
  window.__mirage_muted=false;
  window.__wr_audio_listener_count=0;
  var __wr_audio_listeners=[];
  var __wr_audio_streams=[];
  window.wallpaperRegisterAudioListener=function(callback){
    if(__wr_audio_listeners.indexOf(callback)<0){
      __wr_audio_listeners.push(callback);
      window.__wr_audio_listener_count=__wr_audio_listeners.length;
    }
  };
  window.wallpaperRemoveAudioListener=function(callback){
    var index=__wr_audio_listeners.indexOf(callback);
    if(index>=0){
      __wr_audio_listeners.splice(index,1);
      window.__wr_audio_listener_count=__wr_audio_listeners.length;
    }
  };
  window.__wr_pushAudio=function(spectrum){
    __wr_audio_listeners.forEach(function(callback){
      try{callback(spectrum);}catch(error){console.error('WebRenderer audio listener:',error);}
    });
  };
  function __wr_applyMediaState(media){
    media.volume=window.__mirage_volume;
    media.muted=window.__mirage_muted;
    if(window.__mirage_paused&&!media.paused){media.__wr_host_was_playing=true;media.pause();}
  }
  function __wr_applyMediaTree(root){
    if(root instanceof HTMLMediaElement)__wr_applyMediaState(root);
    root.querySelectorAll('audio,video').forEach(__wr_applyMediaState);
  }
  window.wallpaperRegisterAudioStream=function(media){
    if(__wr_audio_streams.indexOf(media)<0)__wr_audio_streams.push(media);
    __wr_applyMediaState(media);
    return media;
  };
  window.wallpaperRemoveAudioStream=function(media){
    var index=__wr_audio_streams.indexOf(media);
    if(index>=0)__wr_audio_streams.splice(index,1);
  };
  new MutationObserver(function(records){
    records.forEach(function(record){
      record.addedNodes.forEach(function(node){
        if(node.nodeType===Node.ELEMENT_NODE)__wr_applyMediaTree(node);
      });
    });
  }).observe(document,{subtree:true,childList:true});
  var __wr_pending_props={},__wr_listener=null;
  function __wr_flushProps(){
    if(!__wr_listener||typeof __wr_listener.applyUserProperties!=='function')return false;
    var keys=Object.keys(__wr_pending_props);
    if(!keys.length)return true;
    var properties=__wr_pending_props;
    __wr_listener.applyUserProperties(properties);
    __wr_pending_props={};
    return true;
  }
  Object.defineProperty(window,'wallpaperPropertyListener',{configurable:true,enumerable:true,
    get:function(){return __wr_listener;},
    set:function(listener){
      __wr_listener=listener;
      __wr_flushProps();
      if(__wr_listener&&typeof __wr_listener.setPaused==='function')__wr_listener.setPaused(window.__mirage_paused);
    }});
  window.__wr_applyProps=function(properties){
    if(!properties||typeof properties!=='object')return;
    Object.keys(properties).forEach(function(key){__wr_pending_props[key]=properties[key];});
    __wr_flushProps();
  };
  var __wr_paused=false,__wr_fps=0,__wr_raf_serial=1,__wr_raf_pending={},__wr_raf_native=0,__wr_raf_delay=0;
  var __wr_native_raf=(window.requestAnimationFrame||function(callback){return window.setTimeout(function(){callback(performance.now());},16);}).bind(window);
  var __wr_native_cancel=(window.cancelAnimationFrame||window.clearTimeout).bind(window);
  var __wr_native_timeout=window.setTimeout.bind(window),__wr_native_clear_timeout=window.clearTimeout.bind(window);
  var __wr_native_interval=window.setInterval.bind(window),__wr_native_clear_interval=window.clearInterval.bind(window);
  function __wr_runRaf(timestamp){
    __wr_raf_native=0;__wr_raf_delay=0;
    if(__wr_paused)return;
    if(__wr_fps>0&&__wr_fps<60)window.__wr_last_raf=performance.now();
    var callbacks=__wr_raf_pending;__wr_raf_pending={};
    Object.keys(callbacks).forEach(function(key){callbacks[key](timestamp);});
  }
  function __wr_scheduleRaf(){
    if(__wr_paused||__wr_raf_native||__wr_raf_delay||!Object.keys(__wr_raf_pending).length)return;
    var interval=__wr_fps>0&&__wr_fps<60?1000/__wr_fps:0;
    if(interval){
      var now=performance.now(),wait=window.__wr_last_raf?Math.max(0,interval-(now-window.__wr_last_raf)):0;
      __wr_raf_delay=__wr_native_timeout(function(){__wr_raf_delay=0;__wr_raf_native=__wr_native_raf(__wr_runRaf);},wait);
    }else __wr_raf_native=__wr_native_raf(__wr_runRaf);
  }
  window.requestAnimationFrame=function(callback){var id=__wr_raf_serial++;__wr_raf_pending[id]=callback;__wr_scheduleRaf();return id;};
  window.cancelAnimationFrame=function(id){delete __wr_raf_pending[id];};
  window.__wr_setFps=function(fps){__wr_fps=(!isFinite(fps)||fps<=0||fps>=60)?0:fps;window.__wr_last_raf=0;};
  var __wr_timer_serial=1,__wr_timers={};
  function __wr_scheduleTimer(id){
    var timer=__wr_timers[id];if(!timer||__wr_paused)return;
    timer.due=Date.now()+timer.remaining;
    timer.native=__wr_native_timeout(function(){
      var current=__wr_timers[id];if(!current)return;current.native=0;
      if(__wr_paused){current.remaining=Math.max(0,current.due-Date.now());return;}
      if(!current.repeat)delete __wr_timers[id];
      current.callback.apply(window,current.arguments);
      if(current.repeat&&__wr_timers[id]){current.remaining=current.delay;__wr_scheduleTimer(id);}
    },timer.remaining);
  }
  function __wr_makeTimer(callback,delay,repeat,args){
    if(typeof callback!=='function')return repeat?__wr_native_interval(callback,delay):__wr_native_timeout(callback,delay);
    var id=__wr_timer_serial++,milliseconds=Math.max(0,Number(delay)||0);
    __wr_timers[id]={callback:callback,arguments:args,delay:milliseconds,remaining:milliseconds,repeat:repeat,native:0,due:0};
    __wr_scheduleTimer(id);return id;
  }
  window.setTimeout=function(callback,delay){return __wr_makeTimer(callback,delay,false,Array.prototype.slice.call(arguments,2));};
  window.setInterval=function(callback,delay){return __wr_makeTimer(callback,delay,true,Array.prototype.slice.call(arguments,2));};
  window.clearTimeout=window.clearInterval=function(id){
    var timer=__wr_timers[id];
    if(timer){if(timer.native)__wr_native_clear_timeout(timer.native);delete __wr_timers[id];return;}
    __wr_native_clear_timeout(id);__wr_native_clear_interval(id);
  };
  function __wr_setCssPaused(paused){
    var root=document.documentElement;if(!root)return;
    if(!document.getElementById('__wr_pause_style')){
      var style=document.createElement('style');style.id='__wr_pause_style';
      style.textContent='html.__wr-paused *,html.__wr-paused *::before,html.__wr-paused *::after{-webkit-animation-play-state:paused!important;animation-play-state:paused!important;}';
      (document.head||root).appendChild(style);
    }
    root.classList.toggle('__wr-paused',paused);
  }
  var __wr_hidden=false;
  Object.defineProperty(document,'hidden',{configurable:true,get:function(){return __wr_hidden;}});
  Object.defineProperty(document,'visibilityState',{configurable:true,get:function(){return __wr_hidden?'hidden':'visible';}});
  function __wr_setVisibility(hidden){
    if(__wr_hidden===hidden)return;__wr_hidden=hidden;
    document.dispatchEvent(new Event('visibilitychange'));
  }
  window.__wr_setPaused=function(paused){
    paused=!!paused;
    if(__wr_paused===paused)return;
    __wr_paused=paused;window.__mirage_paused=paused;window.wallpaperEngine_paused=paused;
    __wr_setCssPaused(paused);__wr_setVisibility(paused);
    if(paused){
      if(__wr_raf_native)__wr_native_cancel(__wr_raf_native);__wr_raf_native=0;
      if(__wr_raf_delay)__wr_native_clear_timeout(__wr_raf_delay);__wr_raf_delay=0;
      Object.keys(__wr_timers).forEach(function(id){var timer=__wr_timers[id];if(timer.native){__wr_native_clear_timeout(timer.native);timer.native=0;timer.remaining=Math.max(0,timer.due-Date.now());}});
    }else{
      __wr_scheduleRaf();Object.keys(__wr_timers).forEach(__wr_scheduleTimer);
    }
    document.querySelectorAll('audio,video').forEach(function(media){
      if(paused){
        media.__wr_host_was_playing=!media.paused;
        if(media.__wr_host_was_playing)media.pause();
      }else if(media.__wr_host_was_playing){
        media.__wr_host_was_playing=false;
        var promise=media.play();
        if(promise&&promise.catch)promise.catch(function(){});
      }
    });
    if(__wr_listener&&typeof __wr_listener.setPaused==='function')__wr_listener.setPaused(paused);
  };
  window.__wr_setVolume=function(volume){
    window.__mirage_volume=Math.max(0,Math.min(1,Number(volume)||0));
    __wr_applyMediaTree(document);
  };
  window.__wr_setMuted=function(muted){
    window.__mirage_muted=!!muted;
    __wr_applyMediaTree(document);
  };
})();
)JS");
}

} // namespace

WebRendererEngine::WebRendererEngine(const Config& config, QObject* parent)
    : QObject(parent), m_config(config), m_view(new QWebEngineView),
      m_audioTap(std::make_unique<WRAudioTap>()),
      m_schemeHandler(new WallpaperSchemeHandler(m_view->page()->profile())) {
    m_view->setAttribute(Qt::WA_DontShowOnScreen, true);
    m_view->setContextMenuPolicy(Qt::NoContextMenu);
    // WKWebView disables the user-gesture gate when audio playback is enabled.
    // QtWebEngine keeps Chromium's gate enabled by default, which prevents
    // Wallpaper Engine pages from starting their configured background audio
    // during document load. Keep the setting explicit so --no-audio remains a
    // real policy choice instead of relying on a backend default.
    m_view->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture,
                                     !m_config.enableAudioPlayback);
    QWebEngineScript shim;
    shim.setName(QStringLiteral("mirage-web-shim"));
    shim.setSourceCode(WebEngineShim());
    shim.setInjectionPoint(QWebEngineScript::DocumentCreation);
    shim.setWorldId(QWebEngineScript::MainWorld);
    shim.setRunsOnSubFrames(true);
    m_view->page()->scripts().insert(shim);
    m_view->page()->profile()->installUrlSchemeHandler("mirage-wallpaper", m_schemeHandler);
    connect(m_view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok) {
            emit failed(QStringLiteral("QtWebEngine failed to load wallpaper"));
            return;
        }
        // QWebEngineView installs its Chromium render widget as the focus
        // proxy. Native WebViewer events are delivered to this child, so the
        // off-screen wallpaper must use the same target explicitly.
        m_inputTarget = m_view->focusProxy();
        if (m_inputTarget == nullptr) {
            emit failed(QStringLiteral("QtWebEngine input target is unavailable"));
            return;
        }
        // All host state is retained until the document-start shim exists.
        // This avoids sending a snapshot into the previous navigation and lets
        // the shim hold it until a wallpaper installs its property listener.
        m_pageLoaded = true;
        setFrameRate(m_config.frameRate);
        setVolume(m_config.initialVolume);
        setMuted(m_muted);
        applyUserProperties(m_initialProperties);
        setPaused(m_paused);
        if (std::getenv("WR_DEBUG") != nullptr) {
            qInfo("WebRenderer: navigation finished; polling audio listener demand");
        }
        pollAudioDemand();
        emit contentReady();
    });
    m_captureTimer = new QTimer(this);
    connect(m_captureTimer, &QTimer::timeout, this, &WebRendererEngine::captureFrame);
    m_audioDemandTimer = new QTimer(this);
    m_audioDemandTimer->setInterval(200);
    connect(m_audioDemandTimer, &QTimer::timeout, this, &WebRendererEngine::pollAudioDemand);
    m_audioTimer = new QTimer(this);
    m_audioTimer->setInterval(1000 / 30);
    connect(m_audioTimer, &QTimer::timeout, this, &WebRendererEngine::tickAudioSpectrum);
    setFrameRate(m_config.frameRate);
}

WebRendererEngine::~WebRendererEngine() {
    stopAudioSpectrum();
    delete m_view;
}

void WebRendererEngine::openWallpaper(const WRManifest& manifest) {
    // Capturing a page while Chromium is still creating its compositor floods
    // the GUI thread with full-frame copies and only produces blank frames.
    // loadFinished restarts capture after the document is ready to paint.
    m_captureTimer->stop();
    m_pageLoaded = false;
    m_audioListenerDemand = false;
    reconcileAudioSpectrum();
    m_workshopDirectory = manifest.workshopDirectory();
    m_initialProperties = manifest.userProperties();
    m_schemeHandler->configure(m_workshopDirectory, m_config.assetOverlayDirectories,
                               m_config.loadFromMemory);
    const QString entry = QDir(m_workshopDirectory).filePath(manifest.entryHtml());
    if (!QFileInfo::exists(entry)) {
        emit failed(QStringLiteral("wallpaper entry does not exist: %1").arg(entry));
        return;
    }
    QUrl url;
    url.setScheme(QStringLiteral("mirage-wallpaper"));
    url.setHost(QStringLiteral("wallpaper"));
    url.setPath(QStringLiteral("/") + manifest.entryHtml());
    m_view->load(url);
}

void WebRendererEngine::evaluate(const QString& script) {
    m_view->page()->runJavaScript(script);
}

void WebRendererEngine::applyUserProperty(const QString& key, const QJsonValue& value) {
    // Keep the authoritative snapshot so a late listener or a later page load
    // sees every live change in the same wire format as the initial manifest.
    m_initialProperties.insert(key, value);
    if (!m_pageLoaded) return;
    QJsonObject property;
    property.insert(key, value);
    const QString json = QString::fromUtf8(QJsonDocument(property).toJson(QJsonDocument::Compact));
    evaluate(QStringLiteral("window.__wr_applyProps(%1);").arg(json));
}

void WebRendererEngine::applyUserProperties(const QJsonObject& properties) {
    m_initialProperties = properties;
    if (!m_pageLoaded) return;
    const QString json = QString::fromUtf8(QJsonDocument(properties).toJson(QJsonDocument::Compact));
    evaluate(QStringLiteral("window.__wr_applyProps(%1);").arg(json));
}

void WebRendererEngine::setPaused(bool paused) {
    m_paused = paused;
    if (!m_pageLoaded) {
        reconcileAudioSpectrum();
        emit audioSpectrumDemandChanged(m_audioListenerDemand && !m_paused);
        return;
    }
    // Host-driven resume is not a user gesture. Consume a rejected play()
    // promise here so a backend policy decision cannot surface as an uncaught
    // page exception; page-owned play() calls keep their normal semantics.
    evaluate(QStringLiteral("window.__wr_setPaused(%1);")
                 .arg(paused ? QStringLiteral("true") : QStringLiteral("false")));
    reconcileAudioSpectrum();
    emit audioSpectrumDemandChanged(m_audioListenerDemand && !m_paused);
}

void WebRendererEngine::setVolume(float volume) {
    m_volume = qBound(0.0f, volume, 1.0f);
    // Wallpaper Engine exposes host volume through the audio property while
    // the shim separately applies it to HTML media elements.
    applyUserProperty(QStringLiteral("audio"), QJsonObject{{QStringLiteral("value"), m_volume}});
    if (!m_pageLoaded) return;
    evaluate(QStringLiteral("window.__wr_setVolume(%1);").arg(m_volume, 0, 'f', 4));
}

void WebRendererEngine::setMuted(bool muted) {
    m_muted = muted;
    // Muting must not discard the configured volume; the listener receives
    // that value and the media shim applies the effective mute state.
    applyUserProperty(QStringLiteral("audio"), QJsonObject{{QStringLiteral("value"), m_volume}});
    if (!m_pageLoaded) return;
    evaluate(QStringLiteral("window.__wr_setMuted(%1);").arg(muted ? QStringLiteral("true") : QStringLiteral("false")));
}

void WebRendererEngine::setFrameRate(int fps) {
    const int bounded = qBound(1, fps, 240);
    m_config.frameRate = bounded;
    m_captureTimer->setInterval(1000 / bounded);
    if (m_pageLoaded) {
        if (!m_captureTimer->isActive()) m_captureTimer->start();
        evaluate(QStringLiteral("window.__wr_setFps(%1);").arg(bounded));
    }
}

void WebRendererEngine::setHostMediaPlaybackSuspended(bool suspended) {
    setPaused(suspended);
}

void WebRendererEngine::startAudioSpectrum() {
    m_audioSpectrumRequested = true;
    if (!m_audioDemandTimer->isActive()) m_audioDemandTimer->start();
    pollAudioDemand();
}

void WebRendererEngine::stopAudioSpectrum() {
    m_audioSpectrumRequested = false;
    m_audioDemandTimer->stop();
    m_audioTimer->stop();
    m_audioTap->stop();
    m_audioSpectrumStarted = false;
    m_audioSpectrumObserved = false;
}

void WebRendererEngine::pushAudioSpectrum(const std::array<float, 128>& spectrum) {
    if (!m_pageLoaded || !m_audioListenerDemand || m_paused) return;
    QJsonArray samples;
    for (float sample : spectrum) samples.append(static_cast<double>(sample));
    const QString json = QString::fromUtf8(QJsonDocument(samples).toJson(QJsonDocument::Compact));
    evaluate(QStringLiteral("window.__wr_pushAudio(%1);").arg(json));
}

void WebRendererEngine::pollAudioDemand() {
    if (!m_audioSpectrumRequested || !m_pageLoaded) return;
    // The shim owns this numeric field, so the callback consumes a known
    // protocol value instead of inspecting page-defined listener objects.
    m_view->page()->runJavaScript(QStringLiteral("window.__wr_audio_listener_count"),
                                  [this](const QVariant& result) {
        if (std::getenv("WR_DEBUG") != nullptr) {
            qInfo() << "WebRenderer: audio listener count query=" << result;
        }
        const bool needed = result.toInt() > 0;
        if (m_audioListenerDemand == needed) return;
        m_audioListenerDemand = needed;
        if (std::getenv("WR_DEBUG") != nullptr) {
            qInfo("WebRenderer: audio listener demand=%d", needed ? 1 : 0);
        }
        reconcileAudioSpectrum();
        emit audioSpectrumDemandChanged(needed && !m_paused);
    });
}

void WebRendererEngine::reconcileAudioSpectrum() {
    const bool needed = m_audioSpectrumRequested && m_config.enableAudioSpectrum &&
                        m_audioListenerDemand && !m_paused;
    if (!needed) {
        if (m_audioSpectrumStarted) {
            m_audioTimer->stop();
            m_audioTap->stop();
            m_audioSpectrumStarted = false;
            m_audioSpectrumObserved = false;
        }
        return;
    }
    if (m_audioSpectrumStarted) return;
    QString error;
    if (!m_audioTap->start(&error)) {
        qWarning("WebRenderer: audio spectrum disabled: %s", qPrintable(error));
        return;
    }
    m_audioSpectrumStarted = true;
    m_audioSpectrumObserved = false;
    if (std::getenv("WR_DEBUG") != nullptr) {
        qInfo("WebRenderer: audio spectrum capture started");
    }
    m_audioTimer->start();
}

void WebRendererEngine::tickAudioSpectrum() {
    std::array<float, 64> left {};
    std::array<float, 64> right {};
    if (!m_audioTap->copySpectrum(left, right)) return;
    if (!m_audioSpectrumObserved && std::getenv("WR_DEBUG") != nullptr) {
        qInfo("WebRenderer: audio spectrum first frame received");
    }
    m_audioSpectrumObserved = true;
    std::array<float, 128> spectrum {};
    for (std::size_t index = 0; index < left.size(); ++index) {
        spectrum[index] = left[index];
        spectrum[left.size() + index] = right[index];
    }
    pushAudioSpectrum(spectrum);
}

void WebRendererEngine::captureFrame() {
    if (m_paused || m_view->width() <= 0 || m_view->height() <= 0) return;
    const auto captureStart = std::chrono::steady_clock::now();
    const QPixmap pixmap = m_view->grab();
    if (pixmap.isNull()) return;
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGBA8888);
    if (qEnvironmentVariableIsSet("MIRAGE_DISPLAY_DIAGNOSTICS")) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - captureStart).count();
        std::fprintf(stderr, "WebWallpaper diagnostics: chromium_capture_rgba=%lldus bytes=%llu\n",
                     static_cast<long long>(elapsed),
                     static_cast<unsigned long long>(image.sizeInBytes()));
    }
    emit frameReady(image);
}

void WebRendererEngine::takeSnapshotToPath(const QString& path) {
    const QPixmap pixmap = m_view->grab();
    const bool ok = !pixmap.isNull() && pixmap.save(path);
    emit snapshotFinished(ok);
}

bool WebRendererEngine::mapPointerEventPosition(float x, float y, QPointF& position,
                                                QPointF& globalPosition) {
    // A document cannot consume input before loadFinished establishes its
    // render child; do not carry startup events into the loaded page.
    if (!m_pageLoaded) return false;
    if (m_inputTarget == nullptr) {
        // Latch the failure so a high-rate motion stream cannot emit the same
        // renderer error for every queued event.
        m_pageLoaded = false;
        emit failed(QStringLiteral("QtWebEngine input target was destroyed"));
        return false;
    }
    position = QPointF(static_cast<qreal>(x) * static_cast<qreal>(m_inputTarget->width()),
                       static_cast<qreal>(y) * static_cast<qreal>(m_inputTarget->height()));
    globalPosition = m_inputTarget->mapToGlobal(position.toPoint());
    return true;
}

void WebRendererEngine::sendPointerMotion(float x, float y) {
    QPointF position;
    QPointF globalPosition;
    if (!mapPointerEventPosition(x, y, position, globalPosition)) return;
    QMouseEvent event(QEvent::MouseMove, position, globalPosition, Qt::NoButton,
                      m_pressedButtons, Qt::NoModifier);
    if (!QCoreApplication::sendEvent(m_inputTarget, &event)) {
        qWarning("WebRenderer: QtWebEngine rejected pointer motion");
    }
}

void WebRendererEngine::sendPointerEnter(float x, float y) {
    QPointF position;
    QPointF globalPosition;
    if (!mapPointerEventPosition(x, y, position, globalPosition)) return;
    const QPointF scenePosition = m_inputTarget->mapTo(m_view, position.toPoint());
    QEnterEvent event(position, scenePosition, globalPosition);
    if (!QCoreApplication::sendEvent(m_inputTarget, &event)) {
        qWarning("WebRenderer: QtWebEngine rejected pointer enter");
    }
}

void WebRendererEngine::sendPointerLeave() {
    if (!m_pageLoaded) return;
    if (m_inputTarget == nullptr) {
        m_pageLoaded = false;
        emit failed(QStringLiteral("QtWebEngine input target was destroyed"));
        return;
    }
    QEvent event(QEvent::Leave);
    if (!QCoreApplication::sendEvent(m_inputTarget, &event)) {
        qWarning("WebRenderer: QtWebEngine rejected pointer leave");
    }
}

void WebRendererEngine::sendPointerButton(float x, float y, Qt::MouseButton button,
                                          bool pressed) {
    QPointF position;
    QPointF globalPosition;
    if (!mapPointerEventPosition(x, y, position, globalPosition)) return;
    if (button == Qt::NoButton) {
        emit failed(QStringLiteral("QtWebEngine received an invalid pointer button"));
        return;
    }
    if (pressed) m_pressedButtons |= button;
    else m_pressedButtons &= ~Qt::MouseButtons(button);
    QMouseEvent event(pressed ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
                      position, globalPosition, button, m_pressedButtons,
                      Qt::NoModifier);
    if (!QCoreApplication::sendEvent(m_inputTarget, &event)) {
        qWarning("WebRenderer: QtWebEngine rejected pointer button");
    }
}

void WebRendererEngine::sendPointerAxis(float x, float y, float deltaX, float deltaY,
                                        bool pixelBased) {
    QPointF position;
    QPointF globalPosition;
    if (!mapPointerEventPosition(x, y, position, globalPosition)) return;
    // Protocol deltas are logical wheel ticks; Qt expects 120 angle units for
    // one conventional tick. Continuous/finger deltas originated as pixels
    // divided by 120 and must return through QWheelEvent::pixelDelta.
    const QPoint delta(qRound(deltaX * 120.0f), qRound(deltaY * 120.0f));
    QWheelEvent event(position, globalPosition, pixelBased ? delta : QPoint(),
                      pixelBased ? QPoint() : delta, m_pressedButtons,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    if (!QCoreApplication::sendEvent(m_inputTarget, &event)) {
        qWarning("WebRenderer: QtWebEngine rejected pointer axis");
    }
}

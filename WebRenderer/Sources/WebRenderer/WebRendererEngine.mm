#import "WebRendererEngine.h"
#import "WallpaperManifest.h"
#import "WRAudioTap.h"
#import "WRURLSchemeHandler.h"

#import <WebKit/WebKit.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// HEIC keeps a 5K still in the low hundreds of KB. Macs without an HEVC encoder
// return a null destination, so fall back to JPEG rather than writing nothing.
static BOOL WRWriteImage(CGImageRef image, NSString *path, CFStringRef type) {
    NSURL *url = [NSURL fileURLWithPath:path];
    CGImageDestinationRef dest =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, type, 1, NULL);
    if (dest == NULL) return NO;
    NSDictionary *options = @{ (id)kCGImageDestinationLossyCompressionQuality: @0.9 };
    CGImageDestinationAddImage(dest, image, (__bridge CFDictionaryRef)options);
    const BOOL ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    return ok;
}

static BOOL WREncodeSnapshot(CGImageRef image, NSString *path) {
    if (image == NULL) return NO;
    if (WRWriteImage(image, path, (__bridge CFStringRef)UTTypeHEIC.identifier)) return YES;
    return WRWriteImage(image, path, (__bridge CFStringRef)UTTypeJPEG.identifier);
}

// A deferred WKWebView must be silent before any wallpaper script runs. Volume
// alone is not a WebKit master mute: an ordinary HTMLMediaElement can ignore the
// WE "audio" property, and WebAudio does not use it at all. Install this guard
// in every frame at document-start so both existing and future media stay
// blocked until Mirage replays the authoritative mute/pause state after the
// window has been activated.
static NSString *const kStrictAudioGuardJS = @"\
(function(){\
  if(window.__mirage_audio_guard_installed)return;\
  window.__mirage_audio_guard_installed=true;\
  var __isTop=false;try{__isTop=window.top===window;}catch(e){}\
  var __muted=__isTop?!!window.__mirage_audio_guard_initial_muted:true;\
  var __paused=false;\
  try{delete window.__mirage_audio_guard_initial_muted;}catch(e){}\
  function __blocked(){return __muted||__paused;}\
  var __media=new Set(),__mediaDesired=new WeakMap();\
  var __mediaProto=window.HTMLMediaElement&&HTMLMediaElement.prototype;\
  var __mutedDescriptor=__mediaProto?Object.getOwnPropertyDescriptor(__mediaProto,'muted'):null;\
  function __readNativeMuted(el){\
    try{return __mutedDescriptor&&__mutedDescriptor.get?!!__mutedDescriptor.get.call(el):!!el.muted;}catch(e){return false;}\
  }\
  function __writeNativeMuted(el,value){\
    try{if(__mutedDescriptor&&__mutedDescriptor.set)__mutedDescriptor.set.call(el,!!value);else el.muted=!!value;}catch(e){}\
  }\
  function __trackMedia(el){\
    try{\
      if(!__mediaProto||!(el instanceof HTMLMediaElement))return el;\
      if(!__mediaDesired.has(el))__mediaDesired.set(el,__readNativeMuted(el));\
      __media.add(el);\
      __writeNativeMuted(el,__blocked()?true:!!__mediaDesired.get(el));\
    }catch(e){}\
    return el;\
  }\
  function __scanMedia(root){\
    if(!root)return;\
    __trackMedia(root);\
    try{root.querySelectorAll('audio,video').forEach(__trackMedia);}catch(e){}\
  }\
  if(__mediaProto&&__mutedDescriptor&&__mutedDescriptor.get&&__mutedDescriptor.set&&__mutedDescriptor.configurable){\
    try{Object.defineProperty(__mediaProto,'muted',{\
      configurable:__mutedDescriptor.configurable,enumerable:__mutedDescriptor.enumerable,\
      get:function(){return __mutedDescriptor.get.call(this);},\
      set:function(value){__mediaDesired.set(this,!!value);__media.add(this);__mutedDescriptor.set.call(this,__blocked()?true:!!value);}\
    });}catch(e){}\
  }\
  if(__mediaProto&&typeof __mediaProto.play==='function'){\
    try{var __nativeMediaPlay=__mediaProto.play;__mediaProto.play=function(){__trackMedia(this);return __nativeMediaPlay.apply(this,arguments);};}catch(e){}\
  }\
  try{\
    var __mediaObserver=new MutationObserver(function(records){records.forEach(function(record){\
      if(record.type==='attributes')__trackMedia(record.target);\
      else record.addedNodes.forEach(__scanMedia);\
    });});\
    __mediaObserver.observe(document,{subtree:true,childList:true,attributes:true,attributeFilter:['muted','autoplay','src']});\
    window.__mirage_audio_guard_media_observer=__mediaObserver;\
  }catch(e){}\
  try{document.addEventListener('play',function(event){__trackMedia(event.target);},true);}catch(e){}\
  __scanMedia(document);\
  var __contexts=new Set(),__contextDesired=new WeakMap(),__contextOps=new WeakMap(),__contextObserved=new WeakSet();\
  function __ignorePromise(value){try{if(value&&value.catch)value.catch(function(){});}catch(e){}}\
  function __trackContext(context,ops){\
    if(!context||!ops)return context;\
    if(!__contextDesired.has(context))__contextDesired.set(context,context.state==='running');\
    __contexts.add(context);__contextOps.set(context,ops);\
    if(__blocked()&&context.state!=='closed'){\
      if(context.state==='running')__contextDesired.set(context,true);\
      __ignorePromise(ops.suspend.call(context));\
    }\
    if(!__contextObserved.has(context)){\
      __contextObserved.add(context);\
      try{context.addEventListener('statechange',function(){\
        if(__blocked()&&context.state==='running')__ignorePromise(ops.suspend.call(context));\
      });}catch(e){}\
    }\
    return context;\
  }\
  function __patchContext(name){\
    var Native=window[name];if(typeof Native!=='function'||!Native.prototype)return;\
    var proto=Native.prototype,ops=proto.__mirage_audio_guard_ops;\
    if(!ops){\
      ops={resume:proto.resume,suspend:proto.suspend,close:proto.close};\
      if(typeof ops.resume!=='function'||typeof ops.suspend!=='function')return;\
      try{Object.defineProperty(proto,'__mirage_audio_guard_ops',{value:ops});}catch(e){return;}\
      try{proto.resume=function(){\
        __trackContext(this,ops);__contextDesired.set(this,true);\
        if(__blocked()){__ignorePromise(ops.suspend.call(this));return Promise.resolve();}\
        return ops.resume.apply(this,arguments);\
      };}catch(e){}\
      try{proto.suspend=function(){__trackContext(this,ops);__contextDesired.set(this,false);return ops.suspend.apply(this,arguments);};}catch(e){}\
      if(typeof ops.close==='function'){try{proto.close=function(){__trackContext(this,ops);__contextDesired.set(this,false);return ops.close.apply(this,arguments);};}catch(e){}}\
    }\
    try{\
      var Wrapped=function(options){var context=arguments.length?new Native(options):new Native();return __trackContext(context,ops);};\
      Wrapped.prototype=proto;try{Object.setPrototypeOf(Wrapped,Native);}catch(e){}\
      window[name]=Wrapped;\
    }catch(e){}\
  }\
  __patchContext('AudioContext');__patchContext('webkitAudioContext');\
  function __applyAudioState(muted,paused,relay){\
    __muted=!!muted;__paused=!!paused;\
    __media.forEach(function(el){__writeNativeMuted(el,__blocked()?true:!!__mediaDesired.get(el));});\
    __scanMedia(document);\
    __contexts.forEach(function(context){\
      var ops=__contextOps.get(context);if(!ops||context.state==='closed')return;\
      if(__blocked()){if(context.state==='running')__contextDesired.set(context,true);__ignorePromise(ops.suspend.call(context));}\
      else if(__contextDesired.get(context))__ignorePromise(ops.resume.call(context));\
    });\
    if(relay)__broadcastState();\
  }\
  var __messageTag='mirage-strict-audio-v1';\
  function __stateMessage(){return{tag:__messageTag,kind:'state',muted:__muted,paused:__paused};}\
  function __broadcastState(){\
    var state=__stateMessage();\
    try{for(var i=0;i<window.frames.length;i++)window.frames[i].postMessage(state,'*');}catch(e){}\
  }\
  window.addEventListener('message',function(event){\
    var data=event.data;if(!data||data.tag!==__messageTag)return;\
    if(data.kind==='query'&&__isTop){try{if(event.source)event.source.postMessage(__stateMessage(),'*');}catch(e){}return;}\
    if(data.kind==='state'&&!__isTop&&event.source===window.top)__applyAudioState(!!data.muted,!!data.paused,false);\
  });\
  window.__mirageAudioGuardTrackMedia=__trackMedia;\
  window.__mirageAudioGuardSetMuted=function(value){if(__isTop)__applyAudioState(!!value,__paused,true);};\
  window.__mirageAudioGuardSetPaused=function(value){if(__isTop)__applyAudioState(__muted,!!value,true);};\
  __applyAudioState(__muted,__paused,__isTop);\
  if(!__isTop){try{window.top.postMessage({tag:__messageTag,kind:'query'},'*');}catch(e){}}\
})();";

static NSString *WRStrictAudioGuardScript(BOOL initiallyMuted) {
    NSString *prefix = initiallyMuted
        ? @"window.__mirage_audio_guard_initial_muted=true;"
        : @"window.__mirage_audio_guard_initial_muted=false;";
    return [prefix stringByAppendingString:kStrictAudioGuardJS];
}

// WE JS API shim, installed at document-start (≈ CEF OnContextCreated).
// Engine entrypoints (called via evaluateJavaScript):
//   __wr_applyProps(obj)   → wallpaperPropertyListener.applyUserProperties
//   __wr_setPaused(bool)   → wallpaperPropertyListener.setPaused + state
//   __wr_setFps(int)       → requestAnimationFrame throttle
//   __wr_applyMute(bool)   → strict all-frame HTML media + WebAudio guard
//   __wr_pauseStreams()    → registered audio streams .pause()
//   __wr_resumeStreams()   → resume streams the host paused
//   __wr_pushAudio([128])  → wallpaperRegisterAudioListener callbacks
static NSString *const kShimJS = @"\
(function(){\
  if (window.__wr_installed) return;\
  window.__wr_installed = true;\
  /* Chrome-compat: many WE wallpapers were authored against Chromium and \
     feature-sniff for window.chrome. WebKit lacks it; stub it. */\
  try { window.chrome = window.chrome || { runtime: {} }; } catch(e) {}\
  /* WE exposes file properties as filesystem paths. Legacy wallpapers often \
     prepend file:/// in page script, which WKWebView blocks for a custom-scheme \
     document. Rewrite those subresources through the allow-listed handler. */\
  function __wr_localAssetURL(raw){\
    if(typeof raw!=='string'||raw.slice(0,5).toLowerCase()!=='file:')return raw;\
    try {\
      var u=new URL(raw),p=decodeURIComponent(u.pathname||'');\
      while(p.length>1&&p.charAt(0)==='/'&&p.charAt(1)==='/')p=p.slice(1);\
      return 'we-wallpaper://wallpaper/__mirage_local?path='+encodeURIComponent(p);\
    } catch(e) { return raw; }\
  }\
  function __wr_rewriteCssURLs(value){\
    if(typeof value!=='string')return value;\
    return value.replace(/file:[^'\")]+/gi,function(url){return __wr_localAssetURL(url.trim());});\
  }\
  function __wr_wrapCssProperty(name){\
    try {\
      var d=Object.getOwnPropertyDescriptor(CSSStyleDeclaration.prototype,name);\
      if(d&&d.get&&d.set)Object.defineProperty(CSSStyleDeclaration.prototype,name,{\
        configurable:d.configurable,enumerable:d.enumerable,get:d.get,\
        set:function(v){d.set.call(this,__wr_rewriteCssURLs(v));}\
      });\
    } catch(e) {}\
  }\
  ['background','backgroundImage','cssText'].forEach(__wr_wrapCssProperty);\
  try {\
    var __wr_nativeSetProperty=CSSStyleDeclaration.prototype.setProperty;\
    CSSStyleDeclaration.prototype.setProperty=function(name,value,priority){\
      return __wr_nativeSetProperty.call(this,name,__wr_rewriteCssURLs(value),priority);\
    };\
  } catch(e) {}\
  function __wr_wrapURLProperty(proto,name){\
    try {\
      var d=Object.getOwnPropertyDescriptor(proto,name);\
      if(d&&d.get&&d.set)Object.defineProperty(proto,name,{\
        configurable:d.configurable,enumerable:d.enumerable,get:d.get,\
        set:function(v){d.set.call(this,__wr_localAssetURL(v));}\
      });\
    } catch(e) {}\
  }\
  [[HTMLImageElement.prototype,'src'],[HTMLMediaElement.prototype,'src'],\
   [HTMLSourceElement.prototype,'src']].forEach(function(x){__wr_wrapURLProperty(x[0],x[1]);});\
  try {\
    var __wr_nativeSetAttribute=Element.prototype.setAttribute;\
    Element.prototype.setAttribute=function(name,value){\
      var n=String(name).toLowerCase();\
      if(n==='src'||n==='poster')value=__wr_localAssetURL(value);\
      else if(n==='style')value=__wr_rewriteCssURLs(value);\
      return __wr_nativeSetAttribute.call(this,name,value);\
    };\
  } catch(e) {}\
  function __wr_rewriteElementAsset(element,name){\
    try {\
      var value=element.getAttribute(name);\
      if(!value||value.toLowerCase().indexOf('file:')<0)return;\
      var rewritten=name==='style'?__wr_rewriteCssURLs(value):__wr_localAssetURL(value);\
      if(rewritten!==value)__wr_nativeSetAttribute.call(element,name,rewritten);\
    } catch(e) {}\
  }\
  function __wr_scanElementAssets(root){\
    if(!root||root.nodeType!==1)return;\
    ['style','src','poster'].forEach(function(name){__wr_rewriteElementAsset(root,name);});\
    try {\
      root.querySelectorAll('[style],[src],[poster]').forEach(function(element){\
        ['style','src','poster'].forEach(function(name){__wr_rewriteElementAsset(element,name);});\
      });\
    } catch(e) {}\
  }\
  function __wr_installLocalAssetObserver(){\
    if(!document.documentElement||window.__wr_localAssetObserver)return;\
    var observer=new MutationObserver(function(records){\
      records.forEach(function(record){\
        if(record.type==='attributes')__wr_rewriteElementAsset(record.target,record.attributeName);\
        else record.addedNodes.forEach(__wr_scanElementAssets);\
      });\
    });\
    observer.observe(document.documentElement,{subtree:true,childList:true,attributes:true,attributeFilter:['src','poster']});\
    window.__wr_localAssetObserver=observer;\
    __wr_scanElementAssets(document.documentElement);\
  }\
  if(document.documentElement)__wr_installLocalAssetObserver();\
  document.addEventListener('DOMContentLoaded',__wr_installLocalAssetObserver,{once:true});\
  window.wallpaperEngine_paused = false;\
  var __streams = [];\
  window.wallpaperRegisterAudioStream = function(el){\
    if (el && __streams.indexOf(el) < 0) __streams.push(el);\
    try { if(window.__mirageAudioGuardTrackMedia)window.__mirageAudioGuardTrackMedia(el); } catch(e) {}\
    return el;\
  };\
  window.wallpaperRemoveAudioStream = function(el){\
    var i = __streams.indexOf(el); if (i >= 0) __streams.splice(i,1);\
  };\
  window.__wr_applyMute = function(m){\
    try { if(window.__mirageAudioGuardSetMuted)window.__mirageAudioGuardSetMuted(!!m); } catch(e){}\
  };\
  window.__wr_pauseStreams = function(){\
    for (var i=0;i<__streams.length;i++){\
      try {\
        var s = __streams[i];\
        if (!s.paused) { s.__wr_wasPlaying = true; s.pause(); }\
        else s.__wr_wasPlaying = false;\
      } catch(e){}\
    }\
  };\
  window.__wr_resumeStreams = function(){\
    for (var i=0;i<__streams.length;i++){\
      try {\
        var s = __streams[i];\
        if (s.__wr_wasPlaying) { s.__wr_wasPlaying = false; var p = s.play(); if (p && p.catch) p.catch(function(){}); }\
      } catch(e){}\
    }\
  };\
  var __listeners = [];\
  function __wr_reportAudioDemand(){\
    try {\
      if(window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.wrAudioDemand)\
        window.webkit.messageHandlers.wrAudioDemand.postMessage({needed:__listeners.length>0,count:__listeners.length});\
    } catch(e) {}\
  }\
  window.wallpaperRegisterAudioListener = function(cb){\
    if (typeof cb === 'function' && __listeners.indexOf(cb)<0) { __listeners.push(cb); __wr_reportAudioDemand(); }\
  };\
  window.wallpaperRemoveAudioListener = function(cb){\
    var i = __listeners.indexOf(cb); if (i >= 0) { __listeners.splice(i,1); __wr_reportAudioDemand(); }\
  };\
  window.__wr_pushAudio = function(arr){\
    for (var i=0;i<__listeners.length;i++){ try { __listeners[i](arr); } catch(e){} }\
  };\
  /* A real host pause must stop page-owned clocks as well as WE callbacks.\
     Keep a single rAF wrapper installed: replacing it for fps throttling used\
     to silently remove pause support. */\
  var __paused=false, __fps=0, __rafSerial=1, __rafPending={}, __rafNative=0, __rafDelay=0;\
  var __nativeRaf=(window.requestAnimationFrame||function(cb){return setTimeout(function(){cb(performance.now());},16);}).bind(window);\
  var __nativeCancel=(window.cancelAnimationFrame||clearTimeout).bind(window);\
  var __nativeSetTimeout=window.setTimeout.bind(window), __nativeClearTimeout=window.clearTimeout.bind(window);\
  function __runRaf(stamp){\
    __rafNative=0;__rafDelay=0;\
    if (__paused) return;\
    if (__fps>0&&__fps<60) window.__wr_lastRaf=performance.now();\
    var callbacks=__rafPending;__rafPending={};\
    Object.keys(callbacks).forEach(function(id){try{callbacks[id](stamp);}catch(e){__nativeSetTimeout(function(){throw e;},0);}});\
  }\
  function __scheduleRaf(){\
    if (__paused||__rafNative||__rafDelay||!Object.keys(__rafPending).length) return;\
    var interval=__fps>0&&__fps<60?1000/__fps:0;\
    if (interval) {\
      var now=performance.now(), wait=window.__wr_lastRaf?Math.max(0,interval-(now-window.__wr_lastRaf)):0;\
      __rafDelay=__nativeSetTimeout(function(){\
        __rafDelay=0; __rafNative=__nativeRaf(function(t){__runRaf(t);});\
      },wait);\
    } else {\
      __rafNative=__nativeRaf(function(t){__runRaf(t);});\
    }\
  }\
  window.requestAnimationFrame=function(cb){\
    var id=__rafSerial++; __rafPending[id]=cb; __scheduleRaf(); return id;\
  };\
  window.cancelAnimationFrame=function(id){\
    delete __rafPending[id];\
  };\
  window.__wr_setFps=function(fps){ __fps=(!isFinite(fps)||fps<=0||fps>=60)?0:fps; window.__wr_lastRaf=0; };\
  var __timerSerial=1, __timers={};\
  var __nativeSetInterval=window.setInterval.bind(window), __nativeClearInterval=window.clearInterval.bind(window);\
  function __scheduleTimer(id){\
    var t=__timers[id]; if (!t||__paused) return;\
    t.due=Date.now()+t.remaining;\
    t.native=__nativeSetTimeout(function(){\
      var current=__timers[id]; if (!current) return; current.native=0;\
      if (__paused) { current.remaining=Math.max(0,current.due-Date.now()); return; }\
      if (!current.repeat) delete __timers[id];\
      try { current.fn.apply(window,current.args); } catch(e){ __nativeSetTimeout(function(){throw e;},0); }\
      if (current.repeat&&__timers[id]) { current.remaining=current.delay; __scheduleTimer(id); }\
    },t.remaining);\
  }\
  function __makeTimer(fn,delay,repeat,args){\
    if (typeof fn!=='function') return repeat?__nativeSetInterval(fn,delay):__nativeSetTimeout(fn,delay);\
    var id=__timerSerial++, ms=Math.max(0,Number(delay)||0);\
    __timers[id]={fn:fn,args:args,delay:ms,remaining:ms,repeat:repeat,native:0,due:0}; __scheduleTimer(id); return id;\
  }\
  window.setTimeout=function(fn,delay){return __makeTimer(fn,delay,false,Array.prototype.slice.call(arguments,2));};\
  window.setInterval=function(fn,delay){return __makeTimer(fn,delay,true,Array.prototype.slice.call(arguments,2));};\
  window.clearTimeout=window.clearInterval=function(id){\
    var t=__timers[id]; if (t) { if(t.native) __nativeClearTimeout(t.native); delete __timers[id]; return; } __nativeClearTimeout(id); __nativeClearInterval(id);\
  };\
  function __allMedia(){\
    var a=__streams.slice(); try { var m=document.querySelectorAll('audio,video'); for(var i=0;i<m.length;i++) if(a.indexOf(m[i])<0)a.push(m[i]); }catch(e){} return a;\
  }\
  function __pauseMedia(){ var a=__allMedia(); for(var i=0;i<a.length;i++)try{var s=a[i];s.__wr_wasPlaying=!s.paused;if(s.__wr_wasPlaying)s.pause();}catch(e){} }\
  function __resumeMedia(){ var a=__allMedia(); for(var i=0;i<a.length;i++)try{var s=a[i];if(s.__wr_wasPlaying){s.__wr_wasPlaying=false;var p=s.play();if(p&&p.catch)p.catch(function(){});}}catch(e){} }\
  function __setCssPaused(p){\
    var root=document.documentElement; if(!root)return;\
    if(!document.getElementById('__wr_pause_style')){var style=document.createElement('style');style.id='__wr_pause_style';style.textContent='html.__wr-paused *,html.__wr-paused *::before,html.__wr-paused *::after{-webkit-animation-play-state:paused!important;animation-play-state:paused!important;}';(document.head||root).appendChild(style);}\
    if(p)root.classList.add('__wr-paused');else root.classList.remove('__wr-paused');\
  }\
  window.__wr_pauseStreams=__pauseMedia; window.__wr_resumeStreams=__resumeMedia;\
  /* WebKit never marks this page hidden: the wallpaper window is canHide=NO + \
     orderFrontRegardless, so AppKit never reports it occluded and WebKit's own \
     hidden-page throttling therefore never engages. rAF and timers are already \
     intercepted above, which is what actually stops the work; this additionally \
     surfaces the paused state through the standard Page Visibility API so that \
     wallpapers listening for visibilitychange (many drop their own work on it) \
     observe it too. */\
  var __wrHidden=false;\
  try{\
    Object.defineProperty(document,'hidden',{configurable:true,get:function(){return __wrHidden;}});\
    Object.defineProperty(document,'visibilityState',{configurable:true,get:function(){return __wrHidden?'hidden':'visible';}});\
  }catch(e){}\
  function __setPageVisibility(hidden){\
    if(__wrHidden===hidden)return; __wrHidden=hidden;\
    try{document.dispatchEvent(new Event('visibilitychange'));}catch(e){}\
  }\
  var __wr_pendingProps={},__wr_listener=null;\
  function __wr_flushProps(){\
    var listener=__wr_listener;\
    if(listener&&typeof listener.applyUserProperties==='function'){\
      try {\
        var applied=__wr_pendingProps;\
        if(Object.keys(applied).length){\
          listener.applyUserProperties(applied);__wr_pendingProps={};\
          /* WebKit does not consistently expose direct CSSStyleDeclaration \
             property assignments through overridable JS setters. Legacy WE \
             pages commonly set style.background='url(file:///...)' inside \
             applyUserProperties. Scan once after that callback instead of \
             permanently observing every style mutation. */\
          __wr_scanElementAssets(document.documentElement);\
          __nativeSetTimeout(function(){__wr_scanElementAssets(document.documentElement);},0);\
        }\
        return true;\
      }\
      catch(e){ console.error('WebRenderer applyUserProperties:',e); }\
    }\
    return false;\
  }\
  try {\
    Object.defineProperty(window,'wallpaperPropertyListener',{configurable:true,enumerable:true,\
      get:function(){return __wr_listener;},set:function(value){__wr_listener=value;__wr_flushProps();}});\
  } catch(e) { __wr_listener=window.wallpaperPropertyListener||null; }\
  window.__wr_applyProps=function(props){\
    if(!props||typeof props!=='object')return;\
    Object.keys(props).forEach(function(key){__wr_pendingProps[key]=props[key];});\
    __wr_flushProps();\
  };\
  window.__wr_applySnapshot=function(props,generation){\
    if(!props||typeof props!=='object')return -1;\
    try {\
      Object.keys(props).forEach(function(key){__wr_pendingProps[key]=props[key];});\
      var applied=__wr_flushProps();\
      if(window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.wrProperties)\
        window.webkit.messageHandlers.wrProperties.postMessage({generation:String(generation||'snapshot'),count:Object.keys(props).length,applied:applied});\
      return applied?1:0;\
    } catch(e) { console.error('WebRenderer applyUserProperties:',e); return -1; }\
  };\
  window.__wr_setPaused = function(p){\
    p=!!p;\
    try { if(window.__mirageAudioGuardSetPaused)window.__mirageAudioGuardSetPaused(p); } catch(e){}\
    if (__paused===p) return; __paused=p; window.wallpaperEngine_paused=p;\
    __setCssPaused(p);\
    __setPageVisibility(p);\
    if(p){\
      if(__rafNative)__nativeCancel(__rafNative);__rafNative=0;\
      if(__rafDelay)__nativeClearTimeout(__rafDelay);__rafDelay=0;\
      Object.keys(__timers).forEach(function(id){var t=__timers[id];if(t.native){__nativeClearTimeout(t.native);t.native=0;t.remaining=Math.max(0,t.due-Date.now());}});\
      __pauseMedia();\
    }else{\
      __scheduleRaf();\
      Object.keys(__timers).forEach(function(id){__scheduleTimer(id);});\
      __resumeMedia();\
    }\
    try {\
      if (window.wallpaperPropertyListener && typeof window.wallpaperPropertyListener.setPaused === 'function')\
        window.wallpaperPropertyListener.setPaused(p);\
    } catch(e){ console.error('WebRenderer setPaused:', e); }\
  };\
  /* Synthetic mouse-event dispatch — used by WRDesktopInputForwarder to feed \
     the page real desktop clicks/moves (the wallpaper window sits below \
     Finder's desktop window and never receives them directly). \
     4th arg `buttons` maps to MouseEvent.buttons (0=none, 1=left held). */\
  window.__wr_dispatchMouse = function(type, x, y, buttons){\
    try {\
      var btn = (type === 'mousedown' || type === 'mouseup' || type === 'click') ? 0 : -1;\
      var el = document.elementFromPoint(x, y) || document.body;\
      el.dispatchEvent(new MouseEvent(type, {\
        bubbles: true, cancelable: true, view: window, clientX: x, clientY: y,\
        button: (btn >= 0 ? btn : 0), buttons: (buttons || 0)\
      }));\
    } catch(e){ console.error('WebRenderer dispatchMouse:', e); }\
  };\
  /* Pipe console.* to native (≈ OWE ClientHandler::OnConsoleMessage). */\
  if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.wrConsole) {\
    var __oc = window.console || {};\
    var __wrap = function(level, orig){\
      return function(){\
        try {\
          var args = Array.prototype.slice.call(arguments);\
          var msg = args.map(function(a){\
            try { return (typeof a === 'object') ? JSON.stringify(a) : String(a); } catch(e){ return String(a); }\
          }).join(' ');\
          window.webkit.messageHandlers.wrConsole.postMessage({type: level, message: msg});\
        } catch(e){}\
        if (typeof orig === 'function') { try { orig.apply(window.console, args); } catch(e){} }\
      };\
    };\
    window.console = {\
      log: __wrap('log', __oc.log), info: __wrap('info', __oc.info),\
      warn: __wrap('warn', __oc.warn), error: __wrap('error', __oc.error),\
      debug: __wrap('debug', __oc.debug)\
    };\
    window.onerror = function(msg, src, line, col){\
      try { window.webkit.messageHandlers.wrConsole.postMessage({type:'error', message:'onerror: '+msg+' ('+src+':'+line+':'+col+')'}); } catch(e){}\
      return false;\
    };\
  }\
})();";

// WRNetworkPolicyBlock. Default-deny with ignore-previous-rules exemptions for
// the schemes the wallpaper document itself needs. Later rules win in WebKit
// content extensions, so the exemptions override the catch-all block.
static NSString *const kBlockExternalRules = @"["
    "{\"trigger\":{\"url-filter\":\".*\"},\"action\":{\"type\":\"block\"}},"
    "{\"trigger\":{\"url-filter\":\"^we-wallpaper://\"},\"action\":{\"type\":\"ignore-previous-rules\"}},"
    "{\"trigger\":{\"url-filter\":\"^about:\"},\"action\":{\"type\":\"ignore-previous-rules\"}},"
    "{\"trigger\":{\"url-filter\":\"^data:\"},\"action\":{\"type\":\"ignore-previous-rules\"}},"
    "{\"trigger\":{\"url-filter\":\"^blob:\"},\"action\":{\"type\":\"ignore-previous-rules\"}}"
    "]";

static NSString *const kBlockRuleListIdentifier = @"MirageWebRendererBlockExternal";

// WRNetworkPolicyObserve. A WKContentRuleList CANNOT report without blocking:
// its only actions are block, block-cookies, css-display-none,
// ignore-previous-rules and make-https — there is no "report" action, and
// WKWebView exposes no public per-resource load delegate. So observation is
// done from inside the page instead: PerformanceObserver('resource') yields an
// entry for every subresource actually fetched (img/script/css/font/xhr/fetch)
// with its absolute URL and without affecting the load, and WebSocket and
// sendBeacon — which resource timing does not cover — are wrapped directly.
// Limits, stated honestly: requests that fail before producing a timing entry
// (DNS/TLS failure, blocked-by-CSP) are not reported, and a page that deletes
// PerformanceObserver before its own requests cannot be observed this way.
// This is auditing, not enforcement; use --network-policy=block for that.
static NSString *const kNetworkObserverJS = @"\
(function(){\
  if (window.__wr_netobs) return;\
  window.__wr_netobs = true;\
  var __seen = {}, __count = 0, __limit = 256;\
  function __post(kind, url){\
    try {\
      if (typeof url !== 'string' || !url) return;\
      if (/^(we-wallpaper|about|data|blob):/i.test(url)) return;\
      var key = kind + ' ' + url;\
      if (__seen[key] || __count >= __limit) return;\
      __seen[key] = 1; __count++;\
      if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.wrNetwork)\
        window.webkit.messageHandlers.wrNetwork.postMessage({kind:String(kind), url:String(url)});\
    } catch(e) {}\
  }\
  function __drain(list){\
    try { list.getEntries().forEach(function(e){ __post(e.initiatorType || 'resource', e.name); }); }\
    catch(e) {}\
  }\
  try {\
    var __po = new PerformanceObserver(function(list){ __drain(list); });\
    try { __po.observe({type:'resource', buffered:true}); }\
    catch(e) { __po.observe({entryTypes:['resource']}); }\
  } catch(e) {}\
  try {\
    var __NativeWS = window.WebSocket;\
    if (__NativeWS) {\
      var __WS = function(url, protocols){\
        __post('websocket', String(url));\
        return protocols === undefined ? new __NativeWS(url) : new __NativeWS(url, protocols);\
      };\
      __WS.prototype = __NativeWS.prototype;\
      ['CONNECTING','OPEN','CLOSING','CLOSED'].forEach(function(k){ try { __WS[k] = __NativeWS[k]; } catch(e) {} });\
      window.WebSocket = __WS;\
    }\
  } catch(e) {}\
  try {\
    var __beacon = navigator.sendBeacon;\
    if (typeof __beacon === 'function')\
      navigator.sendBeacon = function(url, data){ __post('beacon', String(url)); return __beacon.call(navigator, url, data); };\
  } catch(e) {}\
})();";

// Cap on distinct observed requests reported to stderr, so a wallpaper cannot
// turn the audit log into a denial of service against whoever drains stderr.
static const NSUInteger kMaxNetworkObservations = 256;
static const NSUInteger kMaxObservedURLLength = 512;

static NSString *const kDefaultUserAgent =
    @"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
     "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

// Script-message bodies come from untrusted wallpaper JS: any page can
// postMessage() an arbitrary JSON value to every registered handler. Checking
// only the container is not enough — {"count":null} arrives as NSNull and
// -[NSNull integerValue] (or -[NSNumber UTF8String]) is an unrecognized
// selector that aborts the process. Every value is coerced through these.
static NSDictionary *WRDictionaryValue(id value) {
    return [value isKindOfClass:[NSDictionary class]] ? (NSDictionary *)value : nil;
}

static NSNumber *WRNumberValue(id value) {
    return [value isKindOfClass:[NSNumber class]] ? (NSNumber *)value : nil;
}

static NSString *WRStringValue(id value) {
    return [value isKindOfClass:[NSString class]] ? (NSString *)value : nil;
}

// WKUserContentController retains its script message handlers STRONGLY, so
// registering the engine itself closed the cycle
// ucc → engine → webView → configuration → ucc. Nothing ever broke it: the
// engine, its WRAudioTap (hence a live system-wide audio tap) and the
// WKWebView were immortal. Register this weak forwarder instead and drop the
// names in -[WebRendererEngine dealloc].
@interface WRWeakScriptMessageHandler : NSObject <WKScriptMessageHandler>
@property (nonatomic, weak) id<WKScriptMessageHandler> target;
@end

@implementation WRWeakScriptMessageHandler
- (void)userContentController:(WKUserContentController *)ucc
      didReceiveScriptMessage:(WKScriptMessage *)message {
    [self.target userContentController:ucc didReceiveScriptMessage:message];
}
@end

@interface WebRendererEngine () <WKScriptMessageHandler>
@property (nonatomic, strong) WKWebView *webView;
@property (nonatomic, strong) WKUserContentController *userContentController;
@property (nonatomic, strong) WRWeakScriptMessageHandler *messageHandlerProxy;
@property (nonatomic, strong) NSMutableSet<NSString *> *scriptHandlerNames;
@property (nonatomic, strong) WRAudioTap *audioTap;
@property (nonatomic, strong) NSTimer *audioTimer;
@property (nonatomic, strong) WRManifest *manifest;
@property (nonatomic, strong) WRURLSchemeHandler *schemeHandler;
@property (nonatomic, assign) BOOL didFinishLoad;
@property (nonatomic, assign) BOOL contentReadyReported;
@property (nonatomic, assign) NSUInteger contentReadinessGeneration;
@property (nonatomic, assign) BOOL networkRulesReady;
@property (nonatomic, assign) BOOL networkRulesFailed;
@property (nonatomic, assign) BOOL pendingWallpaperLoad;
@property (nonatomic, assign) BOOL initialMediaPlaybackStateReady;
@property (nonatomic, assign) BOOL desiredHostMediaPlaybackSuspended;
@property (nonatomic, assign) BOOL hostMediaPlaybackSuspended;
@property (nonatomic, assign) BOOL hostMediaPlaybackRequestInFlight;
@property (nonatomic, strong) NSMutableArray<NSNumber *> *hostMediaPlaybackStates;
@property (nonatomic, strong) NSMutableArray *hostMediaPlaybackCompletions;
@property (nonatomic, assign) NSUInteger networkObservationCount;
@property (nonatomic, strong) NSMutableArray<NSString *> *pendingJS;
@property (nonatomic, assign) float volume;
@property (nonatomic, assign) BOOL muted;
@property (nonatomic, strong) NSMutableDictionary<NSString *, id> *userPropertySnapshot;
@property (nonatomic, copy) NSString *userPropertyGeneration;
@property (nonatomic, assign) NSUInteger propertyApplySerial;
@property (nonatomic, assign) BOOL propertySnapshotApplied;
@property (nonatomic, assign) BOOL audioSpectrumStarted;
@property (nonatomic, assign) BOOL audioSpectrumRequested;
@property (nonatomic, assign) BOOL audioListenerDemand;
@property (nonatomic, assign) BOOL paused;
- (void)installNetworkRuleList;
- (void)loadWallpaperIfReady;
- (void)loadWallpaperEntry;
- (void)startNextHostMediaPlaybackRequest;
@end

@implementation WebRendererEngine {
    WREngineConfig _config;
}

+ (WREngineConfig)defaultConfig {
    WREngineConfig c;
    c.enableInspector = YES;
    c.enableAudioSpectrum = YES;
    c.enableAudioPlayback = YES;
    c.initiallySuspendsMediaPlayback = NO;
    c.initialVolume = 1.0f;
    c.frameRate = 60;
    c.loadFromMemory = NO;
    c.networkPolicy = WRNetworkPolicyObserve;
    c.userAgent = nil;
    c.assetOverlayDirectories = nil;
    return c;
}

- (instancetype)initWithFrame:(NSRect)frame config:(WREngineConfig)config {
    self = [super init];
    if (self) {
        _config = config;
        _pendingJS = [NSMutableArray array];
        _volume = config.initialVolume;
        _muted = (config.initialVolume <= 0.0f);
        _initialMediaPlaybackStateReady = !config.initiallySuspendsMediaPlayback;
        _desiredHostMediaPlaybackSuspended = config.initiallySuspendsMediaPlayback;
        _hostMediaPlaybackSuspended = NO;
        _hostMediaPlaybackStates = [NSMutableArray array];
        _hostMediaPlaybackCompletions = [NSMutableArray array];
        _audioTap = [[WRAudioTap alloc] init];
        [self setupWebViewWithFrame:frame];
    }
    return self;
}

- (void)setupWebViewWithFrame:(NSRect)frame {
    WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
    WKUserContentController *ucc = [WKUserContentController new];
    WKUserScript *audioGuard = [[WKUserScript alloc]
        initWithSource:WRStrictAudioGuardScript(
            _muted || _config.initiallySuspendsMediaPlayback)
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart
        forMainFrameOnly:NO];
    [ucc addUserScript:audioGuard];
    WKUserScript *shim = [[WKUserScript alloc] initWithSource:kShimJS
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:YES];
    [ucc addUserScript:shim];
    _messageHandlerProxy = [WRWeakScriptMessageHandler new];
    _messageHandlerProxy.target = self;
    // Two of these registrations are conditional, so -dealloc cannot just
    // remove a hardcoded list: removing a name that was never added only
    // happens to be a no-op in current WebKit. Record exactly what was
    // registered and remove exactly that, so the two lists cannot drift.
    _scriptHandlerNames = [NSMutableSet set];
    WRWeakScriptMessageHandler *proxy = _messageHandlerProxy;
    NSMutableSet<NSString *> *handlerNames = _scriptHandlerNames;
    void (^addScriptHandler)(NSString *) = ^(NSString *name) {
        [ucc addScriptMessageHandler:proxy name:name];
        [handlerNames addObject:name];
    };
    if (_config.enableInspector || getenv("WR_DEBUG") != NULL) {
        addScriptHandler(@"wrConsole");
    }
    addScriptHandler(@"wrProperties");
    addScriptHandler(@"wrAudioDemand");
    if (_config.networkPolicy == WRNetworkPolicyObserve) {
        WKUserScript *netobs = [[WKUserScript alloc] initWithSource:kNetworkObserverJS
                                                      injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                   forMainFrameOnly:YES];
        [ucc addUserScript:netobs];
        addScriptHandler(@"wrNetwork");
    }
    _userContentController = ucc;
    cfg.userContentController = ucc;
    // Untrusted page script has no legitimate need to open windows, and the
    // wallpaper host has no UI to show them in.
    cfg.preferences.javaScriptCanOpenWindowsAutomatically = NO;
    cfg.suppressesIncrementalRendering = NO;
    if (_config.enableAudioPlayback) {
        cfg.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    }

    // Custom scheme = the WKWebView equivalent of CEF's --allow-file-access-from-files.
    _schemeHandler = [WRURLSchemeHandler new];
    [cfg setURLSchemeHandler:_schemeHandler forURLScheme:@"we-wallpaper"];

    // Untrusted Workshop content must not get on-disk cookies/localStorage/
    // IndexedDB/cache shared with every other wallpaper. This has to be set on
    // the configuration BEFORE the web view is created: -[WKWebView
    // configuration] returns a COPY, so assigning it afterwards is a no-op.
    cfg.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];

    _webView = [[WKWebView alloc] initWithFrame:frame configuration:cfg];
    _webView.navigationDelegate = self;
    _webView.customUserAgent = (_config.userAgent.length > 0) ? _config.userAgent : kDefaultUserAgent;
    if (@available(macOS 13.0, *)) {
        _webView.inspectable = _config.enableInspector ? YES : NO;
    }

    if (_config.initiallySuspendsMediaPlayback) {
        __weak WebRendererEngine *weakSelf = self;
        [self setHostMediaPlaybackSuspended:YES completion:^{
            WebRendererEngine *strongSelf = weakSelf;
            if (strongSelf == nil) return;
            strongSelf.initialMediaPlaybackStateReady = YES;
            [strongSelf loadWallpaperIfReady];
        }];
    }

    switch (_config.networkPolicy) {
    case WRNetworkPolicyBlock:
        [self installNetworkRuleList];
        break;
    case WRNetworkPolicyObserve:
        fprintf(stderr, "WebRenderer: network policy=observe "
                        "(remote requests are logged, not blocked)\n");
        break;
    case WRNetworkPolicyAllow:
        fprintf(stderr, "WebRenderer: network policy=allow (no egress restriction)\n");
        break;
    }
}

- (void)dealloc {
    // KVO/notification-style registrations that outlive the object crash the
    // process, and WKUserContentController holds its handlers strongly — drop
    // everything explicitly. No property accessors, no blocks: ivars only.
    [_audioTimer invalidate];
    _audioTimer = nil;
    [_audioTap stop];
    for (NSString *name in _scriptHandlerNames) {
        [_userContentController removeScriptMessageHandlerForName:name];
    }
    [_scriptHandlerNames removeAllObjects];
    [_userContentController removeAllUserScripts];
    [_userContentController removeAllContentRuleLists];
}

#pragma mark - Network policy

// Rule-list compilation is ASYNCHRONOUS, so -openWallpaper: defers the page
// load until the list is actually installed on the (shared) user content
// controller. Note that -[WKWebView configuration] hands back a copy, so the
// list must be added to the controller instance we kept, not to
// webView.configuration.userContentController.
- (void)installNetworkRuleList {
    WKContentRuleListStore *store = [WKContentRuleListStore defaultStore];
    if (store == nil) {
        fprintf(stderr, "WebRenderer: network policy=block cannot be enforced "
                        "(no content rule list store); refusing to load wallpaper\n");
        _networkRulesFailed = YES;
        return;
    }
    __weak WebRendererEngine *weakSelf = self;
    [store compileContentRuleListForIdentifier:kBlockRuleListIdentifier
                        encodedContentRuleList:kBlockExternalRules
                             completionHandler:^(WKContentRuleList *list, NSError *error) {
        __strong WebRendererEngine *s = weakSelf;
        if (s == nil) return;
        if (list == nil) {
            // Fail closed: silently degrading an explicitly requested security
            // control to "allow" is worse than not showing the wallpaper.
            fprintf(stderr, "WebRenderer: network policy=block failed to compile (%s); "
                            "refusing to load wallpaper\n",
                    error.localizedDescription.UTF8String ?: "unknown error");
            s.networkRulesFailed = YES;
            s.pendingWallpaperLoad = NO;
            return;
        }
        [s.userContentController addContentRuleList:list];
        s.networkRulesReady = YES;
        fprintf(stderr, "WebRenderer: network policy=block (external requests blocked)\n");
        if (s.pendingWallpaperLoad) [s loadWallpaperIfReady];
    }];
}

#pragma mark - Open wallpaper

- (void)openWallpaper:(WRManifest *)manifest {
    _manifest = manifest;
    _didFinishLoad = NO;
    _contentReadyReported = NO;
    _contentReadinessGeneration += 1;
    _propertyApplySerial += 1;
    _propertySnapshotApplied = NO;
    _userPropertySnapshot = nil;
    _userPropertyGeneration = nil;
    [_pendingJS removeAllObjects];

    _schemeHandler.baseDirectory = manifest.workshopDir;
    _schemeHandler.overlayDirectories = _config.assetOverlayDirectories ?: @[];
    [_schemeHandler clearMemoryCache];
    _schemeHandler.loadFromMemory = _config.loadFromMemory;

    _pendingWallpaperLoad = YES;
    [self loadWallpaperIfReady];
}

- (void)loadWallpaperIfReady {
    if (!_pendingWallpaperLoad || _manifest == nil) return;
    if (!_initialMediaPlaybackStateReady) return;
    if (_config.networkPolicy == WRNetworkPolicyBlock && !_networkRulesReady) {
        if (_networkRulesFailed) {
            fprintf(stderr, "WebRenderer: not loading wallpaper — "
                            "network policy=block could not be enforced\n");
            _pendingWallpaperLoad = NO;
            return;
        }
        // The content rule list is still compiling. Loading now would give the
        // page a window of unrestricted egress, so hand the load to the
        // compilation completion handler instead.
        fprintf(stderr, "WebRenderer: deferring load until the network rule list is installed\n");
        return;
    }
    [self loadWallpaperEntry];
}

- (void)loadWallpaperEntry {
    _pendingWallpaperLoad = NO;
    WRManifest *manifest = _manifest;
    if (manifest == nil) return;
    NSString *entry = manifest.entryHTML ?: @"index.html";
    NSURL *url = [NSURL URLWithString:[NSString stringWithFormat:@"we-wallpaper://wallpaper/%@", entry]];
    fprintf(stderr, "WebRenderer: loading %s\n", entry.UTF8String ?: "index.html");
    [_webView loadRequest:[NSURLRequest requestWithURL:url]];
}

#pragma mark - JS helpers

- (NSString *)jsLiteralForObject:(id)obj {
    if (obj == nil || obj == [NSNull null]) return @"null";
    if ([obj isKindOfClass:[NSString class]]) {
        NSData *d = [NSJSONSerialization dataWithJSONObject:@[obj] options:0 error:nil];
        if (d == nil) return @"null";
        NSString *s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
        return [s substringWithRange:NSMakeRange(1, s.length - 2)];
    }
    if ([obj isKindOfClass:[NSNumber class]]) {
        if (strcmp([obj objCType], @encode(BOOL)) == 0 ||
            strcmp([obj objCType], @encode(bool)) == 0) {
            return [obj boolValue] ? @"true" : @"false";
        }
        return [obj description];
    }
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    return d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : @"null";
}

// Evaluate now if loaded, else queue and replay on didFinishNavigation.
- (void)eval:(NSString *)script {
    if (_didFinishLoad) {
        [_webView evaluateJavaScript:script completionHandler:^(id result, NSError *error) {
            (void)result;
            if (error != nil) {
                fprintf(stderr, "WebRenderer: JavaScript control failed: %s\n",
                        error.localizedDescription.UTF8String ?: "unknown error");
            }
        }];
    } else {
        [_pendingJS addObject:script];
    }
}

- (void)flushPendingJS {
    NSArray *pending = [_pendingJS copy];
    [_pendingJS removeAllObjects];
    for (NSString *s in pending) {
        [self eval:s];
    }
}

#pragma mark - WE API

- (void)beginPropertySnapshotApplication {
    if (!_didFinishLoad || _userPropertySnapshot == nil) return;
    _propertySnapshotApplied = NO;
    NSUInteger serial = ++_propertyApplySerial;
    [self applyPropertySnapshotWithSerial:serial attempt:0];
}

- (void)applyPropertySnapshotWithSerial:(NSUInteger)serial attempt:(NSUInteger)attempt {
    (void)attempt;
    if (!_didFinishLoad || serial != _propertyApplySerial || _userPropertySnapshot == nil) return;
    NSString *json = [self jsLiteralForObject:_userPropertySnapshot];
    NSString *generationJSON = [self jsLiteralForObject:_userPropertyGeneration ?: @"snapshot"];
    NSString *script = [NSString stringWithFormat:@"__wr_applySnapshot(%@, %@);", json, generationJSON];
    __weak WebRendererEngine *weakSelf = self;
    [_webView evaluateJavaScript:script completionHandler:^(id result, NSError *error) {
        __strong WebRendererEngine *self = weakSelf;
        if (self == nil || serial != self.propertyApplySerial || !self.didFinishLoad) return;
        if (error != nil) {
            fprintf(stderr, "WebRenderer: property snapshot failed: %s\n",
                    error.localizedDescription.UTF8String ?: "unknown error");
            return;
        }
        NSInteger status = [result respondsToSelector:@selector(integerValue)] ? [result integerValue] : 0;
        if (status == 1) {
            self.propertySnapshotApplied = YES;
            return;
        }
        if (status < 0) {
            fprintf(stderr, "WebRenderer: wallpaper rejected property snapshot generation=%s\n",
                    self.userPropertyGeneration.UTF8String ?: "snapshot");
            return;
        }
        // The document-start shim retains the snapshot and applies it from the
        // wallpaperPropertyListener setter when the page installs its listener.
    }];
}

- (void)applyUserProperty:(NSString *)key value:(id)value {
    if (_userPropertySnapshot == nil) {
        NSDictionary *base = _manifest.userProperties ?: @{};
        _userPropertySnapshot = [base mutableCopy];
        _userPropertyGeneration = @"manifest";
    }
    _userPropertySnapshot[key] = value;
    if (!_didFinishLoad) return;
    if (!_propertySnapshotApplied) {
        [self beginPropertySnapshotApplication];
        return;
    }
    NSString *payload = [self jsLiteralForObject:@{key: value}];
    [self eval:[NSString stringWithFormat:@"__wr_applyProps(%@);", payload]];
}

- (void)applyUserProperties:(NSDictionary<NSString *,id> *)properties generation:(NSString *)generation {
    _userPropertySnapshot = [properties mutableCopy];
    _userPropertyGeneration = generation.length > 0 ? [generation copy] : @"snapshot";
    [self beginPropertySnapshotApplication];
}

- (void)setPaused:(BOOL)paused {
    _paused = paused;
    [self eval:[NSString stringWithFormat:@"__wr_setPaused(%@);", paused ? @"true" : @"false"]];
    [self reconcileAudioSpectrum];
    if (self.audioSpectrumDemandHandler != nil) {
        self.audioSpectrumDemandHandler(_audioListenerDemand && !paused);
    }
}

- (void)setVolume:(float)volume {
    _volume = volume;
    [self applyUserProperty:@"audio" value:@{@"value": @(volume)}];
    BOOL effectiveMuted = _muted || volume <= 0.0f;
    [self eval:[NSString stringWithFormat:@"__wr_applyMute(%@);", effectiveMuted ? @"true" : @"false"]];
}

- (void)setMuted:(BOOL)muted {
    _muted = muted;
    BOOL effectiveMuted = muted || _volume <= 0.0f;
    [self eval:[NSString stringWithFormat:@"__wr_applyMute(%@);", effectiveMuted ? @"true" : @"false"]];
}

- (void)setHostMediaPlaybackSuspended:(BOOL)suspended
                            completion:(void (^)(void))completion {
    _desiredHostMediaPlaybackSuspended = suspended;
    [_hostMediaPlaybackStates addObject:@(suspended)];
    [_hostMediaPlaybackCompletions addObject:
        completion != nil ? [completion copy] : [^{} copy]];
    [self startNextHostMediaPlaybackRequest];
}

- (void)startNextHostMediaPlaybackRequest {
    if (_hostMediaPlaybackRequestInFlight || _hostMediaPlaybackStates.count == 0) return;
    _hostMediaPlaybackRequestInFlight = YES;
    const BOOL suspended = _hostMediaPlaybackStates.firstObject.boolValue;
    void (^completion)(void) = _hostMediaPlaybackCompletions.firstObject;
    [_hostMediaPlaybackStates removeObjectAtIndex:0];
    [_hostMediaPlaybackCompletions removeObjectAtIndex:0];

    __weak WebRendererEngine *weakSelf = self;
    [_webView setAllMediaPlaybackSuspended:suspended completionHandler:^{
        WebRendererEngine *strongSelf = weakSelf;
        if (strongSelf == nil) return;
        strongSelf.hostMediaPlaybackSuspended = suspended;
        strongSelf.hostMediaPlaybackRequestInFlight = NO;
        if (completion) completion();
        [strongSelf startNextHostMediaPlaybackRequest];
    }];
}

- (void)setFrameRate:(int)fps {
    [self eval:[NSString stringWithFormat:@"__wr_setFps(%d);", fps]];
}

#pragma mark - Audio spectrum

- (void)startAudioSpectrum {
    _audioSpectrumRequested = YES;
    [self reconcileAudioSpectrum];
}

- (void)reconcileAudioSpectrum {
    BOOL needed = _audioSpectrumRequested && _config.enableAudioSpectrum &&
                  _audioListenerDemand && !_paused;
    if (!needed) {
        if (_audioSpectrumStarted) {
            [_audioTimer invalidate];
            _audioTimer = nil;
            [_audioTap stop];
            _audioSpectrumStarted = NO;
        }
        return;
    }
    if (_audioSpectrumStarted) return;
    _audioSpectrumStarted = YES;
    __weak WebRendererEngine *weakSelf = self;
    [_audioTap startWithCompletion:^(BOOL ok, NSString *msg) {
        __strong WebRendererEngine *s = weakSelf;
        if (!s) return;
        if (!ok) {
            s->_audioSpectrumStarted = NO;
            fprintf(stderr, "WebRenderer: audio spectrum disabled (%s)\n",
                    msg ? msg.UTF8String : "unknown");
            return;
        }
        if (getenv("WR_DEBUG")) {
            fprintf(stderr, "WebRenderer: audio spectrum tap running\n");
        }
        // The timer block must capture weakSelf, not the strong `s`: the run
        // loop retains the timer and the timer retains the block, so a strong
        // capture made engine and timer keep each other alive forever.
        // -timerWithTimeInterval: is unscheduled, so the addTimer: below is the
        // one and only registration. -scheduledTimerWithTimeInterval: had
        // already scheduled it in the default mode, which made the addTimer:
        // register the same timer twice instead of upgrading it to common modes.
        NSTimer *timer = [NSTimer timerWithTimeInterval:1.0/30.0 repeats:YES
            block:^(NSTimer *t) { (void)t; [weakSelf tickAudio]; }];
        s->_audioTimer = timer;
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    }];
}

- (void)stopAudioSpectrum {
    _audioSpectrumRequested = NO;
    [_audioTimer invalidate];
    _audioTimer = nil;
    [_audioTap stop];
    _audioSpectrumStarted = NO;
}

- (void)tickAudio {
    float bins[64];
    if (![_audioTap copySpectrum:bins count:64]) return;

    // WE contract: 128 floats — [0..63]=L, [64..127]=R. Mono source duplicated
    // into both halves (matches OWE's WebViewer with wavsen's mono output).
    NSMutableString *arr = [NSMutableString stringWithCapacity:128 * 8];
    [arr appendString:@"__wr_pushAudio(["];
    char buf[32];
    for (int i = 0; i < 64; ++i) {
        if (i) [arr appendString:@","];
        snprintf(buf, sizeof(buf), "%.4f", bins[i]);
        [arr appendFormat:@"%s", buf];
    }
    for (int i = 0; i < 64; ++i) {
        [arr appendString:@","];
        snprintf(buf, sizeof(buf), "%.4f", bins[i]);
        [arr appendFormat:@"%s", buf];
    }
    [arr appendString:@"]);"];
    [_webView evaluateJavaScript:arr completionHandler:nil];
}

- (void)pushAudioSpectrum:(NSArray<NSNumber *> *)spectrum {
    if (!_audioListenerDemand || _paused || spectrum.count != 128) return;
    // Spectrum frames are only meaningful live. Before the page has loaded there
    // is nothing to receive them, and queueing them would just replay stale
    // audio once it does.
    if (!_didFinishLoad) return;
    // Pass the samples as a structured argument rather than serialising them to
    // JSON and pasting that into a freshly built ~800-character source string
    // 30 times a second — roughly 3900 NSString allocations per second, plus a
    // full parse/compile of a new script for each frame. The body is constant
    // now; only the argument changes.
    static NSString *const kPushAudioBody = @"window.__wr_pushAudio(d);";
    [_webView callAsyncJavaScript:kPushAudioBody
                        arguments:@{ @"d": spectrum }
                          inFrame:nil
                   inContentWorld:WKContentWorld.pageWorld
                completionHandler:nil];
}

#pragma mark - Snapshot

- (void)takeSnapshotToPath:(NSString *)path
                completion:(void (^)(BOOL ok))completion {
    if (path.length == 0) {
        if (completion) completion(NO);
        return;
    }
    // A page that has not finished loading would snapshot as blank, which is a
    // worse desktop picture than leaving the previous one alone.
    if (!_didFinishLoad) {
        if (completion) completion(NO);
        return;
    }
    WKSnapshotConfiguration *config = [[WKSnapshotConfiguration alloc] init];
    // nil rect means the whole visible bounds; afterScreenUpdates keeps the
    // still in step with what the compositor last showed.
    config.afterScreenUpdates = YES;
    [_webView takeSnapshotWithConfiguration:config
                         completionHandler:^(NSImage *image, NSError *error) {
        if (image == nil || error != nil) {
            if (error != nil) {
                fprintf(stderr, "WebRenderer: snapshot failed: %s\n",
                        error.localizedDescription.UTF8String ?: "unknown");
            }
            if (completion) completion(NO);
            return;
        }
        CGImageRef cgImage = [image CGImageForProposedRect:NULL context:nil hints:nil];
        const BOOL ok = WREncodeSnapshot(cgImage, path);
        if (completion) completion(ok);
    }];
}

#pragma mark - WKNavigationDelegate

- (void)probeContentReadinessForGeneration:(NSUInteger)generation
                                  attempts:(NSInteger)attempts {
    if (generation != _contentReadinessGeneration || !_didFinishLoad ||
        _contentReadyReported) return;

    // didFinishNavigation precedes the first compositor commit. A tiny snapshot
    // is a public WebKit synchronization point that proves the page process has
    // produced pixels without allocating a full-display bitmap.
    WKSnapshotConfiguration *snapshot = [WKSnapshotConfiguration new];
    const NSRect bounds = self.webView.bounds;
    snapshot.rect = NSMakeRect(NSMidX(bounds), NSMidY(bounds), 1.0, 1.0);
    snapshot.afterScreenUpdates = YES;
    __weak WebRendererEngine *weakSelf = self;
    [self.webView takeSnapshotWithConfiguration:snapshot
                              completionHandler:^(NSImage *image, NSError *error) {
        (void)error;
        WebRendererEngine *strongSelf = weakSelf;
        if (strongSelf == nil || generation != strongSelf.contentReadinessGeneration ||
            !strongSelf.didFinishLoad || strongSelf.contentReadyReported) return;
        if (image != nil) {
            strongSelf.contentReadyReported = YES;
            if (strongSelf.contentReadyHandler) strongSelf.contentReadyHandler();
            return;
        }
        if (attempts <= 0) return;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            [strongSelf probeContentReadinessForGeneration:generation
                                                  attempts:attempts - 1];
        });
    }];
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    (void)navigation;
    _didFinishLoad = YES;
    fprintf(stderr, "WebRenderer: navigation finished url=%s; injecting user properties\n",
            webView.URL.absoluteString.UTF8String ?: "unknown");

    if (_userPropertySnapshot == nil) {
        _userPropertySnapshot = [(_manifest.userProperties ?: @{}) mutableCopy];
        _userPropertyGeneration = @"manifest";
    }
    if (_config.initialVolume < 1.0f || _muted) {
        _userPropertySnapshot[@"audio"] = @{ @"value": @(_volume) };
        BOOL effectiveMuted = _muted || _volume <= 0.0f;
        [self eval:[NSString stringWithFormat:@"__wr_applyMute(%@);", effectiveMuted ? @"true" : @"false"]];
    }
    [self beginPropertySnapshotApplication];
    if (_config.frameRate > 0 && _config.frameRate < 60) {
        [self setFrameRate:_config.frameRate];
    }
    [self flushPendingJS];

    const NSUInteger generation = _contentReadinessGeneration;
    __weak WebRendererEngine *weakSelf = self;
    void (^beginReadinessProbe)(void) = ^{
        WebRendererEngine *strongSelf = weakSelf;
        if (strongSelf == nil || generation != strongSelf.contentReadinessGeneration ||
            !strongSelf.didFinishLoad) return;
        [strongSelf probeContentReadinessForGeneration:generation attempts:40];
    };
    if (_desiredHostMediaPlaybackSuspended) {
        // Suspending the initially empty WKWebView before navigation closes the
        // startup race, but a navigation creates a new page. Reassert the host
        // barrier for that concrete document and do not publish `prepared`
        // until WebKit acknowledges it.
        [self setHostMediaPlaybackSuspended:YES completion:beginReadinessProbe];
    } else {
        beginReadinessProbe();
    }
}

- (void)webView:(WKWebView *)webView
        decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction
                        decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    (void)webView;
    NSString *scheme = navigationAction.request.URL.scheme.lowercaseString;
    // Allow our scheme + about: only; cancel external page-level nav so a
    // wallpaper can't yank the window off to the web. Sub-resources unaffected.
    // file: and data: are deliberately NOT allowed in the MAIN frame: a
    // top-level navigation to either hands untrusted page script a document on
    // a different security origin than the we-wallpaper: sandbox it is
    // supposed to stay inside.
    if ([scheme isEqualToString:@"we-wallpaper"] || [scheme isEqualToString:@"about"]) {
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    // This delegate also fires for IFRAME navigations, and Wallpaper Engine web
    // wallpapers routinely isolate widgets in <iframe src="data:text/html,...">
    // — blocking those just leaves the widget blank without buying anything,
    // since the sub-frame document does not become the wallpaper's own origin.
    // targetFrame is nil for a navigation that would open a new window, which
    // is explicitly NOT a sub-frame and stays denied. file: is denied in every
    // frame: it would reach the real filesystem outside the wallpaper dir.
    WKFrameInfo *targetFrame = navigationAction.targetFrame;
    BOOL isSubframe = (targetFrame != nil && !targetFrame.isMainFrame);
    if (isSubframe && ([scheme isEqualToString:@"data"] || [scheme isEqualToString:@"blob"])) {
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    fprintf(stderr, "WebRenderer: blocked external navigation to %s\n",
            navigationAction.request.URL.absoluteString.UTF8String ?: "");
    decisionHandler(WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)webView; (void)navigation;
    _didFinishLoad = NO;
    _propertyApplySerial += 1;
    fprintf(stderr, "WebRenderer: navigation failed: %s\n", error.localizedDescription.UTF8String ?: "");
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)webView; (void)navigation;
    _didFinishLoad = NO;
    _propertyApplySerial += 1;
    fprintf(stderr, "WebRenderer: provisional load failed: %s\n", error.localizedDescription.UTF8String ?: "");
}

- (void)userContentController:(WKUserContentController *)ucc didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)ucc;
    if ([message.name isEqualToString:@"wrProperties"]) {
        NSDictionary *body = WRDictionaryValue(message.body);
        NSString *generation = WRStringValue(body[@"generation"]);
        NSNumber *count = WRNumberValue(body[@"count"]);
        fprintf(stderr, "WebRenderer: applied property snapshot generation=%s count=%ld\n",
                generation.UTF8String ?: "unknown",
                count != nil ? (long)count.integerValue : -1L);
        return;
    }
    if ([message.name isEqualToString:@"wrAudioDemand"]) {
        NSDictionary *body = WRDictionaryValue(message.body);
        BOOL needed = WRNumberValue(body[@"needed"]).boolValue;
        if (_audioListenerDemand != needed) {
            _audioListenerDemand = needed;
            [self reconcileAudioSpectrum];
            if (self.audioSpectrumDemandHandler != nil) {
                self.audioSpectrumDemandHandler(needed && !_paused);
            }
        }
        return;
    }
    if ([message.name isEqualToString:@"wrNetwork"]) {
        NSDictionary *body = WRDictionaryValue(message.body);
        NSString *url = WRStringValue(body[@"url"]);
        NSString *kind = WRStringValue(body[@"kind"]) ?: @"resource";
        if (url.length == 0) return;
        if (_networkObservationCount > kMaxNetworkObservations) return;
        _networkObservationCount += 1;
        if (_networkObservationCount > kMaxNetworkObservations) {
            fprintf(stderr, "WebRenderer: [network-observe] reached %lu distinct requests; "
                            "further reports suppressed\n",
                    (unsigned long)kMaxNetworkObservations);
            return;
        }
        // The page can postMessage to this handler directly, so neither field
        // can be trusted to be short.
        if (url.length > kMaxObservedURLLength) {
            url = [[url substringToIndex:kMaxObservedURLLength] stringByAppendingString:@"..."];
        }
        if (kind.length > 32) kind = [kind substringToIndex:32];
        fprintf(stderr, "WebRenderer: [network-observe] would block %s request: %s\n",
                kind.UTF8String ?: "resource", url.UTF8String ?: "");
        return;
    }
    if (![message.name isEqualToString:@"wrConsole"]) return;
    NSDictionary *body = WRDictionaryValue(message.body);
    NSString *type = WRStringValue(body[@"type"]) ?: @"log";
    NSString *text = WRStringValue(body[@"message"]) ?: @"";
    fprintf(stderr, "WebRenderer [%s] %s\n", type.UTF8String ?: "log", text.UTF8String ?: "");
}

@end

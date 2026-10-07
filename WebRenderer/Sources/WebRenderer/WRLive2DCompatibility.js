// Copyright © 2026 王孝慈. All rights reserved.

const __wrLive2DCompatibility = (() => {
  const states = new WeakMap();
  const prototype = CubismRenderer_WebGL.prototype;

  function dispose(renderer) {
    const state = states.get(renderer);
    if (!state) return;
    states.delete(renderer);
    for (const entry of state.entries) {
      if (!entry) continue;
      for (const key of ["vertex", "uv", "index"]) {
        if (entry[key]) state.gl.deleteBuffer(entry[key]);
      }
    }
  }

  function sameValues(a, b) {
    if (!a || a.constructor !== b.constructor || a.length !== b.length) return false;
    for (let i = 0; i < b.length; i++) {
      if (a[i] !== b[i]) return false;
    }
    return true;
  }

  function uploadStatic(gl, entry, key, target, values) {
    const snapshot = key + "Snapshot";
    if (sameValues(entry[snapshot], values)) return;
    gl.bindBuffer(target, entry[key]);
    gl.bufferData(target, values, gl.STATIC_DRAW);
    entry[snapshot] = new values.constructor(values);
  }

  function prepare(renderer, state) {
    const model = renderer.getModel();
    const gl = state.gl;
    const count = model.getDrawableCount();
    while (state.entries.length > count) {
      const entry = state.entries.pop();
      if (entry) for (const key of ["vertex", "uv", "index"]) gl.deleteBuffer(entry[key]);
    }
    state.lookup = new WeakMap();
    const needed = new Set();
    for (let i = 0; i < count; i++) {
      if (model.getDrawableDynamicFlagIsVisible(i)) needed.add(i);
    }
    const clips = renderer._clippingManager?._clippingContextListForMask || [];
    for (const clip of clips) {
      for (let i = 0; i < clip._clippingIdCount; i++) needed.add(clip._clippingIdList[i]);
    }
    for (const i of needed) {
      if (i < 0 || i >= count) continue;
      const vertices = model.getDrawableVertices(i);
      const indices = model.getDrawableVertexIndices(i);
      const uvs = model.getDrawableVertexUvs(i);
      let entry = state.entries[i];
      if (!entry) {
        entry = state.entries[i] = { vertex: null, uv: null, index: null };
        for (const key of ["vertex", "uv", "index"]) {
          entry[key] = gl.createBuffer();
          if (!entry[key]) throw new Error("Live2D geometry allocation failed");
        }
      }
      uploadStatic(gl, entry, "index", gl.ELEMENT_ARRAY_BUFFER, indices);
      uploadStatic(gl, entry, "uv", gl.ARRAY_BUFFER, uvs);
      gl.bindBuffer(gl.ARRAY_BUFFER, entry.vertex);
      gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);
      entry.vertices = vertices;
      entry.indices = indices;
      entry.uvs = uvs;
      entry.__wrUploaded = true;
      state.lookup.set(vertices, entry);
    }
  }

  for (const name of ["initialize", "startUp", "release"]) {
    const original = prototype[name];
    prototype[name] = function(...args) {
      dispose(this);
      return original.apply(this, args);
    };
  }

  const drawModel = prototype.doDrawModel;
  prototype.doDrawModel = function(...args) {
    const model = this.getModel();
    let state = states.get(this);
    if (state && (state.model !== model || state.gl !== this.gl)) {
      dispose(this);
      state = null;
    }
    if (!state) {
      state = { model, gl: this.gl, entries: [], lookup: new WeakMap(), active: false, disabled: false };
      states.set(this, state);
    }
    if (!state.disabled && !state.gl.isContextLost()) {
      try {
        prepare(this, state);
        state.active = true;
      } catch (error) {
        dispose(this);
        state = { ...state, entries: [], lookup: new WeakMap(), active: false, disabled: true };
        states.set(this, state);
        console.warn("WebRenderer: Live2D geometry compatibility disabled", error);
      }
    }
    try {
      return drawModel.apply(this, args);
    } finally {
      state.active = false;
    }
  };

  const drawMesh = prototype.drawMesh;
  prototype.drawMesh = function(...args) {
    const state = states.get(this);
    const entry = state?.active ? state.lookup.get(args[4]) : null;
    if (!entry || entry.indices !== args[3] || entry.uvs !== args[5]) {
      return drawMesh.apply(this, args);
    }
    const previous = this._bufferData;
    this._bufferData = entry;
    try {
      return drawMesh.apply(this, args);
    } finally {
      this._bufferData = previous;
    }
  };

  function attachApplication(app) {
    const renderer = app.renderer;
    let media = null;
    let disposed = false;
    function unwatch() {
      if (media) media.removeEventListener("change", update);
      media = null;
    }
    function update() {
      if (disposed) return;
      const value = window.devicePixelRatio;
      const ratio = Number.isFinite(value) && value > 0 ? value : 1;
      if (renderer.resolution !== ratio) {
        renderer.resolution = ratio;
        renderer.resize(renderer.screen.width, renderer.screen.height);
      }
      unwatch();
      media = window.matchMedia(`(resolution: ${ratio}dppx)`);
      media.addEventListener("change", update);
    }
    function cleanup() {
      if (disposed) return;
      disposed = true;
      unwatch();
      window.removeEventListener("resize", update);
      window.removeEventListener("pagehide", pageHide);
    }
    function pageHide(event) {
      if (!event.persisted) cleanup();
    }
    const destroy = app.destroy;
    app.destroy = function(...args) {
      cleanup();
      return destroy.apply(this, args);
    };
    window.addEventListener("resize", update);
    window.addEventListener("pagehide", pageHide);
    update();
  }

  return { attachApplication };
})();

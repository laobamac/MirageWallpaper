// Copyright © 2026 王孝慈. All rights reserved.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import vm from 'node:vm';

const source = readFileSync(new URL('../Sources/WebRenderer/WRLive2DCompatibility.js', import.meta.url), 'utf8');

function harness() {
  const events = [];
  let serial = 0;
  let lost = false;
  const bound = new Map();
  const buffers = new Set();
  const gl = {
    ARRAY_BUFFER: 1, ELEMENT_ARRAY_BUFFER: 2, STATIC_DRAW: 3, DYNAMIC_DRAW: 4,
    createBuffer() { const b = { id: ++serial }; buffers.add(b); events.push(['create', b]); return b; },
    deleteBuffer(b) { buffers.delete(b); events.push(['delete', b]); },
    bindBuffer(target, buffer) { bound.set(target, buffer); },
    bufferData(target, data, usage) { events.push(['upload', bound.get(target), Array.from(data), usage]); },
    isContextLost() { return lost; }
  };
  const drawable = (visible = true) => ({ visible, vertices: new Float32Array([0, 0, 1, 0, 0, 1]), indices: new Uint16Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]) });
  const drawables = [drawable(), drawable(false), drawable(false)];
  const model = {
    getDrawableCount: () => drawables.length,
    getDrawableDynamicFlagIsVisible: i => drawables[i].visible,
    getDrawableVertices: i => drawables[i].vertices,
    getDrawableVertexIndices: i => drawables[i].indices,
    getDrawableVertexUvs: i => drawables[i].uvs
  };
  class Renderer {
    constructor() { this.gl = gl; this.model = model; this._bufferData = {}; this._clippingManager = { _clippingContextListForMask: [{ _clippingIdList: [1], _clippingIdCount: 1 }] }; }
    getModel() { return this.model; }
    initialize(m) { this.model = m; }
    startUp(g) { this.gl = g; }
    release() { events.push(['release']); }
    drawMesh(texture, count, verticesCount, indices, vertices, uvs) {
      const b = this._bufferData;
      if (!b.__wrUploaded) events.push(['fallback']);
      events.push(['draw', vertices, b]);
      if (this.throwDraw) throw new Error('draw failure');
    }
    doDrawModel() {
      const draw = i => this.drawMesh(0, 3, 3, this.model.getDrawableVertexIndices(i), this.model.getDrawableVertices(i), this.model.getDrawableVertexUvs(i));
      draw(1);
      for (let i = 0; i < this.model.getDrawableCount(); i++) if (this.model.getDrawableDynamicFlagIsVisible(i)) draw(i);
    }
  }
  const listeners = new Map();
  const mediaQueries = [];
  const window = {
    devicePixelRatio: 2,
    addEventListener(name, fn) { if (!listeners.has(name)) listeners.set(name, new Set()); listeners.get(name).add(fn); },
    removeEventListener(name, fn) { listeners.get(name)?.delete(fn); },
    matchMedia(query) {
      const m = { query, callbacks: new Set(), addEventListener(_, f) { this.callbacks.add(f); }, removeEventListener(_, f) { this.callbacks.delete(f); } };
      mediaQueries.push(m); return m;
    }
  };
  const ctx = vm.createContext({ CubismRenderer_WebGL: Renderer, window, console: { warn: (...args) => events.push(['warning', ...args]) } });
  vm.runInContext(source + '\nglobalThis.compat = __wrLive2DCompatibility;', ctx);
  const renderer = new Renderer();
  const resize = [];
  const app = { renderer: { resolution: 1, screen: { width: 1920, height: 1080 }, resize(w, h) { resize.push([w, h, this.resolution]); } }, destroy() { this.destroyed = true; } };
  const emit = name => { for (const f of [...(listeners.get(name) || [])]) f({ persisted: false }); };
  return { renderer, gl, events, buffers, drawables, drawable, model, compat: ctx.compat, window, listeners, mediaQueries, app, resize, emit, setLost: x => lost = x };
}

test('uploads finish before mask and visible draws, without changing their order', () => {
  const h = harness(); h.renderer.doDrawModel();
  const firstDraw = h.events.findIndex(e => e[0] === 'draw');
  assert.equal(h.events.slice(firstDraw).some(e => e[0] === 'upload'), false);
  assert.equal(h.events.filter(e => e[0] === 'upload').length, 6);
  assert.deepEqual(h.events.filter(e => e[0] === 'draw').map(e => e[1]), [h.drawables[1].vertices, h.drawables[0].vertices]);
  assert.equal(h.events.some(e => e[0] === 'fallback'), false);
  assert.equal(h.buffers.size, 6);
});

test('static UV and index buffers persist; dynamic vertices update every frame', () => {
  const h = harness(); h.renderer.doDrawModel(); h.events.length = 0;
  h.drawables[0].vertices[0] = 9; h.renderer.doDrawModel();
  const uploads = h.events.filter(e => e[0] === 'upload');
  assert.equal(uploads.length, 2); assert(uploads.every(e => e[3] === h.gl.DYNAMIC_DRAW));
  assert(uploads.some(e => e[2][0] === 9)); assert.equal(h.buffers.size, 6);
});

test('static buffers detect in-place edits and replacement with different sizes', () => {
  const h = harness(); h.renderer.doDrawModel(); h.events.length = 0;
  h.drawables[0].uvs[0] = 0.75; h.drawables[0].indices[0] = 2;
  h.renderer.doDrawModel(); assert.equal(h.events.filter(e => e[0] === 'upload' && e[3] === h.gl.STATIC_DRAW).length, 2);
  h.events.length = 0; h.drawables[0].indices = new Uint16Array([0, 1, 2, 1, 2, 0]);
  h.renderer.doDrawModel(); assert.equal(h.events.filter(e => e[0] === 'upload' && e[3] === h.gl.STATIC_DRAW).length, 1);
});

test('newly visible geometry is initialized and reappearing geometry is refreshed', () => {
  const h = harness(); h.renderer.doDrawModel(); h.drawables[2].visible = true; h.events.length = 0;
  h.renderer.doDrawModel(); assert.equal(h.buffers.size, 9); assert.equal(h.events.filter(e => e[0] === 'upload').length, 5);
  h.drawables[2].visible = false; h.renderer.doDrawModel(); h.drawables[2].vertices[0] = 19;
  h.drawables[2].visible = true; h.events.length = 0; h.renderer.doDrawModel();
  assert(h.events.some(e => e[0] === 'upload' && e[2][0] === 19));
});

test('renderers have isolated caches and steady frames do not accumulate buffers', () => {
  const h = harness(); const other = new h.renderer.constructor();
  h.renderer.doDrawModel(); other.doDrawModel(); assert.equal(h.buffers.size, 12);
  for (let i = 0; i < 200; i++) {
    h.events.length = 0; h.renderer.doDrawModel(); other.doDrawModel();
    assert.equal(h.buffers.size, 12); assert.equal(h.events.some(e => e[0] === 'create'), false);
  }
  h.renderer.release(); assert.equal(h.buffers.size, 6);
  other.doDrawModel(); other.release(); assert.equal(h.buffers.size, 0);
});

test('context restart and release dispose all compatibility buffers', () => {
  const h = harness(); h.renderer.doDrawModel(); h.renderer.startUp(h.gl); assert.equal(h.buffers.size, 0);
  h.renderer.doDrawModel(); assert.equal(h.buffers.size, 6); h.renderer.release(); assert.equal(h.buffers.size, 0);
  h.renderer.release(); assert.equal(h.buffers.size, 0);
});

test('initialize and model changes invalidate geometry caches', () => {
  const h = harness(); h.renderer.doDrawModel(); h.renderer.initialize(h.model); assert.equal(h.buffers.size, 0);
  h.renderer.doDrawModel(); const previous = [...h.buffers]; h.renderer.model = { ...h.model }; h.renderer.doDrawModel();
  assert(previous.every(b => !h.buffers.has(b))); assert.equal(h.buffers.size, 6);
});

test('removed drawables release their cached buffers', () => {
  const h = harness(); h.drawables[2].visible = true; h.renderer.doDrawModel(); assert.equal(h.buffers.size, 9);
  h.drawables.pop(); h.renderer.doDrawModel(); assert.equal(h.buffers.size, 6);
});

test('direct draws and lost contexts fall back without reusing stale geometry', () => {
  const h = harness(); h.renderer.doDrawModel(); h.events.length = 0;
  const d = h.drawables[0]; h.renderer.drawMesh(0, 3, 3, d.indices, d.vertices, d.uvs);
  assert(h.events.some(e => e[0] === 'fallback')); h.setLost(true); h.events.length = 0;
  h.renderer.doDrawModel(); assert.equal(h.events.some(e => e[0] === 'upload'), false);
  assert(h.events.some(e => e[0] === 'fallback'));
});

test('allocation failure frees partial buffers and disables optimization until restart', () => {
  const h = harness(); const create = h.gl.createBuffer; let count = 0;
  h.gl.createBuffer = () => ++count === 2 ? null : create(); h.renderer.doDrawModel();
  assert.equal(h.buffers.size, 0); assert(h.events.some(e => e[0] === 'fallback'));
  h.gl.createBuffer = create; h.events.length = 0; h.renderer.doDrawModel(); assert.equal(h.buffers.size, 0);
  h.renderer.startUp(h.gl); h.renderer.doDrawModel(); assert.equal(h.buffers.size, 6);
});

test('draw exceptions restore the original buffer set and active state', () => {
  const h = harness(); const fallback = h.renderer._bufferData; h.renderer.throwDraw = true;
  assert.throws(() => h.renderer.doDrawModel(), /draw failure/); assert.equal(h.renderer._bufferData, fallback);
  h.renderer.throwDraw = false; h.renderer.doDrawModel(); assert.equal(h.renderer._bufferData, fallback);
});

test('DPR changes resize the backing store without changing logical dimensions', () => {
  const h = harness(); h.compat.attachApplication(h.app);
  assert.deepEqual(h.resize, [[1920, 1080, 2]]); h.window.devicePixelRatio = 1;
  for (const f of [...h.mediaQueries.at(-1).callbacks]) f();
  assert.deepEqual(h.resize.at(-1), [1920, 1080, 1]); assert.equal(h.mediaQueries[0].callbacks.size, 0);
  h.window.devicePixelRatio = 1.5; h.emit('resize'); assert.deepEqual(h.resize.at(-1), [1920, 1080, 1.5]);
  h.emit('resize'); assert.equal(h.resize.length, 3);
  h.window.devicePixelRatio = NaN; h.emit('resize'); assert.equal(h.app.renderer.resolution, 1);
});

test('matching DPR does not double-scale, and application teardown removes listeners', () => {
  const h = harness(); h.app.renderer.resolution = 2; h.compat.attachApplication(h.app); assert.equal(h.resize.length, 0);
  h.app.destroy(); assert.equal(h.app.destroyed, true); assert.equal(h.listeners.get('resize').size, 0);
  assert.equal(h.listeners.get('pagehide').size, 0); assert(h.mediaQueries.every(m => m.callbacks.size === 0));
  h.window.devicePixelRatio = 1; h.emit('resize'); assert.equal(h.resize.length, 0);
});

test('page cache suspension retains DPR observers for restoration', () => {
  const h = harness(); h.compat.attachApplication(h.app);
  for (const f of h.listeners.get('pagehide')) f({ persisted: true });
  h.window.devicePixelRatio = 1; h.emit('resize');
  assert.deepEqual(h.resize.at(-1), [1920, 1080, 1]);
  h.app.destroy(); assert(h.mediaQueries.every(m => m.callbacks.size === 0));
});

test('page teardown removes DPR observers', () => {
  const h = harness(); h.compat.attachApplication(h.app); h.emit('pagehide');
  assert.equal(h.listeners.get('resize').size, 0); assert(h.mediaQueries.every(m => m.callbacks.size === 0));
});

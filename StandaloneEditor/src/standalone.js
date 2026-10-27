// Mirage Wallpaper
// Copyright © 2026 王孝慈. All rights reserved.
// macOS integration for the original editor / 原版编辑器的 macOS 适配。
(() => {
    const session = window.mirageEditorSession;
    if (!session) return;
    const settingsKey = 'mirage.editor.settings.v1';
    document.documentElement.lang = session.language === 'zh-chs' ? 'zh-Hans' : session.language === 'zh-cht' ? 'zh-Hant' : session.language;
    async function watchAppearance() {
        let previous = '';
        while (true) {
            try {
                const response = await fetch(session.bridge.endpoint + '/appearance', {
                    method: 'POST', headers: {'Authorization': 'Bearer ' + session.bridge.token, 'X-Mirage-Appearance': previous}
                });
                if (!response.ok) throw new Error(String(response.status));
                const result = await response.json();
                if (result.appearance === 'dark' || result.appearance === 'light') {
                    previous = result.appearance;
                    document.documentElement.dataset.mirageAppearance = previous;
                }
            } catch (_) { await new Promise(resolve => setTimeout(resolve, 10000)); }
        }
    }
    if (location.hash.startsWith('#/editor')) watchAppearance();
    let choosingProject = false;
    window.mirageEditor = {
        createPreview(canvas, requestSize) {
            let gl, program, texture, handle, importedHandle, frame, disposed = false, visible = true;
            const vertex = '#version 300 es\nout vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);uv=vec2(p.x,1.0-p.y);}';
            const fragment = '#version 300 es\nprecision highp float;uniform sampler2D image;in vec2 uv;out vec4 color;void main(){color=vec4(texture(image,uv).rgb,1);}';
            let width, height;
            function stop() {
                if (frame !== undefined) cancelAnimationFrame(frame);
                frame = undefined;
            }
            function release() {
                stop();
                if (gl && !gl.isContextLost()) {
                    gl.useProgram(null);
                    gl.bindTexture(gl.TEXTURE_2D, null);
                    if (texture) gl.deleteTexture(texture);
                    if (program) gl.deleteProgram(program);
                }
                texture = program = importedHandle = width = height = undefined;
            }
            function prepare() {
                if (!gl) gl = canvas.getContext('webgl2', {alpha: false, depth: false, stencil: false, antialias: false});
                if (!gl || gl.isContextLost()) return false;
                if (program && handle === importedHandle && width === canvas.width && height === canvas.height) return true;
                if (!program) {
                    const shaders = [];
                    try {
                        program = gl.createProgram();
                        for (const [type, source] of [[gl.VERTEX_SHADER, vertex], [gl.FRAGMENT_SHADER, fragment]]) {
                            const shader = gl.createShader(type);
                            shaders.push(shader);
                            gl.shaderSource(shader, source);
                            gl.compileShader(shader);
                            if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
                            gl.attachShader(program, shader);
                        }
                        gl.linkProgram(program);
                        if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
                        gl.useProgram(program);
                        gl.uniform1i(gl.getUniformLocation(program, 'image'), 0);
                        gl.disable(gl.DEPTH_TEST);
                        gl.disable(gl.BLEND);
                    } catch (error) {
                        release();
                        window.ui.logError('Mirage preview: ' + error.message);
                        return false;
                    } finally { for (const shader of shaders) gl.deleteShader(shader); }
                }
                if (handle !== importedHandle) {
                    if (texture) gl.deleteTexture(texture);
                    texture = gl.createTexture();
                    gl.activeTexture(gl.TEXTURE0);
                    gl.bindTexture(gl.TEXTURE_2D, texture);
                    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
                    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
                    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
                    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
                    gl.texImage2DShared(handle);
                    importedHandle = handle;
                }
                gl.viewport(0, 0, canvas.width, canvas.height);
                width = canvas.width;
                height = canvas.height;
                return true;
            }
            function draw() {
                frame = undefined;
                if (disposed || !visible || document.hidden || !handle || !canvas.width || !canvas.height) return;
                if (!prepare()) return;
                gl.drawArrays(gl.TRIANGLES, 0, 3);
                frame = requestAnimationFrame(draw);
            }
            function schedule() {
                stop();
                if (!disposed && visible && !document.hidden && handle) frame = requestAnimationFrame(draw);
            }
            function lost(event) { event.preventDefault(); release(); }
            function restored() { requestSize(); schedule(); }
            document.addEventListener('visibilitychange', schedule);
            canvas.addEventListener('webglcontextlost', lost);
            canvas.addEventListener('webglcontextrestored', restored);
            const observer = new IntersectionObserver(entries => {
                visible = entries[0].isIntersecting;
                schedule();
            });
            observer.observe(canvas);
            return {
                resize(width, height) {
                    if (canvas.width !== width) canvas.width = width;
                    if (canvas.height !== height) canvas.height = height;
                    schedule();
                },
                show(nextHandle) {
                    handle = nextHandle;
                    canvas.style.visibility = handle ? 'visible' : 'hidden';
                    if (!handle) release();
                    schedule();
                },
                destroy() {
                    disposed = true;
                    release();
                    observer.disconnect();
                    document.removeEventListener('visibilitychange', schedule);
                    canvas.removeEventListener('webglcontextlost', lost);
                    canvas.removeEventListener('webglcontextrestored', restored);
                }
            };
        },
        brandTranslations(translations) {
            for (const key of Object.keys(translations)) {
                if (typeof translations[key] === 'string') translations[key] = translations[key].replace(/Wallpaper Engine/g, 'Mirage Editor');
            }
            Object.assign(translations, {
                ui_caption_editor: 'Mirage Editor',
                ui_editor_welcome_modal_title: 'Mirage Editor',
                mirage_about: session.language === 'zh-chs' ? '关于 Mirage Editor' : session.language === 'zh-cht' ? '關於 Mirage Editor' : 'About Mirage Editor'
            });
            return translations;
        },
        filterMenu(menu) {
            return menu.filter(item => item.label !== 'ui_editor_filemenu_provider').map(item => {
                if (item.label === 'ui_editor_filemenu_help') {
                    return {label: item.label, children: [{label: 'mirage_about', icon: 'fas fa-info-circle', fn: () => window.mirageEditor.about()}]};
                }
                if (item.children) item.children = item.children.filter(child => child.label !== 'ui_editor_filemenu_apply_wallpaper');
                return item;
            });
        },
        async about() {
            await fetch(session.bridge.endpoint + '/about', {
                method: 'POST', headers: {'Authorization': 'Bearer ' + session.bridge.token}
            });
        },
        async zoom() {
            await fetch(session.bridge.endpoint + '/window/zoom', {
                method: 'POST', headers: {'Authorization': 'Bearer ' + session.bridge.token}
            });
        },
        saveSettings(settings) { localStorage.setItem(settingsKey, JSON.stringify(settings)); },
        async openExternalProject() {
            if (choosingProject) return;
            choosingProject = true;
            try {
                const response = await fetch(session.bridge.endpoint + '/open-project', {
                    method: 'POST', headers: {'Authorization': 'Bearer ' + session.bridge.token}
                });
                if (!response.ok) throw new Error('Native project picker: ' + response.status);
                const result = await response.json();
                window.rootScope.$evalAsync(() => {
                    if (result.file) window.editorCtrl.requestOpenProject({file: result.file});
                    if (result.error) window.editorCtrl.showGenericError({title: 'ui_editor_filemenu_open', message: result.error});
                });
            } catch (error) {
                window.ui.logError('Mirage project picker: ' + error.message);
                const message = session.language === 'zh-chs' ? '无法打开工程选择器，请重新启动编辑器后重试。' :
                    session.language === 'zh-cht' ? '無法開啟工程選擇器，請重新啟動編輯器後重試。' :
                    'Unable to open the project picker. Restart the editor and try again.';
                window.rootScope.$evalAsync(() => window.editorCtrl.showGenericError({title: 'ui_editor_filemenu_open', message}));
            } finally { choosingProject = false; }
        }
    };
    let attempts = 0;
    function initialize() {
        if (!window.editorCtrl || !window.editorObject || !window.rootScope) {
            if (++attempts < 200) setTimeout(initialize, 50);
            return;
        }
        let saved = {};
        try { saved = JSON.parse(localStorage.getItem(settingsKey) || '{}'); } catch (_) {}
        window.rootScope.$evalAsync(() => {
            window.utilsService.setLanguage(session.language);
            window.editorCtrl.setEditorSettings({editor: Object.assign({}, session.defaults, saved), config: session.project ? {initialCommands: JSON.stringify({file: session.project})} : {}});
        });
    }
    window.addEventListener('DOMContentLoaded', initialize);
})();

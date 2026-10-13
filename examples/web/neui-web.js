// neui-web.js - mount neui apps into a web page (plans/wasm-host.md Stage 7).
//
// An app is a neui program built as an ES module (CMake: neui_link_example
// ... WEB_MODULE -> name.mjs + name.wasm [+ name.data]). Each mount runs its
// own instance (own heap, own neui state) of the module; the wasm is
// fetched and compiled once per module and shared by every mount.
//
//   import { NeuiApp } from './neui-web.js';
//   const app = await NeuiApp.mount(document.getElementById('slot'), {
//     module: './my_app.mjs',            // or { jspi: '...', asyncify: '...' }
//     flavor: 'auto',                    // 'jspi' / 'asyncify' / 'auto'
//     args:   ['--name=left'],           // argv[1..] of main
//     theme:  'auto',                    // 'dark' / 'light' / 'auto'
//   });
//   app.on('message', (text) => ...);    // NEUI_API_WEB post_message
//   app.postMessage({ hello: 1 });       // -> NEUI_EVENT_WEB_MESSAGE (objects as JSON)
//   app.setTheme('dark');
//   await app.unmount();                 // closes every frame; run() returns
//
// The app's frames mount into `element` (a frame can name another element
// with NEUI_API_WEB set_mount / NEUI_API_EMBED set_parent, a CSS selector).
// A sized element is filled by the app's window and followed when it
// resizes. Dialogs float over the page.
//
// No threads, so no COOP / COEP headers are needed. A module with resources
// also fetches its name.data next to it (the browser's cache covers repeats).

const modules = new Map();   // module URL -> { factory, wasm }

// JSPI where the browser has it, else Asyncify.
function chooseUrl(module, flavor) {
  if (typeof module === 'string') return module;
  const jspi = typeof WebAssembly.Suspending === 'function';
  if (flavor === 'jspi' || (flavor !== 'asyncify' && jspi && module.jspi)) return module.jspi;
  return module.asyncify || module.jspi;
}

function entryFor(url) {
  let e = modules.get(url);
  if (!e) {
    e = { factory: import(url).then((m) => m.default), wasm: null };
    modules.set(url, e);
  }
  return e;
}

function compile(entry, url) {
  if (!entry.wasm) {
    const wasmUrl = url.replace(/\.m?js(\?.*)?$/, '.wasm');
    entry.wasm = (WebAssembly.compileStreaming
      ? WebAssembly.compileStreaming(fetch(wasmUrl))
      : Promise.reject(new Error('no compileStreaming')))
      .catch(() => fetch(wasmUrl).then((r) => {
        if (!r.ok) throw new Error(`neui: ${wasmUrl}: HTTP ${r.status}`);
        return r.arrayBuffer();
      }).then((b) => WebAssembly.compile(b)));
    entry.wasm.catch(() => modules.delete(url));   // let a later mount retry
  }
  return entry.wasm;
}

export class NeuiApp {
  static async mount(element, options = {}) {
    if (!options.module) throw new Error('neui: mount needs options.module');
    const url = new URL(chooseUrl(options.module, options.flavor || 'auto'), document.baseURI).href;
    const entry = entryFor(url);
    const factory = await entry.factory;
    const app = new NeuiApp(element);
    let fail = null;
    const failed = new Promise((_, reject) => { fail = reject; });
    const base = new URL('.', url).href;
    const mod = await Promise.race([failed, factory({
      arguments: (options.args || []).map(String),
      locateFile: (path) => new URL(path, base).href,
      instantiateWasm: (imports, done) => {
        compile(entry, url)
          .then((m) => WebAssembly.instantiate(m, imports).then((inst) => done(inst, m)))
          .catch((e) => { app._fail(e); fail(e); });
        return {};
      },
      print: options.print || ((t) => console.log(t)),
      printErr: options.printErr || ((t) => console.warn(t)),
      onAbort: (what) => { const e = new Error('neui: aborted: ' + what); app._fail(e); fail(e); },
      // Read by the platform layer (platform_web.cpp).
      neuiMount: element,
      neuiTheme: options.theme || 'auto',
      neuiOnSetup: (w) => app._setup(w),
      neuiOnMessage: (text) => app._emit('message', text),
      neuiOnEnd: (unmounted) => app._end(unmounted),
    })]);
    app._module = mod;
    return app;
  }

  constructor(element) {
    this.element = element;
    this._module = null;
    this._web = null;             // the platform layer's page side, once neui started
    this._queue = [];             // messages posted before that
    this._held = [];              // app messages from before any listener
    this._theme = null;
    this._listeners = { message: [], exit: [], error: [] };
    this._unmounting = null;
    this._resolveUnmount = null;
    this._unmountWanted = false;
    this._done = false;
  }

  // Messages the app sent before the first 'message' listener (its startup
  // runs before mount() resolves) are held and go to that listener.
  on(type, fn) {
    (this._listeners[type] || (this._listeners[type] = [])).push(fn);
    if (type === 'message' && this._held.length) {
      const held = this._held;
      this._held = [];
      queueMicrotask(() => { for (const t of held) this._emit('message', t); });
    }
    return this;
  }

  off(type, fn) {
    const l = this._listeners[type];
    if (l) { const i = l.indexOf(fn); if (i >= 0) l.splice(i, 1); }
    return this;
  }

  // A message for the app (NEUI_EVENT_WEB_MESSAGE): strings as they are,
  // anything else as JSON.
  postMessage(data) {
    if (this._done) return;
    const text = typeof data === 'string' ? data : JSON.stringify(data);
    if (this._web) this._web.postToApp(text); else this._queue.push(text);
  }

  // 'dark' / 'light' fix the app's system theme; 'auto' follows the
  // browser. As on the desktop, a frame created earlier picks the change up
  // live only with NEUI_ATTR_FOLLOW_SYSTEM_THEME = 1.
  setTheme(mode) {
    if (this._done) return;
    if (this._web) this._web.setTheme(mode); else this._theme = mode;
  }

  // Close every frame of the app (APP_QUIT is delivered but cannot keep a
  // frame); run() returns and the code after it runs. Resolves once the app
  // has left the page.
  unmount() {
    if (this._done) return Promise.resolve();
    if (!this._unmounting) {
      this._unmounting = new Promise((r) => { this._resolveUnmount = r; });
      if (this._web) this._web.unmount(); else this._unmountWanted = true;
    }
    return this._unmounting;
  }

  _setup(web) {
    this._web = web;
    if (this._theme) web.setTheme(this._theme);
    for (const t of this._queue) web.postToApp(t);
    this._queue = [];
    if (this._unmountWanted) web.unmount();
  }

  _end(unmounted) {
    if (!unmounted) {
      // The app ended by itself (its last window closed): tell the page,
      // then take it off the page like an unmount.
      this._emit('exit');
      if (this._web) this._web.unmount();
      return;
    }
    this._done = true;
    this._web = null;
    this._module = null;
    if (this._resolveUnmount) this._resolveUnmount();
    else this._unmounting = Promise.resolve();
  }

  _emit(type, arg) {
    if (type === 'message' && !(this._listeners.message || []).length) { this._held.push(arg); return; }
    for (const fn of (this._listeners[type] || []).slice()) {
      try { fn(arg); } catch (e) { console.error(e); }
    }
  }

  _fail(e) {
    if (this._done) return;
    this._done = true;
    this._emit('error', e);
    if (this._resolveUnmount) this._resolveUnmount();
    console.error(e);
  }
}

// Web (Emscripten) platform layer for the crossplatform host
// (plans/wasm-host.md).
//
// Each frame (APPWINDOW / PLUGWINDOW / DIALOG) is a <canvas> in a wrapper
// <div>, drawn by the Canvas 2D backend. The browser must never block, so the
// event loop is the Linux select() loop with one change: where Linux sleeps in
// select(), this layer suspends the wasm stack in web_wait() (JSPI or Asyncify)
// and the browser runs. DOM listeners never dispatch: they only queue a record
// (keyed by frame id - the WebFrame may be gone by the time it is read) and
// wake the loop, so every Session dispatch, client callback and paint runs on
// the one suspendable stack inside platform_run / platform_run_modal_until.
// A nested loop (modal show, popup_menu) is the same loop with another exit
// condition on the same stack.
//
// Default actions. Whether the browser may scroll the page on a wheel, move
// focus on Tab or open its context menu is only known after neui dispatched
// the event. The listener keeps the event object and resolves the pending
// wait; the suspended stack resumes in the microtask checkpoint right after
// the listener - still inside the browser's event dispatch - and the drain
// calls preventDefault on the kept event when neui consumed it (plans rule 5;
// proven in Chromium and WebKit in Stage 0).
//
// Input routing mirrors platform_linux.cpp (the overlay pre-check order, the
// key order, double-click synthesis, the scroll router); the Linux code is
// tied to X types and LinuxWindow fields, so it is copied, with "mirrors
// platform_linux.cpp" notes, rather than shared.
//
// Blocking UI (modal DIALOG show, popup_menu, message_box) runs a nested loop
// on the same stack. A DIALOG / message box floats in the page with a DOM
// title bar over its canvas; while it is up its owner's frame sits under a
// backdrop that takes its pointer input. A blocking call made during a paint
// is refused (paint never suspends, plans rule 3).
//
// Text input. Each frame has a hidden <textarea> (the text proxy): while a
// text widget (INPUTBOX / MULTILINE) has neui focus, DOM focus sits in it,
// placed at the widget's caret, so an IME's candidate window and a phone's
// virtual keyboard appear there. Plain keys still arrive as keydown (the
// KEYCHAR comes from KeyboardEvent.key, as before); composition events
// become on_composition calls, and text that comes without a key (virtual
// keyboards, the emoji picker) becomes KEYCHARs. The textarea is kept empty:
// every non-composition edit of it is cancelled in beforeinput.
//
// Clipboard. Writes go to an in-process copy and to navigator.clipboard.
// Reads cannot be synchronous in a browser: Ctrl/Cmd+V is held until the
// browser's paste event delivers the clipboard (no permission prompt), then
// dispatched; any other read (menu Paste, NEUI_API_CLIPBOARD) suspends on
// navigator.clipboard.read / readText, which may ask the user, and falls
// back to the in-process copy when refused. There is no PRIMARY selection.
//
// Drag and drop. A drag from the desktop or the page arrives as DOM drag
// events on a frame's canvas: dragenter / dragover go through the session's
// DnD dispatch and get their verdict (the dropEffect) like a key; the canvas
// also applies the last answer at once, so a browser that misses the
// microtask verdict is one event late, not wrong. The browser's own default
// never runs over a registered canvas (no dropped file opens in the tab,
// also where neui refuses it). A drop reads its
// strings inside the event and its files asynchronously; the files land in
// the in-memory file system (/tmp/neui-drop/...) and arrive as file://
// URIs in text/uri-list, plus their bytes under their own MIME type. A
// browser starts its own drags only from a native dragstart, so begin_drag
// runs an in-page drag: a nested loop over pointer records that the page
// routes to the neui frame under the pointer (Linux's internal-drag model).
// Dragging out of the page is not supported.
//
// Embedding. An app built as an ES module is mounted into a page element by
// examples/web/neui-web.js, one wasm instance per mount; its frames mount in
// that element (or where NEUI_API_WEB set_mount / NEUI_API_EMBED set_parent
// name one by CSS selector). A client that never calls run() (an embedded
// PLUGWINDOW, as in a DAW) is run by the service loop, started from the
// first show when no loop runs. Unmount closes every frame for good (APP_QUIT
// cannot keep one), ends every loop so run() returns to the client, and
// removes what the app put on the page: canvases, listeners, fonts. Messages
// to and from the page and the page's visibility go through NEUI_API_WEB and
// NEUI_EVENT_WEB_*.

#include "host.h"
#include "platform.h"
#include <neui/neui.h>

#include <emscripten.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Full DataItem definition for the item-based clipboard stubs below.
#include "../shared/clipboard_item.h"
#include "../shared/damage.h"
#include "../shared/dnd_modifier_suggest.h"
#include "../shared/message_box_painted.h"
#include "../shared/metrics.h"
#include "../shared/web/keys_web.h"
#include "../shared/web/theme_provider_web.h"
#include "../../backends/canvas2d/canvas2d_backend.h"

// This TU emits the single stb_image implementation for the web host.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATED
#include "../shared/image_loader_stb.h"

// ---------------------------------------------------------------------------
// JavaScript half: Module.neuiWeb (frames, the wake hook, kept events) and
// Module.neuiCanvases (canvas by frame id, read by the Canvas 2D backend).
//
// Record types (neui_web_push): 1 resize, 2 focus, 3 blur, 4 close button,
// 5 press on a blocked frame's backdrop, 10 pointer down, 11 pointer up,
// 12 pointer move, 13 pointer leave, 14 context menu, 15 wheel, 20 key down,
// 21 key up, 30 colour scheme (frame 0), 40 composition start, 41
// composition update, 42 composition end, 43 text without a key, 50 drag
// enter / over, 51 drag leave, 52 drop, 60 in-page drag move, 61 in-page
// drag release, 62 in-page drag cancel (60 / 61: frame = the neui frame
// under the pointer, 0 = none).

EM_JS_DEPS(neui_web, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8");

EM_JS(void, web_setup, (), {
  if (Module.neuiWeb) return;
  // mount: the element the page started the app in (neui-web.js), else null.
  // off: removes every page-level listener at shutdown.
  var W = Module.neuiWeb = { frames: {}, wake: null, kept: new Map(), nextEv: 1, z: 1000,
                             mount: Module.neuiMount || null, theme: Module.neuiTheme || 'auto',
                             off: [], dead: false };
  Module.neuiCanvases = Module.neuiCanvases || {};
  W.wakeUp = function() { if (W.wake) { var w = W.wake; W.wake = null; w(2); } };
  // Queue a record for the loop and wake it if it is waiting.
  W.push = function(type, frame, a, b, c, d, e, f) {
    Module._neui_web_push(type, frame, a || 0, b || 0, c || 0, d || 0, e || 0, f || 0);
    W.wakeUp();
  };
  // A key record: code / key through the fixed C++ key buffer (64 bytes
  // each; both are short names or one character). fresh = the clipboard copy
  // was just filled by a paste event (a held Ctrl/Cmd+V).
  W.sendKey = function(id, type, k, evId, fresh) {
    var buf = Module._neui_web_key_buf();
    stringToUTF8(k.code, buf, 64);
    stringToUTF8(k.key, buf + 64, 64);
    Module._neui_web_push_key(type, id, evId, k.mods, fresh);
    W.wakeUp();
  };
  // DOM focus inside a frame follows neui: the text proxy while a text
  // widget has neui focus (f.want), else the canvas. Only moves focus that
  // is already inside the frame; f.hold pins it during a held paste.
  W.applyFocus = function(f) {
    if (!f.ta || f.hold) return;
    var a = document.activeElement, to = null;
    if (f.want && a === f.cv) to = f.ta;
    else if (!f.want && a === f.ta) to = f.cv;
    if (to) { try { to.focus({ preventScroll: true }); } catch (e) { to.focus(); } }
  };
  // A string into the C++ text buffer; returns its UTF-8 length.
  W.str = function(t) {
    var n = lengthBytesUTF8(t), p = Module._neui_web_text_buf(n + 1);
    stringToUTF8(t, p, n + 1);
    return n;
  };
  W.pushText = function(type, id, t, caret) {
    var n = W.str(t);
    Module._neui_web_push_text(type, id, n, caret);
    W.wakeUp();
  };
  // Hold an event until the drain decides about its default action.
  W.keep = function(ev) {
    var id = W.nextEv++;
    W.kept.set(id, ev);
    if (W.kept.size > 256) W.kept.delete(W.kept.keys().next().value);
    return id;
  };
  W.mods = function(e) {
    return (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0) |
           ((e.getModifierState && e.getModifierState('AltGraph')) ? 16 : 0);
  };
  W.apple = /Mac|iPhone|iPad|iPod/.test((navigator.userAgentData && navigator.userAgentData.platform) ||
                                        navigator.platform || navigator.userAgent || "");
  // Colour scheme: 1 dark, 2 light, 0 no preference. The page can fix it
  // (W.theme "dark" / "light"; "auto" follows prefers-color-scheme).
  W.scheme = function() {
    if (W.theme === 'dark') return 1;
    if (W.theme === 'light') return 2;
    if (typeof matchMedia !== 'function') return 0;
    if (matchMedia('(prefers-color-scheme: dark)').matches) return 1;
    if (matchMedia('(prefers-color-scheme: light)').matches) return 2;
    return 0;
  };
  W.css = function(rgb) { return "#" + (0x1000000 + (rgb & 0xFFFFFF)).toString(16).slice(1); };
  // DnD: NEUI_DND_ACTION_* bits <-> the DataTransfer effect names.
  W.effectName = function(a) { return (a & 1) ? 'copy' : (a & 2) ? 'move' : (a & 4) ? 'link' : 'none'; };
  W.allowedOf = function(dt) {
    var m = { none: 0, copy: 1, move: 2, link: 4, copyMove: 3, copyLink: 5, linkMove: 6 };
    var a = dt ? m[dt.effectAllowed] : undefined;
    return a === undefined ? 7 : a;   // "all", "uninitialized"
  };
  // A DataTransfer type as a neui MIME ("" = not one neui can carry).
  W.neuiMime = function(t) {
    t = String(t).toLowerCase();
    if (t === 'text/plain' || t === 'text') return 'text/plain;charset=utf-8';
    if (t === 'files') return 'text/uri-list';
    return t.indexOf('/') > 0 ? t : "";
  };
  // The formats a drag offers: its types, files as text/uri-list plus each
  // file's own MIME type when the browser tells it before the drop.
  W.dndTypes = function(dt) {
    var out = [], add = function(m) { if (m && out.indexOf(m) < 0) out.push(m); };
    if (!dt) return out;
    var ts = dt.types ? Array.prototype.slice.call(dt.types) : [];
    for (var i = 0; i < ts.length; i++) add(W.neuiMime(ts[i]));
    var it = dt.items ? Array.prototype.slice.call(dt.items) : [];
    for (var j = 0; j < it.length; j++) if (it[j].kind === 'file' && it[j].type) add(it[j].type.toLowerCase());
    return out;
  };
  // A DnD record; the formats go through the text buffer, one per line.
  W.pushDnd = function(type, id, x, y, mods, allowed, evId, types) {
    var n = W.str(types.join('\n'));
    Module._neui_web_push_dnd(type, id, x, y, mods, allowed, evId, n);
    W.wakeUp();
  };
  // Place a floating frame (dialog, message box) in document coordinates.
  // With an owner, x / y keep their offset from the owner's create()
  // position, so a dialog lands where it would over the desktop window;
  // centred = over the owner's canvas. Then clamped into the visible part
  // of the page.
  W.place = function(f) {
    var w = f.wrap.offsetWidth, h = f.wrap.offsetHeight;
    var sx = window.scrollX || 0, sy = window.scrollY || 0;
    var left = f.x + sx, top = f.y + sy;
    var o = f.owner ? W.frames[f.owner] : null;
    if (o) {
      var r = o.cv.getBoundingClientRect();
      if (f.center) {
        left = r.left + sx + (r.width - w) / 2;
        top  = r.top  + sy + (r.height - h) / 2;
      } else {
        left = r.left + sx + f.x - f.ox;
        top  = r.top  + sy + f.y - f.oy;
      }
    }
    left = Math.max(sx, Math.min(left, sx + document.documentElement.clientWidth  - w));
    top  = Math.max(sy, Math.min(top,  sy + document.documentElement.clientHeight - h));
    f.wrap.style.left = Math.round(left) + "px";
    f.wrap.style.top  = Math.round(top) + "px";
  };
  // A device-pixel-ratio change (zoom, another monitor) does not always
  // resize a canvas' device-pixel box, so watch the ratio itself: the query
  // for the current ratio stops matching once it changes. Every frame then
  // re-reports its size (device size 0 = derive it from the new ratio).
  // A listener on a media query / the document, removed again at shutdown.
  var listen = function(t, name, fn) {
    if (t.addEventListener) t.addEventListener(name, fn); else if (t.addListener) t.addListener(fn);
    var off = function() {
      if (t.removeEventListener) t.removeEventListener(name, fn); else if (t.removeListener) t.removeListener(fn);
    };
    W.off.push(off);
    return off;
  };
  W.watchDpr = function() {
    if (typeof matchMedia !== 'function') return;
    var q = matchMedia('(resolution: ' + devicePixelRatio + 'dppx)'), off = null;
    off = listen(q, 'change', function() {
      off();
      for (var id in W.frames) {
        var cv = W.frames[id].cv;
        W.push(1, +id, cv.clientWidth, cv.clientHeight, 0, 0);
      }
      W.watchDpr();
    });
  };
  W.watchDpr();
  if (typeof matchMedia === 'function')
    listen(matchMedia('(prefers-color-scheme: dark)'), 'change', function() {
      if (W.theme === 'auto') W.push(30, 0, W.scheme());
    });
  // Page lifecycle (record 31: 1 visible, 2 hidden, 3 leaving).
  listen(document, 'visibilitychange', function() { W.push(31, 0, document.hidden ? 2 : 1); });
  listen(window, 'pagehide', function() { W.push(31, 0, 3); });

  // The page side (neui-web.js) talks to the app through these.
  W.setTheme = function(mode) {
    if (W.dead) return;
    W.theme = (mode === 'dark' || mode === 'light') ? mode : 'auto';
    W.push(30, 0, W.scheme());
  };
  W.postToApp = function(text) { if (!W.dead) W.pushText(70, 0, String(text), 0); };
  W.unmount = function() { if (!W.dead) { Module._neui_web_unmount(); W.wakeUp(); } };
  if (Module.neuiOnSetup) Module.neuiOnSetup(W);
});

// The loop for clients that never call run() (an embedded PLUGWINDOW, an app
// whose main returned), started from a fresh task: neui_web_service returns
// at once while another loop runs.
EM_JS(void, web_schedule_service, (), {
  setTimeout(function() {
    var W = Module.neuiWeb;
    if (!W || W.dead) return;
    try {
      var r = Module._neui_web_service();
      if (r && r.catch) r.catch(function(e) { console.error(e); });
    } catch (e) { console.error(e); }
  }, 0);
});

// The app is done (run() returned): unmounted = after an unmount, which also
// removes everything the app put on the page. Tells neui-web.js, outside the
// wasm call.
EM_JS(void, web_ended, (int unmounted), {
  var W = Module.neuiWeb;
  if (unmounted && W) {
    W.dead = true;
    for (var i = 0; i < W.off.length; i++) { try { W.off[i](); } catch (e) {} }
    W.off = [];
    for (var id in W.frames) {
      var f = W.frames[id];
      if (f.ro) f.ro.disconnect();
      if (f.wrap.parentNode) f.wrap.parentNode.removeChild(f.wrap);
    }
    W.frames = {};
    W.kept.clear();
    Module.neuiCanvases = {};
  }
  var cb = Module.neuiOnEnd;
  if (cb) queueMicrotask(function() { cb(!!unmounted); });
});

// A message for the page (neui-web.js: app.on('message')), delivered outside
// the wasm call. 0 = nobody on the page side.
EM_JS(int, web_post_to_page, (const char* p, int n), {
  var cb = Module.neuiOnMessage;
  if (!cb) return 0;
  var t = UTF8ToString(p, n);
  queueMicrotask(function() { cb(t); });
  return 1;
});

EM_JS(int, web_page_hidden, (), { return (typeof document !== 'undefined' && document.hidden) ? 1 : 0; });

// Suspend until a record is queued (returns 2), the next animation frame when
// want_raf (1), or timeout_ms (0; < 0 = no timeout).
EM_ASYNC_JS(int, web_wait, (int timeout_ms, int want_raf), {
  var W = Module.neuiWeb;
  return await new Promise(function(resolve) {
    var done = false, tid = 0, rid = 0;
    var fin = function(why) {
      if (done) return;
      done = true;
      W.wake = null;
      if (tid) clearTimeout(tid);
      if (rid) cancelAnimationFrame(rid);
      resolve(why);
    };
    W.wake = fin;
    if (want_raf) rid = requestAnimationFrame(function() { rid = 0; fin(1); });
    if (timeout_ms >= 0) tid = setTimeout(function() { tid = 0; fin(0); }, timeout_ms);
  });
});

// The drain's decision about a kept event's default action.
EM_JS(void, web_verdict, (int ev_id, int consume), {
  var W = Module.neuiWeb, e = W.kept.get(ev_id);
  if (!e) return;
  W.kept.delete(ev_id);
  if (consume) e.preventDefault();
});

EM_JS(double, web_dpr, (), { return (typeof devicePixelRatio === 'number' && devicePixelRatio > 0) ? devicePixelRatio : 1; });
EM_JS(int, web_is_apple, (), { return Module.neuiWeb.apple ? 1 : 0; });
EM_JS(int, web_scheme, (), { return Module.neuiWeb.scheme(); });

// The page URL's NEUI_* query parameters ("?NEUI_DEBUG_DAMAGE=1") as
// "KEY=value\n" lines: the host's debug switches are environment variables.
EM_JS(void, web_neui_query, (char* buf, int cap), {
  var out = "";
  try {
    new URLSearchParams(location.search).forEach(function(v, k) {
      if (k.indexOf("NEUI_") === 0) out += k + "=" + v + "\n";
    });
  } catch (e) {}
  stringToUTF8(out, buf, cap);
});

EM_JS(int, web_reduced_motion, (), {
  return (typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches) ? 1 : 0;
});

// kind: 0 APPWINDOW, 1 PLUGWINDOW, 2 DIALOG, 3 message box. An APPWINDOW or
// PLUGWINDOW mounts into the element its CSS selector names (NEUI_API_WEB
// set_mount, NEUI_API_EMBED set_parent; searched in the app's mount element
// first), else into the app's mount element, else #neui-root, else <body>.
// In a container with a size of its own the frame fills it (and follows it);
// otherwise the canvas takes the frame's create() size. A dialog or message
// box floats above the page with a DOM title bar (title, close button, drag
// to move) over its canvas; the canvas stays the client area.
EM_JS(void, web_frame_create, (int id, int kind, int x, int y, int w, int h, const char* selp), {
  var W = Module.neuiWeb, host = null, sel = selp ? UTF8ToString(selp) : "";
  if (kind <= 1) {
    if (sel) {
      try { host = (W.mount && W.mount.querySelector(sel)) || document.querySelector(sel); } catch (err) {}
      if (!host) console.warn('neui: no element matches "' + sel + '"');
    }
    host = host || W.mount || document.getElementById('neui-root');
  }
  host = host || document.body;
  var fill = kind <= 1 && host !== document.body && host.clientWidth > 0 && host.clientHeight > 0;
  var floating = kind >= 2;
  var wrap = document.createElement('div');
  wrap.className = floating ? 'neui-frame neui-dialog' : 'neui-frame';
  wrap.style.cssText = 'display:none;line-height:0;user-select:none;-webkit-user-select:none;position:relative;' +
                       (fill ? 'width:100%;height:100%;' : "");
  var title = null;
  if (floating) {
    wrap.style.position = 'absolute';
    wrap.style.border = '1px solid var(--neui-border, #555)';
    wrap.style.background = 'var(--neui-bar-bg, #2b2b2b)';
    wrap.style.boxShadow = '0 10px 32px rgba(0,0,0,0.45)';
    wrap.setAttribute('role', 'dialog');
    var bar = document.createElement('div');
    bar.style.cssText = 'display:flex;align-items:center;height:28px;line-height:28px;padding-left:10px;' +
                        'font:600 13px system-ui,sans-serif;color:var(--neui-bar-fg, #ddd);' +
                        'cursor:default;touch-action:none;';
    title = document.createElement('span');
    title.style.cssText = 'flex:1;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;';
    var close = document.createElement('button');
    close.type = 'button';
    close.textContent = String.fromCharCode(215);
    close.setAttribute('aria-label', 'Close');
    close.style.cssText = 'width:36px;height:28px;border:0;padding:0;background:transparent;color:inherit;' +
                          'font:16px system-ui,sans-serif;cursor:pointer;';
    close.addEventListener('pointerenter', function() { close.style.background = 'var(--neui-close-hover, #c42b1c)'; });
    close.addEventListener('pointerleave', function() { close.style.background = 'transparent'; });
    close.addEventListener('pointerdown', function(e) { e.stopPropagation(); });
    close.addEventListener('click', function() { W.push(4, id); });
    bar.appendChild(title);
    bar.appendChild(close);
    wrap.appendChild(bar);
    // Drag the title bar to move the frame (the page's business: neui keeps
    // no window position on the web).
    var drag = null;
    bar.addEventListener('pointerdown', function(e) {
      if (e.button !== 0) return;
      drag = { px: e.clientX, py: e.clientY, l: wrap.offsetLeft, t: wrap.offsetTop };
      try { bar.setPointerCapture(e.pointerId); } catch (err) {}
      e.preventDefault();
    });
    bar.addEventListener('pointermove', function(e) {
      if (!drag) return;
      wrap.style.left = (drag.l + e.clientX - drag.px) + 'px';
      wrap.style.top  = (drag.t + e.clientY - drag.py) + 'px';
    });
    var stop = function() { drag = null; };
    bar.addEventListener('pointerup', stop);
    bar.addEventListener('pointercancel', stop);
  }
  var cv = document.createElement('canvas');
  cv.tabIndex = 0;
  // touch-action:none: a drag on the canvas is a neui drag, not a page pan.
  cv.style.cssText = 'display:block;outline:none;touch-action:none;' +
                     (fill ? 'width:100%;height:100%;' : 'width:' + w + 'px;height:' + h + 'px;');
  wrap.appendChild(cv);
  // The text proxy (not on a message box): invisible, never hit by the
  // pointer, moved to the caret by web_text_proxy.
  var ta = null;
  if (kind !== 3) {
    ta = document.createElement('textarea');
    ta.tabIndex = -1;
    ta.setAttribute('autocomplete', 'off');
    ta.setAttribute('autocorrect', 'off');
    ta.setAttribute('autocapitalize', 'off');
    ta.spellcheck = false;
    ta.style.cssText = 'position:absolute;left:0;top:0;width:1px;height:16px;margin:0;padding:0;border:0;' +
                       'opacity:0;resize:none;overflow:hidden;white-space:pre;font-size:16px;line-height:16px;' +
                       'color:transparent;background:transparent;caret-color:transparent;outline:none;' +
                       'pointer-events:none;';
    wrap.appendChild(ta);
  }
  host.appendChild(wrap);
  Module.neuiCanvases[id] = cv;
  var f = W.frames[id] = { wrap: wrap, cv: cv, ro: null, body: host === document.body, fill: fill,
                           floating: floating, title: title, x: x, y: y, owner: 0, ox: 0, oy: 0,
                           center: false, shade: null, ta: ta, typed: false, paste: null,
                           want: false, hold: false, dnd: false, dndAct: 0 };

  // Size + device pixel size (zoom, monitor changes) -> one RESIZE record.
  f.ro = new ResizeObserver(function(entries) {
    var e = entries[entries.length - 1], pw = 0, ph = 0;
    if (e.devicePixelContentBoxSize && e.devicePixelContentBoxSize[0]) {
      pw = e.devicePixelContentBoxSize[0].inlineSize;
      ph = e.devicePixelContentBoxSize[0].blockSize;
    }
    W.push(1, id, e.contentRect.width, e.contentRect.height, pw, ph);
  });
  try { f.ro.observe(cv, { box: 'device-pixel-content-box' }); } catch (err) { f.ro.observe(cv); }
  // Focus moving between the canvas and its text proxy stays inside the
  // frame: only focus entering or leaving the frame is reported.
  var inside = function(el) { return el === cv || (ta !== null && el === ta); };
  var onFocus = function(e) {
    if (!inside(e.relatedTarget)) W.push(2, id);
    // A click focuses the canvas even while the proxy should have focus:
    // move it back at once (still inside the gesture, which a phone needs
    // to open its keyboard).
    if (ta !== null) W.applyFocus(f);
  };
  var onBlur  = function(e) { if (!inside(e.relatedTarget)) W.push(3, id); };
  cv.addEventListener('focus', onFocus);
  cv.addEventListener('blur',  onBlur);

  // Pointer: canvas-relative CSS px (= frame-local logical px). A press
  // captures the pointer, so a drag keeps reporting outside the canvas and
  // its release always arrives (the Linux pressed-widget model).
  var at = function(e) { var r = cv.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
  cv.addEventListener('pointerdown', function(e) {
    var p = at(e);
    try { cv.setPointerCapture(e.pointerId); } catch (err) {}
    if (!inside(document.activeElement)) { try { cv.focus({ preventScroll: true }); } catch (err) { cv.focus(); } }
    W.push(10, id, p[0], p[1], e.button, e.buttons, W.mods(e), e.timeStamp);
  });
  cv.addEventListener('pointerup', function(e) {
    var p = at(e);
    W.push(11, id, p[0], p[1], e.button, e.buttons, W.mods(e), e.timeStamp);
  });
  cv.addEventListener('pointercancel', function(e) {
    var p = at(e);
    W.push(11, id, p[0], p[1], 0, 0, W.mods(e), e.timeStamp);
  });
  cv.addEventListener('pointermove', function(e) {
    var p = at(e);
    W.push(12, id, p[0], p[1], 0, e.buttons, W.mods(e), e.timeStamp);
  });
  cv.addEventListener('pointerleave', function() { W.push(13, id); });
  cv.addEventListener('contextmenu', function(e) {
    var p = at(e);
    W.push(14, id, p[0], p[1], W.keep(e));
  });
  cv.addEventListener('wheel', function(e) {
    var p = at(e);
    W.push(15, id, p[0], p[1], e.deltaX, e.deltaY, e.deltaMode, W.keep(e));
  }, { passive: false });
  // A drag from the desktop or the page (once the host registered the frame
  // as a drop target). The browser's default never runs over the canvas (a
  // file dropped where neui refuses must not open in the tab): refusal is
  // dropEffect "none". f.dndAct, the last answer, is applied at once; the
  // drain's verdict then sets this event's (see the file comment).
  var onDragOver = function(e) {
    if (!f.dnd || W.drag) return;
    var dt = e.dataTransfer, p = at(e);
    e.preventDefault();
    if (dt) dt.dropEffect = W.effectName(f.dndAct);
    W.pushDnd(50, id, p[0], p[1], W.mods(e), W.allowedOf(dt), W.keep(e), W.dndTypes(dt));
  };
  cv.addEventListener('dragenter', onDragOver);
  cv.addEventListener('dragover', onDragOver);
  cv.addEventListener('dragleave', function() {
    if (!f.dnd || W.drag) return;
    f.dndAct = 0;
    W.push(51, id);
  });
  // Strings must be read inside the event; files are read after it, then
  // the drop is queued with everything in the C++ drop item.
  cv.addEventListener('drop', function(e) {
    if (!f.dnd || W.drag) return;
    e.preventDefault();   // never let the browser open a dropped file
    f.dndAct = 0;
    var dt = e.dataTransfer, p = at(e), mods = W.mods(e), allowed = W.allowedOf(dt);
    var types = W.dndTypes(dt), strings = [];
    var ts = dt && dt.types ? Array.prototype.slice.call(dt.types) : [];
    for (var i = 0; i < ts.length; i++) {
      var m = W.neuiMime(ts[i]);
      if (!m || String(ts[i]).toLowerCase() === 'files') continue;
      var v = "";
      try { v = dt.getData(ts[i]); } catch (err) {}
      if (v) strings.push([m, v]);
    }
    var files = dt && dt.files ? Array.prototype.slice.call(dt.files) : [];
    var send = function(bufs) {
      Module._neui_web_drop_begin();
      for (var i = 0; i < strings.length; i++) {
        var ml = lengthBytesUTF8(strings[i][0]);
        Module._neui_web_drop_put(ml, W.str(strings[i][0] + strings[i][1]) - ml);
      }
      for (var j = 0; j < files.length; j++) {
        if (!bufs[j]) continue;
        var name = files[j].name || 'file', type = (files[j].type || "").toLowerCase();
        var nl = lengthBytesUTF8(name), tl = lengthBytesUTF8(type), u8 = bufs[j];
        var q = Module._neui_web_text_buf(nl + tl + u8.length + 1);
        stringToUTF8(name + type, q, nl + tl + 1);
        HEAPU8.set(u8, q + nl + tl);
        Module._neui_web_drop_file(nl, tl, u8.length);
      }
      W.pushDnd(52, id, p[0], p[1], mods, allowed, 0, types);
    };
    if (!files.length) { send([]); return; }
    Promise.all(files.map(function(fl) {
      return fl.arrayBuffer().then(function(b) { return new Uint8Array(b); }, function() { return null; });
    })).then(send);
  });
  // Keys reach the focused canvas or text proxy. Keys an IME is processing
  // ("Process" / 229) are left to it: the composition events follow.
  var key = function(type, e) {
    if (e.isComposing || e.key === 'Process' || e.keyCode === 229) return;
    var k = { code: e.code || "", key: e.key || "", mods: W.mods(e) };
    if (type === 20) {
      // The textarea's beforeinput for this key (same task) is then known
      // to be handled already.
      f.typed = true;
      setTimeout(function() { f.typed = false; }, 0);
      // Ctrl/Cmd+V: hold the key until the paste event has filled the
      // clipboard copy. Its default action must run (it fires 'paste'), and
      // reliably only from an editable element, so focus the proxy first.
      if (ta && (W.apple ? e.metaKey : e.ctrlKey) && !e.altKey && (e.key === 'v' || e.key === 'V')) {
        f.hold = true;
        if (document.activeElement !== ta) { try { ta.focus({ preventScroll: true }); } catch (err) { ta.focus(); } }
        f.paste = k;
        setTimeout(function() {
          if (f.paste !== k) return;
          f.paste = null; f.hold = false; W.applyFocus(f);
          W.sendKey(id, 20, k, 0, 0);
        }, 300);
        return;
      }
    }
    W.sendKey(id, type, k, type === 20 ? W.keep(e) : 0, 0);
  };
  cv.addEventListener('keydown', function(e) { key(20, e); });
  cv.addEventListener('keyup',   function(e) { key(21, e); });

  if (ta) {
    ta.addEventListener('focus', onFocus);
    ta.addEventListener('blur',  onBlur);
    ta.addEventListener('keydown', function(e) { key(20, e); });
    ta.addEventListener('keyup',   function(e) { key(21, e); });
    // Keep the proxy empty: cancel every edit except composition (which
    // cannot be cancelled). Text arriving without a keydown (virtual
    // keyboards, emoji picker, dictation) goes to neui as text or keys.
    ta.addEventListener('beforeinput', function(e) {
      var t = e.inputType || "";
      if (e.isComposing || t.indexOf('Composition') >= 0) return;
      e.preventDefault();
      if (f.typed) return;   // its keydown already reached neui
      var sk = function(name) { W.sendKey(id, 20, { code: name, key: name, mods: 0 }, 0, 0); };
      if (t === 'insertText' || t === 'insertReplacementText') { if (e.data) W.pushText(43, id, e.data, 0); }
      else if (t === 'deleteContentBackward') sk('Backspace');
      else if (t === 'deleteContentForward') sk('Delete');
      else if (t === 'insertLineBreak' || t === 'insertParagraph') sk('Enter');
    });
    ta.addEventListener('compositionstart', function() { W.push(40, id); });
    ta.addEventListener('input', function(e) {
      if (!e.isComposing) { ta.value = ""; return; }
      // The proxy holds just the composition; its selection is the caret.
      var v = ta.value, c = ta.selectionStart;
      if (typeof c !== 'number' || c > v.length) c = v.length;
      W.pushText(41, id, v, lengthBytesUTF8(v.slice(0, c)));
    });
    ta.addEventListener('compositionend', function(e) {
      W.pushText(42, id, e.data || "", 0);
      setTimeout(function() { ta.value = ""; }, 0);
    });
  }
  // The paste event: fill the clipboard copy, then release the held key. A
  // paste from the browser's own menu becomes a Ctrl/Cmd+V of its own.
  var onPaste = function(e) {
    var held = f.paste;
    f.paste = null;
    f.hold = false;
    W.applyFocus(f);
    // A clipboard with nothing neui can use (no text, no HTML, no files)
    // keeps the copy: that is what a failed write of neui's own leaves.
    var dt = e.clipboardData;
    if (dt) {
      var t = dt.getData('text/plain'), h = dt.getData('text/html');
      if (t || h || (dt.files && dt.files.length > 0)) {
        Module._neui_web_clip_begin();
        if (t) Module._neui_web_clip_put(0, W.str(t));
        if (h) Module._neui_web_clip_put(1, W.str(h));
      }
    }
    e.preventDefault();
    W.sendKey(id, 20, held || { code: 'KeyV', key: 'v', mods: W.apple ? 8 : 2 }, 0, 1);
  };
  cv.addEventListener('paste', onPaste);
  if (ta) ta.addEventListener('paste', onPaste);
});

EM_JS(void, web_frame_destroy, (int id), {
  var W = Module.neuiWeb, f = W.frames[id]; if (!f) return;
  if (f.ro) f.ro.disconnect();
  if (f.wrap.parentNode) f.wrap.parentNode.removeChild(f.wrap);
  delete W.frames[id];
  delete Module.neuiCanvases[id];
});

EM_JS(void, web_frame_show, (int id, int show), {
  var W = Module.neuiWeb, f = W.frames[id]; if (!f) return;
  f.wrap.style.display = show ? 'block' : 'none';
  if (show && f.floating) {   // on top of whatever floats already
    f.wrap.style.zIndex = String(++W.z);
    W.place(f);
  }
});

// A floating frame's owner (another frame id, 0 = none) and the owner's
// create() position; centred = over the owner's canvas (message box).
EM_JS(void, web_frame_owner, (int id, int owner, int ox, int oy, int centred), {
  var f = Module.neuiWeb.frames[id]; if (!f) return;
  f.owner = owner; f.ox = ox; f.oy = oy; f.center = !!centred;
});

// Title bar colours from the session palette (0xRRGGBB).
EM_JS(void, web_frame_chrome, (int id, int bar_bg, int bar_fg, int border, int close_hover), {
  var W = Module.neuiWeb, f = W.frames[id]; if (!f || !f.floating) return;
  var s = f.wrap.style;
  s.setProperty('--neui-bar-bg', W.css(bar_bg));
  s.setProperty('--neui-bar-fg', W.css(bar_fg));
  s.setProperty('--neui-border', W.css(border));
  s.setProperty('--neui-close-hover', W.css(close_hover));
});

// A frame blocked by a modal child sits under a backdrop: it dims the frame
// and takes its pointer input (a press there is reported as record 5, so the
// modal child can come forward). Wheel events pass through to the page.
EM_JS(void, web_frame_block, (int id, int on), {
  var W = Module.neuiWeb, f = W.frames[id]; if (!f) return;
  if (on && !f.shade) {
    var d = document.createElement('div');
    d.style.cssText = 'position:absolute;left:0;top:0;right:0;bottom:0;background:rgba(0,0,0,0.28);';
    d.addEventListener('pointerdown', function(e) { e.preventDefault(); W.push(5, id); });
    d.addEventListener('contextmenu', function(e) { e.preventDefault(); });
    f.wrap.appendChild(d);
    f.shade = d;
  } else if (!on && f.shade) {
    if (f.shade.parentNode) f.shade.parentNode.removeChild(f.shade);
    f.shade = null;
  }
});

EM_JS(void, web_frame_focus, (int id), {
  var f = Module.neuiWeb.frames[id]; if (!f) return;
  try { f.cv.focus({ preventScroll: true }); } catch (e) { f.cv.focus(); }
});

// Size in CSS px (not for a frame that fills its container); a floating
// frame also moves (placed as at show).
EM_JS(void, web_frame_geometry, (int id, int x, int y, int w, int h, int move), {
  var W = Module.neuiWeb, f = W.frames[id]; if (!f) return;
  if (!f.fill) { f.cv.style.width = w + 'px'; f.cv.style.height = h + 'px'; }
  if (move && f.floating) { f.x = x; f.y = y; f.center = false; W.place(f); }
});

// Min / max size (CSS px, <= 0 = none) for a frame that sizes itself.
EM_JS(void, web_frame_constraints, (int id, int minw, int minh, int maxw, int maxh), {
  var f = Module.neuiWeb.frames[id]; if (!f || f.fill) return;
  var s = f.cv.style;
  s.minWidth = minw > 0 ? minw + 'px' : ""; s.minHeight = minh > 0 ? minh + 'px' : "";
  s.maxWidth = maxw > 0 ? maxw + 'px' : ""; s.maxHeight = maxh > 0 ? maxh + 'px' : "";
});

// The title labels the canvas for assistive technology; only a full-page app
// (mounted straight into <body>) also owns the document title.
EM_JS(void, web_frame_title, (int id, const char* text), {
  var f = Module.neuiWeb.frames[id]; if (!f) return;
  var t = UTF8ToString(text);
  f.cv.setAttribute('aria-label', t);
  if (f.title) f.title.textContent = t;
  else if (f.body) document.title = t;
});

EM_JS(void, web_set_cursor, (const char* css), {
  var W = Module.neuiWeb, c = UTF8ToString(css);
  for (var id in W.frames) W.frames[id].cv.style.cursor = c;
});

// Text proxy: active = a text widget of this frame has neui focus; x / y / h
// = its caret in frame-local px. DOM focus follows, if it is in the frame.
EM_JS(void, web_text_proxy, (int id, int active, int x, int y, int h), {
  var W = Module.neuiWeb, f = W.frames[id]; if (!f || !f.ta) return;
  if (active) {
    f.ta.style.left = (f.cv.offsetLeft + x) + 'px';
    f.ta.style.top  = (f.cv.offsetTop + y) + 'px';
    f.ta.style.height = Math.max(h, 1) + 'px';
  }
  f.want = !!active;
  W.applyFocus(f);
});

// Clipboard write, fire and forget (inside the key's user activation). A
// page that is not a secure context has no navigator.clipboard; the old
// copy command over a temporary textarea stands in.
EM_JS(void, web_clip_write_text, (const char* p, int n), {
  var t = UTF8ToString(p, n), c = navigator.clipboard;
  if (c && c.writeText && window.isSecureContext) { c.writeText(t).catch(function() {}); return; }
  var a = document.activeElement, ta = document.createElement('textarea');
  ta.value = t;
  ta.style.cssText = 'position:fixed;left:0;top:0;opacity:0;';
  document.body.appendChild(ta);
  ta.select();
  try { document.execCommand('copy'); } catch (e) {}
  document.body.removeChild(ta);
  if (a && a.focus) { try { a.focus({ preventScroll: true }); } catch (e) { a.focus(); } }
});

// Several formats at once (kind 0 text/plain, 1 text/html, 2 image/png),
// collected by web_clip_item_add, written by web_clip_item_commit.
EM_JS(void, web_clip_item_add, (int kind, const unsigned char* p, int n), {
  var W = Module.neuiWeb;
  W.clipItem = W.clipItem || [];
  W.clipItem.push([kind, HEAPU8.slice(p, p + n)]);
});
EM_JS(void, web_clip_item_commit, (), {
  var W = Module.neuiWeb, parts = W.clipItem || [], c = navigator.clipboard;
  W.clipItem = [];
  var types = ['text/plain', 'text/html', 'image/png'], text = null, obj = {}, any = false;
  for (var i = 0; i < parts.length; i++) {
    var k = parts[i][0], u8 = parts[i][1];
    if (k === 0) text = new TextDecoder().decode(u8);
    obj[types[k]] = new Blob([u8], { type: types[k] });
    any = true;
  }
  if (!any || !c) return;
  var plain = function() { if (text !== null && c.writeText) c.writeText(text).catch(function() {}); };
  if (typeof ClipboardItem === 'function' && c.write) c.write([new ClipboardItem(obj)]).catch(plain);
  else plain();
});

// Read the system clipboard into the C++ copy (neui_web_clip_begin / put):
// every format (all) or just the text. Suspends; may show the browser's
// permission prompt. 1 = read, 0 = refused / unsupported / timed out.
EM_ASYNC_JS(int, web_clip_read, (int all, int timeout_ms), {
  var c = navigator.clipboard;
  if (!c) return 0;
  var late = new Promise(function(r) { setTimeout(function() { r(null); }, timeout_ms); });
  var put = function(kind, u8) {
    var p = Module._neui_web_text_buf(u8.length + 1);
    HEAPU8.set(u8, p);
    Module._neui_web_clip_put(kind, u8.length);
  };
  try {
    if (all && c.read) {
      var items = await Promise.race([c.read(), late]);
      if (!items) return 0;
      var types = ['text/plain', 'text/html', 'image/png'], got = [];
      for (var i = 0; i < items.length; i++)
        for (var k = 0; k < 3; k++)
          if (items[i].types.indexOf(types[k]) >= 0) {
            var b = await items[i].getType(types[k]);
            got.push([k, new Uint8Array(await b.arrayBuffer())]);
          }
      Module._neui_web_clip_begin();
      for (var j = 0; j < got.length; j++) put(got[j][0], got[j][1]);
      return 1;
    }
    if (!c.readText) return 0;
    var t = await Promise.race([c.readText(), late]);
    if (typeof t !== 'string') return 0;
    Module._neui_web_clip_begin();
    put(0, new TextEncoder().encode(t));
    return 1;
  } catch (e) {
    return 0;
  }
});

// Drag and drop: a frame takes drags only once the host registered it.
EM_JS(void, web_dnd_register, (int id, int on), {
  var f = Module.neuiWeb.frames[id]; if (!f) return;
  f.dnd = !!on;
  f.dndAct = 0;
});

// The drain's answer to a drag enter / over: the effect it accepts with,
// "none" = refused (the canvas already cancelled the default).
EM_JS(void, web_dnd_verdict, (int id, int ev_id, int act), {
  var W = Module.neuiWeb, f = W.frames[id], e = W.kept.get(ev_id);
  if (f) f.dndAct = act;
  if (!e) return;
  W.kept.delete(ev_id);
  if (e.dataTransfer) e.dataTransfer.dropEffect = W.effectName(act);
});

// An in-page drag from neui (begin_drag). Window-level capture listeners
// take every pointer and key event while it runs (nothing reaches a frame)
// and report the neui frame under the pointer in frame-local px. The
// preview (BGRA premultiplied, device px) follows the pointer, hot spot at
// (hx, hy) logical px, -1 = centre.
EM_JS(void, web_drag_begin, (const unsigned char* px, int w, int h, double scale, int hx, int hy), {
  var W = Module.neuiWeb;
  var d = W.drag = { cx: 0, cy: 0, pv: null, hx: 0, hy: 0, style: null, h: null };
  if (px && w > 0 && h > 0) {
    var c = document.createElement('canvas');
    c.width = w; c.height = h;
    var cw = w / scale, ch = h / scale;
    c.style.cssText = 'position:fixed;left:0;top:0;width:' + cw + 'px;height:' + ch + 'px;' +
                      'pointer-events:none;z-index:2147483647;opacity:0.85;display:none;';
    var img = c.getContext('2d').createImageData(w, h), s = HEAPU8.subarray(px, px + w * h * 4), o = img.data;
    for (var i = 0; i < s.length; i += 4) {
      var a = s[i + 3], k = a ? 255 / a : 0;
      o[i] = Math.min(255, s[i + 2] * k); o[i + 1] = Math.min(255, s[i + 1] * k);
      o[i + 2] = Math.min(255, s[i] * k); o[i + 3] = a;
    }
    c.getContext('2d').putImageData(img, 0, 0);
    document.body.appendChild(c);
    d.pv = c;
    d.hx = hx < 0 ? cw / 2 : hx;
    d.hy = hy < 0 ? ch / 2 : hy;
  }
  d.style = document.createElement('style');
  document.head.appendChild(d.style);
  W.dragCursor = function(name) { d.style.textContent = '*{cursor:' + name + ' !important}'; };
  W.dragCursor('no-drop');
  var target = function(x, y) {
    var el = document.elementFromPoint(x, y);
    for (var id in W.frames) {
      var f = W.frames[id];
      if (f.cv === el && f.dnd) { var r = el.getBoundingClientRect(); return [+id, x - r.left, y - r.top]; }
    }
    return [0, 0, 0];
  };
  var move = function(x, y, mods) {
    d.cx = x; d.cy = y;
    if (d.pv) {
      d.pv.style.display = 'block';
      d.pv.style.transform = 'translate(' + (x - d.hx) + 'px,' + (y - d.hy) + 'px)';
    }
    var t = target(x, y);
    W.push(60, t[0], t[1], t[2], mods);
  };
  var stop = function(e) { e.stopPropagation(); if (e.cancelable) e.preventDefault(); };
  d.h = {
    pointermove: function(e) { stop(e); move(e.clientX, e.clientY, W.mods(e)); },
    pointerup: function(e) {
      stop(e);
      var t = target(e.clientX, e.clientY);
      W.push(61, t[0], t[1], t[2], W.mods(e));
    },
    pointerdown: stop,
    pointercancel: function(e) { stop(e); W.push(62, 0); },
    contextmenu: stop,
    // Esc cancels; Ctrl / Shift change the suggested action where it stands.
    keydown: function(e) {
      stop(e);
      if (e.key === 'Escape') W.push(62, 0); else move(d.cx, d.cy, W.mods(e));
    },
    keyup: function(e) { stop(e); move(d.cx, d.cy, W.mods(e)); },
    // The window losing focus (another app) cancels; an element's blur
    // reaches this capture listener too and does not.
    blur: function(e) { if (e.target === window) W.push(62, 0); }
  };
  for (var n in d.h) window.addEventListener(n, d.h[n], true);
});

// Cursor over the current target: the accepted action, or no-drop.
EM_JS(void, web_drag_cursor, (int act), {
  var W = Module.neuiWeb; if (!W.drag) return;
  W.dragCursor((act & 1) ? 'copy' : (act & 2) ? 'grabbing' : (act & 4) ? 'alias' : 'no-drop');
});

EM_JS(void, web_drag_end, (), {
  var W = Module.neuiWeb, d = W.drag; if (!d) return;
  for (var n in d.h) window.removeEventListener(n, d.h[n], true);
  if (d.pv && d.pv.parentNode) d.pv.parentNode.removeChild(d.pv);
  if (d.style.parentNode) d.style.parentNode.removeChild(d.style);
  W.drag = null;
});

namespace xpl_host
{
  // The host's session registry (host.cpp).
  Session* session_by_id(uint32_t session_id);
  uint32_t session_count();

namespace
{
  // Per-frame record, stashed on WidgetData::native_handle.
  struct WebFrame
  {
    int       id           = 0;        // DOM / canvas id (Module.neuiCanvases)
    Session*  session      = nullptr;
    uint32_t  widget_index = 0;
    bool      is_appwindow = false;
    bool      is_dialog    = false;
    int       owner_id     = 0;        // a dialog's owner frame (0 = none)
    bool      counted      = false;    // contributes to g_appwindow_count
    float     dpr          = 1.0f;     // device px per CSS px (= logical px)
    // needs_paint = a FULL repaint is pending; `damage` collects partial
    // rects while it is false (a pending full repaint absorbs them).
    bool      needs_paint  = false;
    neui_detail::DamageRegion damage;
    bool      toast_anim   = false;
    bool      clock_anim   = false;
    // Set while a modal child is up (its input is dropped).
    bool      input_disabled = false;
    // GRID / SECTION spring-back in flight (the heartbeat steps them). 0 = none.
    uint32_t  bouncing_grid_index    = 0;
    uint32_t  bouncing_section_index = 0;
    // Fractional wheel-notch accumulators for the classic line path.
    double    scroll_v_accum = 0.0;
    double    scroll_h_accum = 0.0;
    // A left press the in-frame menubar consumed: swallow its release.
    bool      swallow_release = false;
    // Text proxy state last sent to the page (-1 = never).
    int       ta_active = -1;
    int       ta_x = 0, ta_y = 0, ta_h = 0;
    bool      composing = false;
    // Double-click synthesis (the browser's dblclick comes after two clicks;
    // neui wants the second press itself).
    double    last_click_ms  = -1.0e9;
    float     last_click_x   = 0.0f;
    float     last_click_y   = 0.0f;
  };

  std::unordered_map<int, WebFrame*> g_frames;
  int      g_next_frame_id  = 1;
  int      g_appwindow_count = 0;
  bool     g_quit           = false;
  bool     g_apple          = false;

  enum : int {
    EV_RESIZE = 1, EV_FOCUS = 2, EV_BLUR = 3, EV_CLOSE = 4, EV_BACKDROP = 5,
    EV_PDOWN = 10, EV_PUP = 11, EV_PMOVE = 12, EV_PLEAVE = 13, EV_CONTEXT = 14, EV_WHEEL = 15,
    EV_KEYDOWN = 20, EV_KEYUP = 21,
    EV_SCHEME = 30, EV_PAGE = 31,
    EV_COMP_START = 40, EV_COMP_UPDATE = 41, EV_COMP_END = 42, EV_TEXT = 43,
    EV_DND_OVER = 50, EV_DND_LEAVE = 51, EV_DND_DROP = 52,
    EV_DRAG_MOVE = 60, EV_DRAG_UP = 61, EV_DRAG_CANCEL = 62,
    EV_MESSAGE = 70, EV_UNMOUNT = 71,
  };
  struct WebEvent
  {
    int type; int frame;
    double a, b, c, d, e, f;
    std::string code, key;   // key records only
  };
  std::deque<WebEvent> g_queue;

  // The animation heartbeat: one 16 ms tick shared by every frame while any
  // of them animates (as the Linux timerfd), 0 = idle.
  double g_next_tick_ms = 0.0;

  // The clipboard as neui last saw it: what neui wrote, or what a paste
  // event / an async read delivered. g_clip_fresh = it matches the system
  // clipboard for the record being dispatched (no new read needed).
  neui_detail::DataItem g_clip;
  bool g_clip_fresh = false;
  std::vector<char> g_text_buf(256);

  const char* clip_mime(int kind)
  {
    return kind == 1 ? NEUI_MIME_HTML : kind == 2 ? NEUI_MIME_PNG : NEUI_MIME_TEXT;
  }
  int clip_kind(const std::string& mime)
  {
    if (mime == NEUI_MIME_TEXT || mime == "text/plain") return 0;
    if (mime == NEUI_MIME_HTML) return 1;
    if (mime == NEUI_MIME_PNG)  return 2;
    return -1;
  }

  // A message box up: its own canvas + render context, not a WebFrame (no
  // widget, no session of its own). Boxes can stack (one opened from a
  // timer while another is up).
  struct WebMsgBox
  {
    int       id      = 0;
    Session*  session = nullptr;   // the owner's, for its palette
    neui_render_ctx_t ctx = nullptr;
    float     dpr     = 1.0f;
    neui_detail::MsgBoxPainted box;
    bool      need_paint = true;
    bool      running = true;
    int       result  = 0;
  };
  std::vector<WebMsgBox*> g_boxes;

  // Floating frames and boxes in the order they came up: a press on a
  // blocked frame's backdrop brings the newest one still open forward.
  std::vector<int> g_modal_order;

  // Set while a frame paints: a blocking call from a paint callback is
  // refused (the paint must finish before the stack may suspend).
  bool g_in_paint = false;

  // Drag and drop. g_dnd_over = the frame a drag (from outside or in-page)
  // is over, 0 = none: the browser enters the next canvas before it leaves
  // the last, so enter / leave are matched here, not per DOM event.
  int g_dnd_over = 0;
  // The drop being assembled by the page (neui_web_drop_*), and the files
  // the last drop wrote (kept until the next drop, so a client can still
  // open a path after the event).
  neui_detail::DataItem    g_drop;
  std::string              g_drop_uris;
  std::string              g_drop_dir;
  std::vector<std::string> g_drop_paths;
  int                      g_drop_serial = 0;
  // A dropped file larger than this is left out (it would be copied twice
  // into the wasm heap).
  constexpr size_t WEB_DROP_FILE_MAX = 256u * 1024u * 1024u;

  // Preview pixels from platform_make_drag_preview, consumed by begin_drag.
  struct DragPreview
  {
    std::vector<uint8_t> bgra;
    uint32_t w_px = 0, h_px = 0;
    float    scale = 1.0f;
  };

  // An in-page drag (begin_drag) in progress.
  struct WebDrag
  {
    neui_detail::DataItem*   item    = nullptr;
    uint32_t                 allowed = 0;
    std::vector<std::string> mimes;
    std::vector<const char*> fmts;
    uint32_t                 accepted = 0;   // the current target's answer
    uint32_t                 result   = 0;
    bool                     running  = true;
  };
  WebDrag* g_drag = nullptr;

  // Loops running now (run, a modal loop, the service loop); 0 = none, so a
  // shown frame needs the service loop (rule 7).
  int  g_loop_depth = 0;
  // Unmounting (neui-web.js app.unmount()): every frame closed for good, no
  // new frame or blocking call; g_ended once the page side was told.
  bool g_unmounting = false;
  bool g_ended      = false;
  uint32_t g_page_state = NEUI_WEB_PAGE_VISIBLE;
  // Mount selectors (NEUI_API_WEB set_mount, NEUI_API_EMBED set_parent),
  // copied; taken by the frame's creation at show.
  std::map<std::pair<const Session*, uint32_t>, std::string> g_mounts;

  // One wheel notch scrolls this many lines (the Win32 default, as on Linux).
  constexpr int WEB_WHEEL_LINES = 3;
  // A pixel-mode wheel delta this large is one notch (Chromium's mouse wheel).
  constexpr double WEB_PX_PER_NOTCH = 100.0;

  WebFrame* find_frame(int id)
  {
    auto it = g_frames.find(id);
    return it == g_frames.end() ? nullptr : it->second;
  }

  uint32_t dpi_for(float dpr)
  {
    long dpi = std::lround(static_cast<double>(dpr) * 96.0);
    return dpi > 0 ? static_cast<uint32_t>(dpi) : 96u;
  }

  bool any_window_animating()
  {
    for (auto& kv : g_frames) {
      const WebFrame* f = kv.second;
      if (f->toast_anim || f->clock_anim || f->bouncing_grid_index || f->bouncing_section_index)
        return true;
    }
    return false;
  }

  void arm_heartbeat()
  {
    if (g_next_tick_ms == 0.0) g_next_tick_ms = emscripten_get_now() + 16.0;
  }

  // ---- Painting (mirrors platform_linux.cpp) --------------------------------

  bool paint_pending(const WebFrame* f)
  {
    return f->needs_paint || !f->damage.empty();
  }

  void paint_window(WebFrame* f)
  {
    // The pending state is taken before painting, so an invalidation raised
    // DURING the paint is kept for the next frame.
    const bool full = f->needs_paint;
    neui_detail::DamageRegion damage = std::move(f->damage);
    f->needs_paint = false;
    f->damage.clear();
    if (!f->session) return;
    auto* wd = f->session->get_widget(f->widget_index);
    if (wd && wd->render_ctx) {
      g_in_paint = true;
      f->session->paint_frame(wd->render_ctx, f->widget_index, full ? nullptr : &damage);
      g_in_paint = false;
    }
  }

  void paint_box(WebMsgBox* b)
  {
    b->need_paint = false;
    auto* backend = platform_get_backend();
    if (!b->ctx || !backend) return;
    neui_detail::ScopedPaletteOverride scope(b->session ? b->session->effective_palette_ptr() : nullptr);
    b->box.render(backend, b->ctx);
  }

  bool any_paint_pending()
  {
    for (auto& kv : g_frames)
      if (paint_pending(kv.second)) return true;
    for (auto* b : g_boxes)
      if (b->need_paint) return true;
    return false;
  }

  void flush_pending_paints()
  {
    for (auto* b : g_boxes)
      if (b->need_paint) paint_box(b);
    // Snapshot ids: a paint callback can create / destroy frames.
    std::vector<int> todo;
    for (auto& kv : g_frames)
      if (paint_pending(kv.second)) todo.push_back(kv.first);
    for (int id : todo)
      if (auto* f = find_frame(id)) paint_window(f);
  }

  // ---- Animation (mirrors platform_linux.cpp) -------------------------------

  // Advance any in-flight GRID / SECTION spring-back by one 16 ms step.
  void step_scroll_bounce(WebFrame* f)
  {
    using namespace neui_detail;
    Session* s = f->session;
    if (f->bouncing_grid_index) {
      auto* hw = s->get_widget(f->bouncing_grid_index);
      GridModel* model = hw ? hw->grid_model_ptr() : nullptr;
      if (!model) {
        f->bouncing_grid_index = 0;
      } else {
        auto cfg = grid_read_config(hw->attrs.get());
        GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                                 cfg.row_h, cfg.header_h);
        bool more = grid_scroll_bounce_step(*model, vp, cfg.row_h);
        s->invalidate_widget_rect(f->bouncing_grid_index);
        if (!more) f->bouncing_grid_index = 0;
      }
    }
    if (f->bouncing_section_index) {
      auto* sw = s->get_widget(f->bouncing_section_index);
      SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
      const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
      if (!st || !L) {
        f->bouncing_section_index = 0;
      } else {
        bool more_v = section_scroll_bounce_step(*st, *L, false);
        bool more_h = section_scroll_bounce_step(*st, *L, true);
        s->invalidate_widget_rect(f->bouncing_section_index);
        sw->notify_scroll_changed();
        if (!more_v && !more_h) f->bouncing_section_index = 0;
      }
    }
  }

  void tick_animations()
  {
    std::vector<int> ids;
    for (auto& kv : g_frames) ids.push_back(kv.first);
    for (int id : ids) {
      WebFrame* f = find_frame(id);
      if (!f || !f->session) continue;
      if (f->toast_anim) f->needs_paint = true;
      step_scroll_bounce(f);
      if (f->clock_anim)
        f->clock_anim = f->session->animation_tick(f->widget_index) != 0;
    }
  }

  // Run the heartbeat if it is due; disarm it once nothing animates.
  void service_timers()
  {
    if (g_next_tick_ms == 0.0) return;
    const double now = emscripten_get_now();
    if (now < g_next_tick_ms) return;
    tick_animations();
    g_next_tick_ms = any_window_animating() ? now + 16.0 : 0.0;
  }

  // web_wait timeout: until the heartbeat is due, or none.
  int wait_timeout_ms(int cap_ms)
  {
    int t = -1;
    if (g_next_tick_ms != 0.0) {
      double d = g_next_tick_ms - emscripten_get_now();
      t = d <= 0.0 ? 0 : static_cast<int>(std::ceil(d));
    }
    if (cap_ms >= 0 && (t < 0 || t > cap_ms)) t = cap_ms;
    return t;
  }

  // ---- Window teardown (mirrors platform_linux.cpp) -------------------------

  void forget_modal(int id)
  {
    g_modal_order.erase(std::remove(g_modal_order.begin(), g_modal_order.end(), id),
                        g_modal_order.end());
  }

  // Title bar colours of a floating frame, from its session's palette.
  void style_chrome(int id, Session* s)
  {
    using neui_detail::ColorRole;
    neui_detail::ScopedPaletteOverride scope(s ? s->effective_palette_ptr() : nullptr);
    auto rgb = [](ColorRole r) {
      return static_cast<int>(neui_detail::current_palette().colors[static_cast<size_t>(r)] & 0xFFFFFFu);
    };
    web_frame_chrome(id, rgb(ColorRole::control_bg_alt), rgb(ColorRole::text_primary),
                     rgb(ColorRole::border), 0xC42B1C);
  }

  void destroy_native(WebFrame* f)
  {
    forget_modal(f->id);
    g_frames.erase(f->id);
    web_frame_destroy(f->id);
    delete f;
  }

  // The teardown every close path shares: releases the render ctx, ends a
  // dialog's modal pump (a modal dialog also gives its owner input and focus
  // back), removes the frame's DOM and drops the appwindow count.
  void teardown_window(WebFrame* f, WidgetData* wd)
  {
    Session* s = f->session;
    if (s && wd && wd->is_dialog() && wd->owner_index != 0 &&
        s->_widgets.exists(wd->owner_index)) {
      if (auto* fw = dynamic_cast<FrameWidget*>(wd))
        fw->end_modal_pump();   // widget_show's pump unwinds
      const bool modal = !wd->attrs || wd->attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;
      auto& owner = s->_widgets[wd->owner_index];
      if (modal && owner.native_handle) {
        platform_set_window_enabled(owner.native_handle, true);
        platform_activate_window(owner.native_handle);
      }
    }
    auto* backend = platform_get_backend();
    if (wd && wd->render_ctx && backend) {
      if (s) s->_asset_manager.release_context(wd->render_ctx, backend);
      backend->destroy_context(wd->render_ctx);
      wd->render_ctx = nullptr;
    }
    const bool was_app = f->is_appwindow && f->counted;
    if (wd) wd->native_handle = nullptr;
    destroy_native(f);
    if (was_app && g_appwindow_count > 0 && --g_appwindow_count <= 0)
      g_quit = true;
  }

  // Client gets APP_QUIT (and may veto), then the frame is torn down.
  // `gone` (unmount): the client still hears APP_QUIT but cannot keep it.
  void handle_close(WebFrame* f, bool gone = false)
  {
    Session* s = f->session;
    const int id = f->id;
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_APP_QUIT;
    if (auto* fw = s ? s->get_widget(f->widget_index) : nullptr)
      ev.data.mouse.widget.id = fw->widget_id;  // the frame closing
    const bool allow = !s || s->dispatch_event(&ev);
    // The handler may have destroyed the frame itself.
    f = find_frame(id);
    if (!f || (!allow && !gone)) return;
    teardown_window(f, s ? s->get_widget(f->widget_index) : nullptr);
  }

  // ---- Page interop (NEUI_API_WEB) -------------------------------------------

  void set_mount(const Session* s, uint32_t idx, const char* selector)
  {
    if (selector && *selector) g_mounts[{ s, idx }] = selector;
    else g_mounts.erase({ s, idx });
  }

  // NEUI_EVENT_WEB_MESSAGE / _PAGE to every live session; widget = its first
  // frame (the oldest), widget_none when it has none.
  void deliver_web(neui_event_type_t type, const std::string* text, uint32_t state)
  {
    const uint32_t n = session_count();
    for (uint32_t i = 1; i <= n; ++i) {
      Session* s = session_by_id(i);
      if (!s) continue;
      neui_event_t ev = {};
      ev.type = type;
      ev.data.web.widget.id = UINT32_MAX;
      int first = 0;
      for (auto& kv : g_frames) {
        if (kv.second->session != s || (first && kv.first > first)) continue;
        if (auto* wd = s->get_widget(kv.second->widget_index)) {
          first = kv.first;
          ev.data.web.widget.id = wd->widget_id;
        }
      }
      ev.data.web.data       = text ? text->c_str() : nullptr;
      ev.data.web.length     = text ? static_cast<uint32_t>(text->size()) : 0u;
      ev.data.web.page_state = state;
      s->dispatch_event(&ev);
    }
  }

  // Unmount: end every blocking loop and close every frame for good. The
  // loops see g_quit and unwind; run() returns to the client.
  void unmount_all()
  {
    g_unmounting = true;
    if (g_drag) g_drag->running = false;
    for (auto* b : g_boxes) { b->running = false; b->result = 0; }
    std::vector<int> ids;
    for (auto& kv : g_frames) ids.push_back(kv.first);
    for (int id : ids)
      if (WebFrame* f = find_frame(id)) handle_close(f, /*gone*/true);
    g_quit = true;
  }

  // ---- Pointer (mirrors platform_linux.cpp) ---------------------------------

  void send_mouse(WebFrame* f, neui_event_type_t type, uint32_t target,
                  float lx, float ly, uint32_t buttonmap)
  {
    if (target == 0) return;
    Session* s = f->session;
    auto* hw = s->get_widget(target);
    if (!hw) return;
    neui_event_t ev = {};
    ev.type                 = type;
    ev.data.mouse.widget    = { hw->widget_id };
    ev.data.mouse.x         = static_cast<int>(lx);
    ev.data.mouse.y         = static_cast<int>(ly);
    ev.data.mouse.buttonmap = buttonmap;
    s->dispatch_mouse_event(target, &ev);
  }

  void pointer_down(WebFrame* f, float lx, float ly, int button, uint32_t buttons,
                    uint32_t mods, double t_ms)
  {
    Session* s = f->session;
    const uint32_t bm = neui_detail::web_buttonmap(buttons, mods);
    if (button == 0) {
      // Overlay pre-checks, in the macOS / Linux order.
      if (s->_popup_active) { s->handle_popup_click(lx, ly); return; }
      if (s->handle_toast_click(f->widget_index, lx, ly)) return;
      if (s->handle_combo_click(lx, ly)) return;
      // The in-frame menubar owns clicks in its band and while open; swallow
      // the matching release so it can't reach a widget under the dropdown.
      if (s->handle_menubar_click(f->widget_index, lx, ly)) {
        f->swallow_release = true;
        return;
      }
      const bool dbl = (t_ms - f->last_click_ms <= 400.0) &&
                       std::fabs(lx - f->last_click_x) <= 4.0f &&
                       std::fabs(ly - f->last_click_y) <= 4.0f;
      f->last_click_ms = t_ms;
      f->last_click_x  = lx;
      f->last_click_y  = ly;
      uint32_t hit = s->widget_at(lx, ly, f->widget_index);
      if (dbl) {
        send_mouse(f, NEUI_EVENT_MOUSE_BUTTON_DBLCLICK, hit, lx, ly, bm | 0x0001);
        f->last_click_ms = -1.0e9;   // a 3rd press starts a fresh single click
        return;
      }
      s->set_focus(hit);
      s->set_pressed(hit);
      send_mouse(f, NEUI_EVENT_MOUSE_BUTTON_DOWN, hit, lx, ly, bm | 0x0001);
      return;
    }
    if (button == 2) {
      if (s->_popup_active) { s->handle_popup_click(lx, ly); return; }
      if (s->_menu_open) { s->close_menubar_menu(); return; }
      uint32_t hit = s->widget_at(lx, ly, f->widget_index);
      send_mouse(f, NEUI_EVENT_MOUSE_RBUTTON_DOWN, hit, lx, ly, bm | 0x0002);
    }
    // Middle button: no PRIMARY selection in a browser - nothing to paste.
  }

  void pointer_up(WebFrame* f, float lx, float ly, int button)
  {
    Session* s = f->session;
    if (button == 0) {
      if (f->swallow_release) { f->swallow_release = false; return; }
      if (s->_combo_sb_dragging) { s->_combo_sb_dragging = false; return; }
      uint32_t hit     = s->widget_at(lx, ly, f->widget_index);
      uint32_t pressed = s->_pressed_widget;
      s->set_pressed(0);
      if (hit == 0) return;
      auto* hw = s->get_widget(hit);
      if (!hw) return;
      neui_event_t ev = {};
      ev.data.mouse.widget = { hw->widget_id };
      ev.data.mouse.x      = static_cast<int>(lx);
      ev.data.mouse.y      = static_cast<int>(ly);
      ev.type = NEUI_EVENT_MOUSE_BUTTON_UP;
      s->dispatch_mouse_event(hit, &ev);
      if (hit == pressed) {   // CLICK only when up lands on the pressed widget
        ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
        s->dispatch_mouse_event(hit, &ev);
      }
      return;
    }
    if (button == 2) {
      uint32_t hit = s->widget_at(lx, ly, f->widget_index);
      if (hit == 0) return;
      auto* hw = s->get_widget(hit);
      if (!hw) return;
      neui_event_t ev = {};
      ev.type              = NEUI_EVENT_MOUSE_RBUTTON_UP;
      ev.data.mouse.widget = { hw->widget_id };
      ev.data.mouse.x      = static_cast<int>(lx);
      ev.data.mouse.y      = static_cast<int>(ly);
      s->dispatch_mouse_event(hit, &ev);
    }
  }

  void pointer_move(WebFrame* f, float lx, float ly, uint32_t buttons, uint32_t mods)
  {
    Session* s = f->session;
    uint32_t bm = neui_detail::web_buttonmap(buttons, mods);
    // A left drag is in progress whenever there is a pressed widget.
    if (s->_pressed_widget != 0) bm |= 0x0001;
    if (s->_popup_active) { s->handle_popup_hover(lx, ly); return; }
    if (s->_menu_open) {
      if (s->handle_menubar_hover(f->widget_index, lx, ly)) return;
    } else if (s->handle_menubar_band_hover(f->widget_index, lx, ly)) {
      return;   // hovering the closed band: highlight only
    }
    if (s->handle_combo_scroll_drag(ly)) return;
    if (s->handle_combo_hover(lx, ly)) return;

    uint32_t hit = s->widget_at(lx, ly, f->widget_index);
    s->set_hovered(hit);
    uint32_t target = hit;
    if (s->_pressed_widget != 0 && (bm & 0x0001))
      target = s->_pressed_widget;   // drags go to the pressed widget
    send_mouse(f, NEUI_EVENT_MOUSE_MOVE, target, lx, ly, bm);
  }

  void pointer_leave(WebFrame* f)
  {
    Session* s = f->session;
    if (s->_pressed_widget != 0) return;   // a captured drag continues outside
    s->set_hovered(0);
    if (s->_menu_band_hover != 0) { s->_menu_band_hover = 0; f->needs_paint = true; }
  }

  // ---- Wheel (mirrors platform_linux.cpp feed_scroll) -----------------------

  uint32_t find_scrolling_section(Session* s, uint32_t hit)
  {
    auto* hw = s->get_widget(hit);
    if (hw && hw->scroll_state_ptr()) return hit;
    for (uint32_t p : s->_widgets.get_all_parents(hit)) {
      auto* pw = s->get_widget(p);
      if (pw && pw->scroll_state_ptr()) return p;
    }
    return 0;
  }

  int take_lines(double& accum, double notches)
  {
    accum += notches;
    int lines = static_cast<int>(accum);   // toward zero
    accum -= lines;
    return lines;
  }

  // Feed a precise delta (logical px; +dv = up, +dh = left) into a scrolling
  // SECTION's kinetics. Browsers scroll smoothly, so smooth is the default.
  bool section_kinetic_wheel(WebFrame* f, uint32_t sec_idx, double dv, double dh)
  {
    using namespace neui_detail;
    auto* sw = f->session->get_widget(sec_idx);
    SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
    const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
    if (!st || !L) return false;
    bool has_v = section_axis_has_v(st->axis);
    bool has_h = section_axis_has_h(st->axis);
    // A horizontal-only section takes a pure vertical wheel.
    if (!has_v && has_h && dh == 0.0 && dv != 0.0) { dh = dv; dv = 0.0; }
    int  mode   = section_read_kinetics_mode(sw->attrs.get());
    bool smooth = scroll_kinetics_smooth_enabled(mode, /*platform_default_smooth=*/true);
    bool changed = false, start_bounce = false;
    if (smooth) {
      ScrollWheelAction av{}, ah{};
      if (has_v && dv != 0.0) {
        ScrollWheelInput in; in.precise = true; in.delta_px = dv;
        av = section_scroll_wheel_kinetic(*st, *L, in, false);
      }
      if (has_h && dh != 0.0) {
        ScrollWheelInput in; in.precise = true; in.delta_px = dh;
        ah = section_scroll_wheel_kinetic(*st, *L, in, true);
      }
      changed      = av.changed      || ah.changed;
      start_bounce = av.start_bounce || ah.start_bounce;
    } else {
      if (has_v && dv != 0.0 && section_scroll_step_px(*st, *L, dv, false)) changed = true;
      if (has_h && dh != 0.0 && section_scroll_step_px(*st, *L, dh, true))  changed = true;
    }
    if (changed) { f->session->invalidate_widget_rect(sec_idx); sw->notify_scroll_changed(); }
    if (start_bounce) { f->bouncing_section_index = sec_idx; arm_heartbeat(); }
    return (has_v && dv != 0.0) || (has_h && dh != 0.0);
  }

  // Route one wheel event. dv / dh are notches (+dv = up, +dh = left); pv /
  // ph the same scroll in logical px when the browser reported pixels (the
  // precise paths use them as they are), else 0 and the notches decide.
  // Returns whether neui consumed it (else the page may scroll).
  bool feed_scroll(WebFrame* f, float lx, float ly, double dv, double dh,
                   double pv, double ph, bool have_px)
  {
    using namespace neui_detail;
    Session* s = f->session;
    int vline = take_lines(f->scroll_v_accum, dv);
    int hline = take_lines(f->scroll_h_accum, dh);

    if (s->_open_combo && vline != 0 && s->handle_combo_wheel(lx, ly, vline)) return true;

    uint32_t hit = s->widget_at(lx, ly, f->widget_index);
    if (hit == 0) return false;
    auto* hw = s->get_widget(hit);
    if (!hw) return false;

    // GRID + SMOOTH: the shared kinetics, pixel-precise.
    if (GridModel* model = hw->grid_model_ptr()) {
      auto cfg = grid_read_config(hw->attrs.get());
      if (grid_smooth_enabled(cfg, /*platform_default_smooth=*/true)) {
        GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                                 cfg.row_h, cfg.header_h);
        GridWheelInput in;
        in.precise  = true;
        in.delta_px = have_px ? pv : dv * static_cast<double>(WEB_WHEEL_LINES) * cfg.row_h;
        GridWheelAction act = grid_scroll_wheel(*model, vp, cfg.row_h, in);
        if (act.changed) s->invalidate_widget_rect(hit);
        if (act.start_bounce) { f->bouncing_grid_index = hit; arm_heartbeat(); }
        return true;
      }
    }

    // Scrolling SECTION: widgets inside get first refusal (classic lines);
    // otherwise the section scrolls.
    uint32_t sec_idx = find_scrolling_section(s, hit);
    if (sec_idx != 0) {
      if (hit != sec_idx && (vline != 0 || hline != 0)) {
        neui_event_t ev = {};
        ev.type         = NEUI_EVENT_MOUSE_WHEEL;
        ev.data.wheel.x = static_cast<int>(lx);
        ev.data.wheel.y = static_cast<int>(ly);
        if (vline != 0) {
          ev.data.wheel.delta = vline; ev.data.wheel.is_horizontal = 0;
          if (s->dispatch_wheel_event(hit, &ev, sec_idx)) return true;
        }
        if (hline != 0) {
          ev.data.wheel.delta = hline; ev.data.wheel.is_horizontal = 1;
          if (s->dispatch_wheel_event(hit, &ev, sec_idx)) return true;
        }
      }
      const double line_px = static_cast<double>(WEB_WHEEL_LINES) * SECTION_WHEEL_LINE_PX;
      return section_kinetic_wheel(f, sec_idx, have_px ? pv : dv * line_px,
                                   have_px ? ph : dh * line_px);
    }

    // Classic line path (listbox / treeview / multiline / custom).
    bool consumed = false;
    if (vline != 0) {
      neui_event_t ev = {};
      ev.type = NEUI_EVENT_MOUSE_WHEEL;
      ev.data.wheel.x = static_cast<int>(lx); ev.data.wheel.y = static_cast<int>(ly);
      ev.data.wheel.delta = vline; ev.data.wheel.is_horizontal = 0;
      consumed |= s->dispatch_wheel_event(hit, &ev);
    }
    if (hline != 0) {
      neui_event_t ev = {};
      ev.type = NEUI_EVENT_MOUSE_WHEEL;
      ev.data.wheel.x = static_cast<int>(lx); ev.data.wheel.y = static_cast<int>(ly);
      ev.data.wheel.delta = hline; ev.data.wheel.is_horizontal = 1;
      consumed |= s->dispatch_wheel_event(hit, &ev);
    }
    return consumed;
  }

  // A WheelEvent: deltaMode 0 = pixels, 1 = lines, 2 = pages; positive
  // deltaY = down, positive deltaX = right (neui's notches run the other way).
  bool handle_wheel(WebFrame* f, float lx, float ly, double dx, double dy, int mode)
  {
    double dv = 0.0, dh = 0.0, pv = 0.0, ph = 0.0;
    bool have_px = false;
    if (mode == 0) {
      have_px = true;
      pv = -dy; ph = -dx;
      dv = pv / WEB_PX_PER_NOTCH; dh = ph / WEB_PX_PER_NOTCH;
    } else {
      const double lines_per = (mode == 1) ? 1.0 : 10.0;   // a page ~ ten lines
      dv = -dy * lines_per / WEB_WHEEL_LINES;
      dh = -dx * lines_per / WEB_WHEEL_LINES;
    }
    if (dv == 0.0 && dh == 0.0) return false;
    return feed_scroll(f, lx, ly, dv, dh, pv, ph, have_px);
  }

  // ---- Keyboard (mirrors platform_linux.cpp dispatch_key_press) -------------

  // KEYCHAR to the focused widget: client first, then the widget. Returns
  // whether a focused widget received it.
  bool send_char(Session* s, uint32_t cp, uint32_t mods)
  {
    uint32_t fw = s->_focused_widget;
    if (fw == 0 || !s->_widgets.exists(fw)) return false;
    auto& wd = s->_widgets[fw];
    bool char_consumed = false;
    if (wd.emit_events) {
      neui_event_t ev = {};
      ev.type     = NEUI_EVENT_KEYCHAR;
      ev.data.key = { { wd.widget_id }, cp, mods };
      char_consumed = s->dispatch_event(&ev);
    }
    if (!char_consumed) s->handle_input_key(NEUI_EVENT_KEYCHAR, cp, mods);
    return true;
  }

  // The focused widget when it is a text widget of this frame, else null.
  WidgetData* focused_text_widget(WebFrame* f)
  {
    Session* s = f->session;
    if (!s) return nullptr;
    uint32_t fw = s->_focused_widget;
    if (fw == 0 || !s->_widgets.exists(fw)) return nullptr;
    auto& wd = s->_widgets[fw];
    if (!wd.is_text_input() || s->find_parent_native_handle(fw) != f) return nullptr;
    return &wd;
  }

  // Text arriving without a key (virtual keyboard, emoji picker): one
  // KEYCHAR per codepoint.
  void text_input(WebFrame* f, const std::string& utf8)
  {
    const auto* p = reinterpret_cast<const unsigned char*>(utf8.data());
    size_t left = utf8.size();
    while (left > 0) {
      const uint32_t b = p[0];
      size_t n = (b < 0x80) ? 1 : ((b & 0xE0) == 0xC0) ? 2 : ((b & 0xF0) == 0xE0) ? 3
               : ((b & 0xF8) == 0xF0) ? 4 : 0;
      if (n == 0 || n > left) break;   // the browser hands over valid UTF-8
      uint32_t cp = (n == 1) ? b : (b & (0x7Fu >> n));
      for (size_t i = 1; i < n; ++i) cp = (cp << 6) | (p[i] & 0x3Fu);
      p += n;
      left -= n;
      if (cp >= 0x20 && cp != 0x7F) send_char(f->session, cp, 0);
    }
  }

  // Composition events (mirrors platform_win32.cpp's WM_IME_* translation;
  // the browser reports no clause attributes).
  void composition(WebFrame* f, int type, const std::string& utf8, int caret)
  {
    WidgetData* tw = focused_text_widget(f);
    if (!tw) { f->composing = false; return; }
    const int len = static_cast<int>(utf8.size());
    if (type == EV_COMP_START) {
      f->composing = tw->on_composition(WidgetData::COMP_START, nullptr, 0, 0, nullptr);
      return;
    }
    if (!f->composing)   // an update without a start: start now
      f->composing = tw->on_composition(WidgetData::COMP_START, nullptr, 0, 0, nullptr);
    if (type == EV_COMP_UPDATE) {
      tw->on_composition(WidgetData::COMP_UPDATE, utf8.c_str(), len, caret, nullptr);
      return;
    }
    if (len > 0) tw->on_composition(WidgetData::COMP_RESULT, utf8.c_str(), len, 0, nullptr);
    tw->on_composition(WidgetData::COMP_END, nullptr, 0, 0, nullptr);
    f->composing = false;
  }

  // Keep each frame's text proxy on its focused text widget's caret.
  void sync_text_proxy()
  {
    auto* backend = platform_get_backend();
    for (auto& kv : g_frames) {
      WebFrame* f = kv.second;
      int active = 0, x = 0, y = 0, h = 0;
      if (WidgetData* tw = focused_text_widget(f)) {
        active = 1;
        auto* frame_wd = f->session->get_widget(f->widget_index);
        float lx = 0.0f, ly = 0.0f, lh = 0.0f;
        if (backend && frame_wd && frame_wd->render_ctx &&
            tw->caret_rect_local(backend, frame_wd->render_ctx, &lx, &ly, &lh)) {
          x = tw->abs_x + static_cast<int>(lx);
          y = tw->abs_y + static_cast<int>(ly);
          h = static_cast<int>(lh);
        } else {
          x = tw->abs_x; y = tw->abs_y; h = tw->height;
        }
      }
      if (active == f->ta_active && (!active || (x == f->ta_x && y == f->ta_y && h == f->ta_h)))
        continue;
      f->ta_active = active; f->ta_x = x; f->ta_y = y; f->ta_h = h;
      web_text_proxy(f->id, active, x, y, h);
    }
  }

  // Returns whether neui consumed the press (its default action is then
  // prevented: no Tab focus move, no page scroll on Space / arrows, no
  // browser shortcut).
  bool key_down(WebFrame* f, const std::string& code, const std::string& key, uint32_t web_mods)
  {
    Session* s = f->session;
    const uint32_t mods    = neui_detail::web_modifiers_to_neui(web_mods, g_apple);
    const uint32_t keycode = neui_detail::web_key_to_neui(code.c_str(), key.c_str());

    // Tab cycles logical focus.
    if (keycode == NEUI_KEY_TAB) {
      s->focus_next(!(mods & NEUI_KMOD_SHIFT));
      return true;
    }
    // An open in-frame menubar captures the keyboard.
    if (s->_menu_open) { s->handle_menubar_key(keycode, mods); return true; }
    // Menubar accelerators.
    if (keycode != 0 && s->try_menubar_accel(keycode, mods, f->widget_index)) return true;

    bool consumed = false;
    // KEYDOWN: client first (focused widget), then the widget's on_keydown.
    if (keycode != 0) {
      uint32_t fw = s->_focused_widget;
      if (fw != 0 && s->_widgets.exists(fw)) {
        auto& wd = s->_widgets[fw];
        if (wd.emit_events) {
          neui_event_t ev = {};
          ev.type     = NEUI_EVENT_KEYDOWN;
          ev.data.key = { { wd.widget_id }, keycode, mods };
          consumed = s->dispatch_event(&ev);
        }
        if (!consumed)
          consumed = s->handle_input_key(NEUI_EVENT_KEYDOWN, keycode, mods);
      }
    }

    // KEYCHAR for the typed character (none while the primary modifier is
    // held: those are shortcuts, as on Linux / macOS).
    const uint32_t cp = neui_detail::web_key_char(key.c_str(), mods);
    if (cp == 0) return consumed;
    return send_char(s, cp, mods) || consumed;
  }

  void key_up(WebFrame* f, const std::string& code, const std::string& key, uint32_t web_mods)
  {
    Session* s = f->session;
    const uint32_t keycode = neui_detail::web_key_to_neui(code.c_str(), key.c_str());
    if (keycode == 0) return;
    const uint32_t mods = neui_detail::web_modifiers_to_neui(web_mods, g_apple);
    uint32_t fw = s->_focused_widget;
    if (fw == 0 || !s->_widgets.exists(fw)) return;
    auto& wd = s->_widgets[fw];
    if (!wd.emit_events) return;
    neui_event_t ev = {};
    ev.type     = NEUI_EVENT_KEYUP;
    ev.data.key = { { wd.widget_id }, keycode, mods };
    s->dispatch_event(&ev);
  }

  // ---- Queue -----------------------------------------------------------------

  // The browser reports the CSS size (= logical px) and, where supported, the
  // exact device-pixel size of the canvas. A device-pixel-ratio change (zoom,
  // another monitor) arrives the same way.
  void handle_resize(WebFrame* f, double css_w, double css_h, double dev_w, double dev_h)
  {
    Session* s = f->session;
    auto* wd = s ? s->get_widget(f->widget_index) : nullptr;
    if (!wd) return;
    const float dpr = static_cast<float>(web_dpr());
    uint32_t pw = dev_w > 0.0 ? static_cast<uint32_t>(dev_w)
                              : static_cast<uint32_t>(std::lround(css_w * dpr));
    uint32_t ph = dev_h > 0.0 ? static_cast<uint32_t>(dev_h)
                              : static_cast<uint32_t>(std::lround(css_h * dpr));
    if (pw == 0) pw = 1;
    if (ph == 0) ph = 1;
    if (dpr != f->dpr) {
      f->dpr = dpr;
      s->on_dpi_changed(f->widget_index, dpi_for(dpr));
      if (wd->render_ctx) neui_canvas2d_backend::set_scale(wd->render_ctx, dpr);
    }
    s->resize_render_ctx(f->widget_index, pw, ph);
    const int wlog = static_cast<int>(std::lround(css_w));
    const int hlog = static_cast<int>(std::lround(css_h));
    if (wd->width != wlog || wd->height != hlog) {
      wd->width  = wlog;
      wd->height = hlog;
      // The in-frame menubar band reserves the top of the client area;
      // report the height below it (as Linux and Win32 do).
      neui_event_t re = {};
      re.type               = NEUI_EVENT_RESIZE;
      re.data.resize.widget = { wd->widget_id };
      re.data.resize.width  = wlog;
      re.data.resize.height = hlog - s->frame_top_inset(f->widget_index);
      s->dispatch_event(&re);
    }
    if (auto* g = find_frame(f->id)) g->needs_paint = true;
  }

  // Input on a message box (mirrors the platform_linux.cpp box loop).
  // Returns whether the default action is prevented.
  bool box_event(WebMsgBox* b, const WebEvent& e)
  {
    auto& box = b->box;
    const int lx = static_cast<int>(e.a), ly = static_cast<int>(e.b);
    switch (e.type) {
      case EV_RESIZE: {
        auto* backend = platform_get_backend();
        const float dpr = static_cast<float>(web_dpr());
        uint32_t pw = e.c > 0.0 ? static_cast<uint32_t>(e.c) : static_cast<uint32_t>(std::lround(e.a * dpr));
        uint32_t ph = e.d > 0.0 ? static_cast<uint32_t>(e.d) : static_cast<uint32_t>(std::lround(e.b * dpr));
        if (backend && b->ctx) {
          if (dpr != b->dpr) {
            b->dpr = dpr;
            backend->update_dpi(b->ctx, dpi_for(dpr));
            neui_canvas2d_backend::set_scale(b->ctx, dpr);
          }
          backend->resize(b->ctx, pw ? pw : 1, ph ? ph : 1);
        }
        b->need_paint = true;
        return false;
      }
      case EV_PMOVE: {
        int h = box.hit_button(lx, ly);
        if (h != box.hover) { box.hover = h; b->need_paint = true; }
        return false;
      }
      case EV_PLEAVE:
        if (box.hover != -1) { box.hover = -1; b->need_paint = true; }
        return false;
      case EV_PDOWN:
        if (static_cast<int>(e.c) == 0) { box.pressed = box.hit_button(lx, ly); b->need_paint = true; }
        return false;
      case EV_PUP:
        if (static_cast<int>(e.c) == 0) {
          int h = box.hit_button(lx, ly);
          if (h >= 0 && h == box.pressed) { b->result = box.spec.btn[h].id; b->running = false; }
          box.pressed = -1;
          b->need_paint = true;
        }
        return false;
      case EV_KEYDOWN: {
        const uint32_t k = neui_detail::web_key_to_neui(e.code.c_str(), e.key.c_str());
        if (k == NEUI_KEY_RETURN) { b->result = box.default_result(); b->running = false; }
        else if (k == NEUI_KEY_ESCAPE && box.spec.cancel_index >= 0) {
          b->result = box.cancel_result(); b->running = false;
        }
        return true;   // modal: no key reaches the page behind the box
      }
      case EV_CLOSE:
        b->result = box.cancel_result();
        b->running = false;
        return false;
      case EV_CONTEXT:
        return true;
      default:
        return false;
    }
  }

  // ---- Drag and drop -----------------------------------------------------------

  // The drag left the frame it was over (or ended there without a drop).
  void dnd_out()
  {
    if (WebFrame* o = find_frame(g_dnd_over))
      if (o->session) o->session->dispatch_dnd_leave();
    g_dnd_over = 0;
  }

  // The drag is over frame f (nullptr = none) at frame-local (x, y). Enter
  // when it comes from elsewhere, else move. Returns the accepted action.
  uint32_t dnd_over(WebFrame* f, float x, float y, const std::vector<const char*>& fmts,
                    uint32_t suggested)
  {
    if (!f || !f->session || f->input_disabled) { dnd_out(); return 0; }
    const int lx = static_cast<int>(x), ly = static_cast<int>(y);
    const char* const* p = fmts.empty() ? nullptr : fmts.data();
    const auto n = static_cast<uint32_t>(fmts.size());
    if (g_dnd_over != f->id) {
      dnd_out();
      g_dnd_over = f->id;
      return f->session->dispatch_dnd_enter(f->widget_index, lx, ly, p, n, suggested, 0);
    }
    return f->session->dispatch_dnd_move(f->widget_index, lx, ly, p, n, suggested, 0);
  }

  // Drop on frame f: a last move at the drop point (as XDND's position
  // before drop), then the drop if the target still accepts.
  uint32_t dnd_drop(WebFrame* f, float x, float y, const std::vector<const char*>& fmts,
                    uint32_t suggested, neui_detail::DataItem* item)
  {
    if (dnd_over(f, x, y, fmts, suggested) == 0) { dnd_out(); return 0; }
    g_dnd_over = 0;
    return f->session->dispatch_dnd_drop(f->widget_index, static_cast<int>(x), static_cast<int>(y),
                                         fmts.empty() ? nullptr : fmts.data(),
                                         static_cast<uint32_t>(fmts.size()), suggested, 0, item);
  }

  // The page's modifier bits (1 shift, 2 ctrl) -> the shared convention.
  uint32_t dnd_suggest(uint32_t allowed, uint32_t web_mods)
  {
    return neui_detail::dnd_suggest_action(allowed, (web_mods & 2u) != 0, (web_mods & 1u) != 0);
  }

  // A drag record's formats: one MIME per line.
  void split_formats(const std::string& s, std::vector<std::string>& mimes,
                     std::vector<const char*>& fmts)
  {
    mimes.clear();
    fmts.clear();
    size_t i = 0;
    while (i < s.size()) {
      size_t end = s.find('\n', i);
      if (end == std::string::npos) end = s.size();
      if (end > i) mimes.emplace_back(s, i, end - i);
      i = end + 1;
    }
    for (auto& m : mimes) fmts.push_back(m.c_str());
  }

  // A drag from outside (the desktop, the page): enter / over / leave / drop.
  void external_dnd(WebFrame* f, const WebEvent& e)
  {
    std::vector<std::string> mimes;
    std::vector<const char*> fmts;
    split_formats(e.key, mimes, fmts);
    const auto allowed   = static_cast<uint32_t>(e.d);
    const uint32_t sugg  = dnd_suggest(allowed, static_cast<uint32_t>(e.c));
    const float x = static_cast<float>(e.a), y = static_cast<float>(e.b);
    if (e.type == EV_DND_OVER) {
      const uint32_t act = dnd_over(f, x, y, fmts, sugg) & allowed;
      web_dnd_verdict(e.frame, static_cast<int>(e.e), static_cast<int>(act));
    } else if (e.type == EV_DND_LEAVE) {
      if (g_dnd_over == e.frame) dnd_out();
    } else {
      // Files arrive as text/uri-list next to any list the drag carried.
      if (!g_drop_uris.empty()) {
        std::string list;
        const int n = g_drop.get_format(NEUI_MIME_URI_LIST, nullptr, 0);
        if (n > 0) {
          list.resize(static_cast<size_t>(n));
          g_drop.get_format(NEUI_MIME_URI_LIST, &list[0], n);
          if (list.back() != '\n') list += "\r\n";
        }
        list += g_drop_uris;
        g_drop.set_format(NEUI_MIME_URI_LIST, list.data(), static_cast<uint32_t>(list.size()));
      }
      dnd_drop(f, x, y, fmts, sugg, &g_drop);
      g_drop = neui_detail::DataItem();
      g_drop_uris.clear();
    }
  }

  // An in-page drag record (begin_drag's loop). frame = the neui frame under
  // the pointer, 0 = none.
  void drag_record(const WebEvent& e)
  {
    WebDrag* d = g_drag;
    WebFrame* f = find_frame(e.frame);
    const float x = static_cast<float>(e.a), y = static_cast<float>(e.b);
    const uint32_t sugg = dnd_suggest(d->allowed, static_cast<uint32_t>(e.c));
    if (e.type == EV_DRAG_MOVE) {
      d->accepted = dnd_over(f, x, y, d->fmts, sugg) & d->allowed;
      web_drag_cursor(static_cast<int>(d->accepted));
      return;
    }
    if (e.type == EV_DRAG_UP && f)
      d->result = dnd_drop(f, x, y, d->fmts, sugg, d->item) & d->allowed;
    else
      dnd_out();
    d->running = false;
  }

  // The file part of a drop: written to /tmp/neui-drop/<n>/<name> and listed
  // as a file:// URI (RFC 2483, percent-encoded).
  void drop_file(const std::string& name, const std::string& type, const uint8_t* data, size_t len)
  {
    if (len > WEB_DROP_FILE_MAX) {
      std::fprintf(stderr, "neui: dropped file '%s' too large, left out\n", name.c_str());
      return;
    }
    std::string base = name;
    for (char& c : base) if (c == '/' || c == '\\') c = '_';
    if (base.empty() || base == "." || base == "..") base = "file";
    std::string path = g_drop_dir + "/" + base;
    for (int k = 2; access(path.c_str(), F_OK) == 0; ++k)   // two files, one name
      path = g_drop_dir + "/" + std::to_string(k) + "-" + base;
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return;
    const bool ok = len == 0 || std::fwrite(data, 1, len, fp) == len;
    std::fclose(fp);
    if (!ok) { std::remove(path.c_str()); return; }
    g_drop_paths.push_back(path);
    std::string uri = "file://";
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : path) {
      if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
        uri += static_cast<char>(c);
      } else {
        uri += '%'; uri += hex[c >> 4]; uri += hex[c & 15];
      }
    }
    g_drop_uris += uri + "\r\n";
    // The first file of each type also under its own MIME type.
    if (!type.empty() && type.find('/') != std::string::npos &&
        g_drop.get_format(type, nullptr, 0) <= 0)
      g_drop.set_format(type, data, static_cast<uint32_t>(len));
  }

  void drain_queue()
  {
    while (!g_queue.empty()) {
      WebEvent e = std::move(g_queue.front());
      g_queue.pop_front();
      // A held Ctrl/Cmd+V comes with the clipboard its paste event read.
      g_clip_fresh = (e.type == EV_KEYDOWN && e.c != 0.0);

      if (e.type == EV_SCHEME) {   // page-wide, no frame
        neui_detail::theme_apply_web_scheme(static_cast<int>(e.a));
        neui_detail::broadcast_theme_change();
        for (auto& kv : g_frames)
          if (kv.second->is_dialog) style_chrome(kv.first, kv.second->session);
        for (auto* b : g_boxes) { style_chrome(b->id, b->session); b->need_paint = true; }
        continue;
      }
      if (e.type == EV_PAGE) {   // page-wide, no frame
        g_page_state = static_cast<uint32_t>(e.a);
        deliver_web(NEUI_EVENT_WEB_PAGE, nullptr, g_page_state);
        continue;
      }
      if (e.type == EV_MESSAGE) {
        deliver_web(NEUI_EVENT_WEB_MESSAGE, &e.key, 0);
        continue;
      }
      if (e.type == EV_UNMOUNT) {
        unmount_all();
        continue;
      }
      if (e.type == EV_BACKDROP) {   // bring the newest modal forward
        if (!g_modal_order.empty()) web_frame_focus(g_modal_order.back());
        continue;
      }
      // An in-page drag owns the pointer: its records drive it, and the
      // frames' own pointer records (none should come) are dropped.
      if (e.type >= EV_DRAG_MOVE && e.type <= EV_DRAG_CANCEL) {
        if (g_drag) drag_record(e);
        continue;
      }
      if (g_drag && e.type >= EV_PDOWN && e.type <= EV_PLEAVE) continue;

      // Events that carry a default action get their verdict here, also when
      // the frame is gone or blocked (then: leave it to the browser).
      int verdict_id = 0;
      if (e.type == EV_CONTEXT) verdict_id = static_cast<int>(e.c);
      else if (e.type == EV_WHEEL) verdict_id = static_cast<int>(e.f);
      else if (e.type == EV_KEYDOWN) verdict_id = static_cast<int>(e.a);
      bool consumed = false;

      WebMsgBox* box = nullptr;
      for (auto* b : g_boxes) if (b->id == e.frame) box = b;
      if (box) {
        consumed = box_event(box, e);
        if (verdict_id) web_verdict(verdict_id, consumed ? 1 : 0);
        continue;
      }

      WebFrame* f = find_frame(e.frame);
      Session* s = f ? f->session : nullptr;
      const bool blocked = f && f->input_disabled;
      if (f && s) {
        const float x = static_cast<float>(e.a), y = static_cast<float>(e.b);
        switch (e.type) {
          case EV_RESIZE:
            handle_resize(f, e.a, e.b, e.c, e.d);
            break;
          case EV_FOCUS:
            s->_os_focused = true;
            s->frame_activated(f->widget_index);
            f->needs_paint = true;
            break;
          case EV_BLUR:
            s->_os_focused = false;
            s->close_menubar_menu();
            f->needs_paint = true;
            break;
          case EV_CLOSE:
            if (!blocked) handle_close(f);
            break;
          case EV_PDOWN:
            // A press in a frame other than the one an open popup belongs
            // to dismisses the popup (the coordinates are this frame's).
            if (!blocked && s->_popup_active && s->_popup_frame != f->widget_index)
              s->handle_popup_click(-1.0e6f, -1.0e6f);
            else if (!blocked)
              pointer_down(f, x, y, static_cast<int>(e.c), static_cast<uint32_t>(e.d),
                           static_cast<uint32_t>(e.e), e.f);
            break;
          case EV_PUP:
            if (!blocked) pointer_up(f, x, y, static_cast<int>(e.c));
            break;
          case EV_PMOVE:
            if (!blocked && !(s->_popup_active && s->_popup_frame != f->widget_index))
              pointer_move(f, x, y, static_cast<uint32_t>(e.d), static_cast<uint32_t>(e.e));
            break;
          case EV_PLEAVE:
            pointer_leave(f);
            break;
          case EV_CONTEXT:
            // neui handles the right button itself (RBUTTON_DOWN / UP, the
            // knob's popup); the browser's menu only where neui has nothing.
            consumed = blocked || s->_popup_active || s->widget_at(x, y, f->widget_index) != 0;
            break;
          case EV_WHEEL:
            consumed = blocked ? true
                               : handle_wheel(f, x, y, e.c, e.d, static_cast<int>(e.e));
            break;
          case EV_KEYDOWN:
            if (!blocked) consumed = key_down(f, e.code, e.key, static_cast<uint32_t>(e.b));
            break;
          case EV_KEYUP:
            if (!blocked) key_up(f, e.code, e.key, static_cast<uint32_t>(e.b));
            break;
          case EV_COMP_START:
          case EV_COMP_UPDATE:
          case EV_COMP_END:
            if (!blocked) composition(f, e.type, e.key, static_cast<int>(e.c));
            break;
          case EV_TEXT:
            if (!blocked) text_input(f, e.key);
            break;
          case EV_DND_OVER:
          case EV_DND_LEAVE:
          case EV_DND_DROP:
            external_dnd(f, e);   // a blocked frame refuses (dnd_over)
            break;
          default: break;
        }
      } else if (e.type == EV_DND_OVER) {
        web_dnd_verdict(e.frame, static_cast<int>(e.e), 0);   // the frame is gone
      }
      if (verdict_id) web_verdict(verdict_id, consumed ? 1 : 0);
    }
  }

  // ---- Metrics seam ----------------------------------------------------------

  // NEUI_API_METRICS text width through the browser's own shaping (the
  // desktop default is an average-glyph estimate).
  int metrics_measure_web(neui_session_t /*session*/, const char* text, const char* family,
                          float size_px, int weight)
  {
    if (!text || !*text) return 0;
    if (size_px <= 0.0f) size_px = neui_detail::METRIC_BASE_BODY_FONT * neui_detail::painted_ui_scale();
    static neui_render_ctx_t ctx = nullptr;
    auto* be = platform_get_backend();
    if (!ctx) ctx = be->create_offscreen_context(1, 1, 1.0f);
    if (!ctx) return neui_detail::metrics_measure_text_default(neui_session_t{}, text, family, size_px, weight);
    be->push_font(ctx, family, weight);
    const float w = be->measure_text(ctx, text, -1, size_px);
    be->pop_font(ctx);
    return static_cast<int>(std::ceil(w));
  }

  // ---- Frame creation ---------------------------------------------------------

  void create_frame(Session* session, uint32_t widget_index, WidgetData& wd,
                    int kind, bool is_appwindow, WebFrame* owner)
  {
    platform_init();
    if (g_unmounting) return;   // the app is leaving the page: no new frame
    auto* f = new WebFrame();
    f->id = g_next_frame_id++;
    f->session = session;
    f->widget_index = widget_index;
    f->is_appwindow = is_appwindow;
    f->is_dialog = (kind == 2);
    f->dpr = static_cast<float>(web_dpr());
    g_frames[f->id] = f;

    const int w = wd.width  > 0 ? wd.width  : 1;
    const int h = wd.height > 0 ? wd.height : 1;
    std::string mount;
    auto mit = g_mounts.find({ session, widget_index });
    if (mit != g_mounts.end()) { mount = mit->second; g_mounts.erase(mit); }
    web_frame_create(f->id, kind, wd.x, wd.y, w, h, mount.empty() ? nullptr : mount.c_str());
    if (!wd.text.empty()) web_frame_title(f->id, wd.text.c_str());
    if (f->is_dialog) {
      if (owner) {
        f->owner_id = owner->id;
        auto* ow = session->get_widget(owner->widget_index);
        web_frame_owner(f->id, owner->id, ow ? ow->x : 0, ow ? ow->y : 0, 0);
      }
      style_chrome(f->id, session);
    }

    wd.native_handle = f;
    wd.dpi = dpi_for(f->dpr);

    auto* backend = platform_get_backend();
    if (backend) {
      const auto pw = static_cast<uint32_t>(std::lround(static_cast<double>(w) * f->dpr));
      const auto ph = static_cast<uint32_t>(std::lround(static_cast<double>(h) * f->dpr));
      wd.render_ctx = backend->create_context(reinterpret_cast<void*>(static_cast<uintptr_t>(f->id)),
                                              pw ? pw : 1, ph ? ph : 1);
      if (wd.render_ctx) {
        backend->update_dpi(wd.render_ctx, wd.dpi);
        neui_canvas2d_backend::set_scale(wd.render_ctx, f->dpr);
      }
    }

    // Pre-show attribute-driven size constraints.
    if (wd.attrs) {
      int min_w = wd.attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0);
      int min_h = wd.attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0);
      int max_w = wd.attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0);
      int max_h = wd.attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0);
      if (min_w || min_h || max_w || max_h)
        platform_apply_size_constraints(f, min_w, min_h, max_w, max_h);
    }

    f->needs_paint = true;
    if (is_appwindow) { ++g_appwindow_count; f->counted = true; }
  }

} // namespace

// Records from the DOM listeners (Module.neuiWeb.push). Consecutive pointer
// moves on one frame collapse into the latest: a drag delivers one move per
// loop turn instead of one per input sample.
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_push(int type, int frame, double a, double b,
                                                   double c, double d, double e, double f)
{
  if ((type == EV_PMOVE || type == EV_DRAG_MOVE) && !g_queue.empty()) {
    WebEvent& last = g_queue.back();
    if (last.type == type && last.frame == frame) {
      last.a = a; last.b = b; last.c = c; last.d = d; last.e = e; last.f = f;
      return;
    }
  }
  g_queue.push_back({ type, frame, a, b, c, d, e, f, std::string(), std::string() });
}

// Key records carry the KeyboardEvent's code and key strings, which the
// listener writes into this buffer just before neui_web_push_key.
static char g_key_buf[128];
extern "C" EMSCRIPTEN_KEEPALIVE char* neui_web_key_buf() { return g_key_buf; }

extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_push_key(int type, int frame, int ev_id, int mods,
                                                       int fresh)
{
  g_key_buf[63] = g_key_buf[127] = '\0';
  g_queue.push_back({ type, frame, static_cast<double>(ev_id), static_cast<double>(mods),
                      static_cast<double>(fresh), 0, 0, 0,
                      std::string(g_key_buf), std::string(g_key_buf + 64) });
}

// Strings of any length (composition text, clipboard bytes): the page
// writes them into this buffer first.
extern "C" EMSCRIPTEN_KEEPALIVE char* neui_web_text_buf(int size)
{
  if (size < 1) size = 1;
  if (g_text_buf.size() < static_cast<size_t>(size)) g_text_buf.resize(static_cast<size_t>(size));
  return g_text_buf.data();
}

// A composition / text record; the string (len bytes) is in the text buffer.
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_push_text(int type, int frame, int len, int caret)
{
  if (len < 0 || static_cast<size_t>(len) > g_text_buf.size()) len = 0;
  g_queue.push_back({ type, frame, 0, 0, static_cast<double>(caret), 0, 0, 0,
                      std::string(), std::string(g_text_buf.data(), static_cast<size_t>(len)) });
}

// The clipboard copy, refilled from a paste event or an async read.
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_clip_begin()
{
  g_clip = neui_detail::DataItem();
}
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_clip_put(int kind, int len)
{
  if (len < 0 || static_cast<size_t>(len) > g_text_buf.size()) return;
  g_clip.set_format(clip_mime(kind), g_text_buf.data(), static_cast<uint32_t>(len));
}

// The page side ends the app (neui-web.js app.unmount()). Handled by the
// running loop, or by the service loop when none runs (main returned).
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_unmount()
{
  g_queue.push_back({ EV_UNMOUNT, 0, 0, 0, 0, 0, 0, 0, std::string(), std::string() });
  if (g_loop_depth == 0) web_schedule_service();
}

// A drag record; its formats (one per line, len bytes) are in the text buffer.
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_push_dnd(int type, int frame, double x, double y,
                                                       int mods, int allowed, int ev_id, int len)
{
  if (len < 0 || static_cast<size_t>(len) > g_text_buf.size()) len = 0;
  g_queue.push_back({ type, frame, x, y, static_cast<double>(mods), static_cast<double>(allowed),
                      static_cast<double>(ev_id), 0, std::string(),
                      std::string(g_text_buf.data(), static_cast<size_t>(len)) });
}

// A drop being assembled: begin clears the last one (and its files), put
// adds a string (MIME then data in the text buffer), file adds a file
// (name, type, then its bytes).
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_drop_begin()
{
  g_drop = neui_detail::DataItem();
  g_drop_uris.clear();
  for (auto& p : g_drop_paths) std::remove(p.c_str());
  g_drop_paths.clear();
  if (!g_drop_dir.empty()) rmdir(g_drop_dir.c_str());
  mkdir("/tmp", 0777);
  mkdir("/tmp/neui-drop", 0777);
  g_drop_dir = "/tmp/neui-drop/" + std::to_string(++g_drop_serial);
  mkdir(g_drop_dir.c_str(), 0777);
}
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_drop_put(int mime_len, int len)
{
  if (mime_len <= 0 || len < 0 ||
      static_cast<size_t>(mime_len) + static_cast<size_t>(len) > g_text_buf.size()) return;
  const char* p = g_text_buf.data();
  g_drop.set_format(std::string(p, static_cast<size_t>(mime_len)), p + mime_len,
                    static_cast<uint32_t>(len));
}
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_drop_file(int name_len, int type_len, int len)
{
  if (name_len < 0 || type_len < 0 || len < 0 ||
      static_cast<size_t>(name_len) + static_cast<size_t>(type_len) + static_cast<size_t>(len) >
          g_text_buf.size()) return;
  const char* p = g_text_buf.data();
  drop_file(std::string(p, static_cast<size_t>(name_len)),
            std::string(p + name_len, static_cast<size_t>(type_len)),
            reinterpret_cast<const uint8_t*>(p + name_len + type_len), static_cast<size_t>(len));
}

// ===========================================================================
// Platform seam implementation.

  void platform_init()
  {
    static bool inited = false;
    if (inited) return;
    inited = true;
    web_setup();
    g_apple = web_is_apple() != 0;
    // Debug switches (NEUI_DEBUG_FULL_REPAINT, NEUI_DEBUG_DAMAGE, ...) from the
    // page URL, before the host first reads them.
    static char query[2048];
    web_neui_query(query, static_cast<int>(sizeof query));
    for (char* line = query; *line;) {
      char* end = std::strchr(line, '\n');
      if (end) *end = '\0';
      if (char* eq = std::strchr(line, '=')) { *eq = '\0'; setenv(line, eq + 1, 1); }
      if (!end) break;
      line = end + 1;
    }
    // Before any Session exists, so the first frame already matches the page.
    neui_detail::theme_apply_web_scheme(web_scheme());
    neui_detail::metrics_add_measure_seam(&metrics_measure_web);
    g_page_state = web_page_hidden() ? NEUI_WEB_PAGE_HIDDEN : NEUI_WEB_PAGE_VISIBLE;
  }

  neui_render_backend_t* platform_get_backend()
  {
    return neui_canvas2d_backend::get_backend();
  }

  void platform_create_appwindow(Session* session, uint32_t widget_index, WidgetData& wd)
  {
    create_frame(session, widget_index, wd, 0, /*is_appwindow*/true, nullptr);
  }

  void platform_create_plugwindow(Session* session, uint32_t widget_index, WidgetData& wd)
  {
    create_frame(session, widget_index, wd, 1, /*is_appwindow*/false, nullptr);
  }

  void platform_create_dialog(Session* session, uint32_t widget_index, WidgetData& wd,
                              void* owner_native)
  {
    create_frame(session, widget_index, wd, 2, /*is_appwindow*/false,
                 static_cast<WebFrame*>(owner_native));
  }

  void platform_destroy_window(WidgetData& wd)
  {
    if (!wd.native_handle) return;
    teardown_window(static_cast<WebFrame*>(wd.native_handle), &wd);
  }

  void platform_show_window(void* native_handle)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    web_frame_show(f->id, 1);
    f->needs_paint = true;
    // No loop yet: main is about to call run(), or never will (an embedded
    // PLUGWINDOW). The service loop starts in a fresh task if none runs then.
    if (g_loop_depth == 0) web_schedule_service();
    if (f->is_dialog) {   // a dialog comes up focused, as a desktop one does
      forget_modal(f->id);
      g_modal_order.push_back(f->id);
      web_frame_focus(f->id);
    }
  }

  void platform_hide_window(void* native_handle)
  {
    if (!native_handle) return;
    web_frame_show(static_cast<WebFrame*>(native_handle)->id, 0);
  }

  void platform_set_window_enabled(void* native_handle, bool enabled)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    f->input_disabled = !enabled;
    web_frame_block(f->id, enabled ? 0 : 1);
  }

  void platform_activate_window(void* native_handle)
  {
    if (!native_handle) return;
    web_frame_focus(static_cast<WebFrame*>(native_handle)->id);
  }

  void platform_set_window_title(void* native_handle, const char* text)
  {
    if (!native_handle) return;
    web_frame_title(static_cast<WebFrame*>(native_handle)->id, text ? text : "");
  }

  // Logical px are CSS px. The canvas' CSS size changes; the ResizeObserver
  // then reports the new size (and RESIZE fires) like any other resize. A
  // frame that fills its page container keeps the container's size.
  void platform_set_window_pos(void* native_handle, int x, int y, int w, int h, uint32_t /*dpi*/)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    web_frame_geometry(f->id, x, y, w > 0 ? w : 1, h > 0 ? h : 1, f->is_dialog ? 1 : 0);
  }

  void platform_post_close(void* native_handle)
  {
    if (!native_handle) return;
    handle_close(static_cast<WebFrame*>(native_handle));   // synchronous, as on Linux
  }

  float platform_get_scale_factor(void* native_handle)
  {
    if (!native_handle) return 1.0f;
    const float s = static_cast<WebFrame*>(native_handle)->dpr;
    return s > 0.0f ? s : 1.0f;
  }

  void platform_invalidate(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<WebFrame*>(native_handle)->needs_paint = true;
  }

  void platform_invalidate_rect(void* native_handle, int x, int y, int w, int h)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    if (f->needs_paint) return;   // a full repaint is already pending
    f->damage.add({ x, y, w, h });
  }

  // One turn of the loop: drain, then wait - for the next animation frame
  // when a paint is pending (painting there, in step with the display), else
  // for a record or the heartbeat. cap_ms bounds the wait (-1 = none).
  static void loop_turn(int cap_ms)
  {
    drain_queue();
    if (g_quit) return;   // the last window closed / unmount: don't wait again
    service_timers();
    sync_text_proxy();
    if (any_paint_pending()) {
      if (web_wait(wait_timeout_ms(cap_ms), 1) == 1) {
        drain_queue();
        flush_pending_paints();
        sync_text_proxy();   // the paint walk updated the widgets' positions
      }
    } else {
      web_wait(wait_timeout_ms(cap_ms), 0);
    }
  }

  // After the outermost loop: tell the page side the app is done; after an
  // unmount, everything the app put on the page goes too.
  static void loop_finished()
  {
    if (g_loop_depth > 0 || g_ended) return;
    if (g_unmounting) {
      g_ended = true;
      g_mounts.clear();
      neui_canvas2d_backend::shutdown();
    }
    web_ended(g_unmounting ? 1 : 0);
  }

  bool platform_run()
  {
    platform_init();
    if (g_unmounting) return true;
    g_quit = false;
    ++g_loop_depth;
    while (!g_quit && g_appwindow_count > 0) loop_turn(-1);
    --g_loop_depth;
    loop_finished();
    return true;
  }

  // Non-blocking: drain and paint now. Never suspends, so it is safe from a
  // client's own requestAnimationFrame callback.
  bool platform_pump_once()
  {
    drain_queue();
    service_timers();
    flush_pending_paints();
    sync_text_proxy();
    return !g_quit;
  }

  bool platform_run_modal_until(bool* keep_running)
  {
    if (g_in_paint) {   // plans rule 3: a paint never suspends
      std::fprintf(stderr, "neui: blocking call during paint refused\n");
      return !g_quit;
    }
    ++g_loop_depth;
    while (keep_running && *keep_running && !g_quit) loop_turn(16);
    --g_loop_depth;
    return !g_quit;
  }

  // The service loop (plans rule 7): runs the frames of a client that never
  // calls run() - an embedded PLUGWINDOW (the embed contract forbids run()),
  // or an app whose main returned - and an unmount that comes after run()
  // returned. Started by web_schedule_service; returns at once while another
  // loop runs. A promising export under JSPI (JSPI_EXPORTS).
  extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_service()
  {
    if (g_loop_depth > 0 || g_ended) return;
    ++g_loop_depth;
    drain_queue();
    while (!g_quit && !g_frames.empty()) loop_turn(-1);
    --g_loop_depth;
    loop_finished();
  }

  // Embedding: the "native parent" of a PLUGWINDOW is a CSS selector (a
  // const char*), copied like NEUI_API_WEB set_mount. There is no DAW pump:
  // the service loop runs the frame, so event_fd is -1 and pump_and_tick a
  // no-op.
  void platform_set_embed_parent(Session* session, uint32_t widget_index, void* native_parent)
  {
    if (!session) return;
    auto* wd = session->get_widget(widget_index);
    if (!wd) return;
    const char* sel = static_cast<const char*>(native_parent);
    set_mount(session, widget_index, sel);
    wd->embed_parent = (sel && *sel) ? 1u : 0u;   // a flag here, not a pointer
  }
  int  platform_embed_event_fd(void* /*native_handle*/) { return -1; }
  void platform_embed_pump_and_tick(void* /*native_handle*/) {}

  // ---- Menu bar ---------------------------------------------------------------
  // The browser has no menu bar, so the host draws it inside the frame (the
  // Linux in-frame band: Session::paint_menubar + the handle_menubar_* input
  // path). As on Linux, platform_menubar_create must return a non-null
  // sentinel (widgets.cpp::t_add bails on null); everything else is a no-op.
  bool  platform_menubar_in_frame()                                                     { return true; }
  int   platform_frame_extra_top_inset(void* /*nh*/, bool /*has_menubar*/)              { return 0; }
  void* platform_menubar_create(uint32_t /*widget_id*/)                                 { return reinterpret_cast<void*>(0x1); }
  void  platform_menubar_destroy(void* /*hmenu*/)                                       {}
  void  platform_menubar_attach(void* /*frame*/, void* /*hmenu*/)                       {}
  void  platform_menubar_refresh(void* /*frame*/)                                       {}
  void* platform_menubar_add_popup(void* /*hmenu*/, const char* /*text*/)               { return reinterpret_cast<void*>(0x1); }
  void  platform_menubar_add_item(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_add_separator(void* /*hmenu*/, uint32_t /*cmd*/)               {}
  void  platform_menubar_remove_popup(void* /*hmenu*/, void* /*sub*/)                   {}
  void  platform_menubar_remove_item(void* /*hmenu*/, uint32_t /*cmd*/)                 {}
  void  platform_menubar_enable_item(void* /*hmenu*/, uint32_t /*cmd*/, bool /*en*/)    {}
  void  platform_menubar_enable_popup(void* /*hmenu*/, void* /*sub*/, bool /*en*/)      {}
  void  platform_menubar_check_item(void* /*hmenu*/, uint32_t /*cmd*/, bool /*chk*/)    {}
  void  platform_menubar_set_item_text(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_set_item_shortcut(void* /*hmenu*/, uint32_t /*cmd*/,
                                            uint32_t /*mods*/, uint32_t /*key*/)        {}

  // The favicon is the page's business.
  void platform_set_window_icon(WidgetData& /*wd*/, const char* /*path*/) {}

  void platform_apply_size_constraints(void* native_handle, int min_w, int min_h,
                                        int max_w, int max_h)
  {
    if (!native_handle) return;
    web_frame_constraints(static_cast<WebFrame*>(native_handle)->id, min_w, min_h, max_w, max_h);
  }

  // ---- Images: vendored stb_image over the in-memory file system -------------
  uint8_t* platform_load_image(const char* path, uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_stb(path, width_out, height_out);
  }
  uint8_t* platform_load_image_bytes(const uint8_t* data, size_t len,
                                      uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_stb_memory(data, len, width_out, height_out);
  }
  void platform_free_image(uint8_t* pixels)
  {
    neui_detail::free_image_bgra8_stb(pixels);
  }

  // ---- Clipboard ---------------------------------------------------------------

  // Bring the clipboard copy up to date before a read: nothing to do when a
  // paste event just filled it; otherwise ask the browser (suspends; never
  // during a paint). A refusal leaves the copy as it was. Either way it
  // counts as current for the rest of this record, so the size query and
  // the read that follows ask only once.
  static void clip_refresh(bool all)
  {
    if (g_clip_fresh || g_in_paint) return;
    web_clip_read(all ? 1 : 0, 5000);
    g_clip_fresh = true;
  }

  bool platform_clipboard_set_text(const char* utf8, uint32_t length)
  {
    if (!utf8) return false;
    g_clip = neui_detail::DataItem();
    g_clip.set_format(NEUI_MIME_TEXT, utf8, length);
    web_clip_write_text(utf8, static_cast<int>(length));
    return true;
  }

  int platform_clipboard_get_text(char* buf, int buflen)
  {
    clip_refresh(false);
    const int n = g_clip.get_format(NEUI_MIME_TEXT, nullptr, 0);
    if (n <= 0) return 0;
    if (buf && buflen > 0) {
      std::vector<char> tmp(static_cast<size_t>(n));
      g_clip.get_format(NEUI_MIME_TEXT, tmp.data(), n);
      const int copy = std::min(n, buflen - 1);
      std::memcpy(buf, tmp.data(), static_cast<size_t>(copy));
      buf[copy] = '\0';
    }
    return n + 1;
  }

  // From the copy only (a read could prompt; clients poll this).
  bool platform_clipboard_has_text()
  {
    return g_clip.get_format(NEUI_MIME_TEXT, nullptr, 0) > 0;
  }

  // No PRIMARY selection in a browser.
  void platform_clipboard_set_primary(const char* /*u*/, uint32_t /*l*/)      {}
  int  platform_clipboard_get_primary(char* /*b*/, int /*n*/)                 { return 0; }

  bool platform_clipboard_write_item(const neui_detail::DataItem& item)
  {
    neui_detail::DataItem copy;
    bool any = false;
    item.for_each_format([&](const std::string& mime, const std::vector<uint8_t>& bytes) {
      const int kind = clip_kind(mime);
      if (kind < 0) return;   // the browser clipboard takes these three only
      copy.set_format(clip_mime(kind), bytes.data(), static_cast<uint32_t>(bytes.size()));
      web_clip_item_add(kind, bytes.data(), static_cast<int>(bytes.size()));
      any = true;
    });
    if (!any) return false;
    g_clip = std::move(copy);
    web_clip_item_commit();
    return true;
  }

  bool platform_clipboard_read_item(neui_detail::DataItem& item)
  {
    clip_refresh(true);
    bool any = false;
    g_clip.for_each_format([&](const std::string& mime, const std::vector<uint8_t>& bytes) {
      item.set_format(mime, bytes.data(), static_cast<uint32_t>(bytes.size()));
      any = true;
    });
    return any;
  }

  // ---- Drag & drop --------------------------------------------------------------
  // Drop target: the frame's canvas takes DOM drag events (the frame's
  // WebFrame already knows its session and widget). Drag source: an in-page
  // drag between neui frames; see the file comment.

  bool platform_dnd_register_window(void* native_handle, void* /*session_ptr*/,
                                     uint32_t /*frame_widget_id*/)
  {
    if (!native_handle) return false;
    web_dnd_register(static_cast<WebFrame*>(native_handle)->id, 1);
    return true;
  }
  void platform_dnd_unregister_window(void* native_handle)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    if (g_dnd_over == f->id) g_dnd_over = 0;   // the frame is going: no LEAVE
    web_dnd_register(f->id, 0);
  }

  void* platform_make_drag_preview(const uint8_t* bgra_premul,
                                    uint32_t w_px, uint32_t h_px, float scale)
  {
    if (!bgra_premul || w_px == 0 || h_px == 0) return nullptr;
    auto* p = new DragPreview();
    p->bgra.assign(bgra_premul, bgra_premul + static_cast<size_t>(w_px) * h_px * 4);
    p->w_px = w_px; p->h_px = h_px; p->scale = scale > 0.0f ? scale : 1.0f;
    return p;   // consumed by platform_dnd_begin_drag
  }

  // Blocking in-page drag: a nested loop until release or Esc. Targets are
  // neui frames (drop targets of any session on the page); the item goes to
  // the drop as it is. Returns the negotiated action, 0 = cancelled.
  uint32_t platform_dnd_begin_drag(void* native_handle, neui_detail::DataItem* item,
                                   uint32_t allowed_actions, void* preview_native,
                                   int hot_x, int hot_y)
  {
    std::unique_ptr<DragPreview> preview(static_cast<DragPreview*>(preview_native));
    if (!native_handle || !item || g_drag || g_unmounting) return 0;
    if (g_in_paint) {   // plans rule 3: a paint never suspends
      std::fprintf(stderr, "neui: begin_drag during paint refused\n");
      return 0;
    }
    auto* src = static_cast<WebFrame*>(native_handle);
    const int src_id = src->id;
    WebDrag d;
    d.item    = item;
    d.allowed = allowed_actions ? allowed_actions : (NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE);
    item->for_each_mime([&](const std::string& mime) { d.mimes.push_back(mime); });
    for (auto& m : d.mimes) d.fmts.push_back(m.c_str());

    web_drag_begin(preview ? preview->bgra.data() : nullptr,
                   preview ? static_cast<int>(preview->w_px) : 0,
                   preview ? static_cast<int>(preview->h_px) : 0,
                   preview ? static_cast<double>(preview->scale) : 1.0, hot_x, hot_y);
    g_drag = &d;
    platform_run_modal_until(&d.running);
    g_drag = nullptr;
    web_drag_end();
    if (d.running) dnd_out();   // the loop quit under the drag

    // The release went to the drag, not to the widget it was pressed on.
    if (WebFrame* f = find_frame(src_id))
      if (f->session) f->session->set_pressed(0);
    return d.running ? 0 : d.result;
  }

  void platform_set_cursor(int kind)
  {
    static int current = NEUI_CURSOR_DEFAULT;
    if (kind == current) return;
    current = kind;
    web_set_cursor(kind == NEUI_CURSOR_EW_RESIZE ? "ew-resize" : "");
  }

  void platform_start_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<WebFrame*>(native_handle)->toast_anim = true;
    arm_heartbeat();
  }
  void platform_stop_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<WebFrame*>(native_handle)->toast_anim = false;
  }

  void platform_set_animation_timer(void* native_handle, uint32_t interval_ms)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    f->clock_anim = interval_ms != 0;
    if (f->clock_anim) arm_heartbeat();
  }

  bool platform_reduced_motion() { return web_reduced_motion() != 0; }

  uint64_t platform_now_ms()
  {
    return static_cast<uint64_t>(emscripten_get_now());
  }

  // Message box: neui-drawn (hosts/shared/message_box_painted.h, as on
  // Linux) on its own floating canvas, centred over the owner, which is
  // blocked meanwhile. The box runs on platform_run_modal_until, so timers,
  // toasts and other frames keep going. 0 = failure.
  int platform_message_box(void* native_handle, const char* text,
                           const char* caption, uint32_t flags)
  {
    if (!native_handle || g_unmounting) return 0;
    if (g_in_paint) {
      std::fprintf(stderr, "neui: message box during paint refused\n");
      return 0;
    }
    auto* backend = platform_get_backend();
    if (!backend) return 0;
    auto* owner = static_cast<WebFrame*>(native_handle);
    const int owner_id = owner->id;

    auto* b = new WebMsgBox();
    b->id = g_next_frame_id++;
    b->session = owner->session;
    {
      neui_detail::ScopedPaletteOverride scope(b->session ? b->session->effective_palette_ptr() : nullptr);
      if (!b->box.layout(backend, text, caption, flags)) { delete b; return 0; }
    }
    const int w = b->box.win_w, h = b->box.win_h;
    b->dpr = static_cast<float>(web_dpr());
    web_frame_create(b->id, 3, 0, 0, w, h, nullptr);
    web_frame_title(b->id, caption ? caption : "");
    web_frame_owner(b->id, owner_id, 0, 0, /*centred*/1);
    style_chrome(b->id, b->session);
    b->ctx = backend->create_context(reinterpret_cast<void*>(static_cast<uintptr_t>(b->id)),
                                     static_cast<uint32_t>(std::lround(static_cast<float>(w) * b->dpr)),
                                     static_cast<uint32_t>(std::lround(static_cast<float>(h) * b->dpr)));
    if (!b->ctx) { web_frame_destroy(b->id); delete b; return 0; }
    backend->update_dpi(b->ctx, dpi_for(b->dpr));
    neui_canvas2d_backend::set_scale(b->ctx, b->dpr);
    g_boxes.push_back(b);
    g_modal_order.push_back(b->id);

    // Block the owner (restored after: a box over an already-modal dialog
    // leaves that dialog's owner blocked).
    const bool prev_disabled = owner->input_disabled;
    platform_set_window_enabled(owner, false);
    web_frame_show(b->id, 1);
    web_frame_focus(b->id);

    platform_run_modal_until(&b->running);
    const int result = b->running ? 0 : b->result;   // the loop quit: no answer

    g_boxes.erase(std::remove(g_boxes.begin(), g_boxes.end(), b), g_boxes.end());
    forget_modal(b->id);
    backend->destroy_context(b->ctx);
    web_frame_destroy(b->id);
    delete b;
    // The owner may have closed while the box was up.
    if (WebFrame* o = find_frame(owner_id)) {
      platform_set_window_enabled(o, !prev_disabled);
      if (!prev_disabled) platform_activate_window(o);
    }
    return result;
  }

  // ---- NEUI_API_WEB (include/neui/d/web.h) ----------------------------------

  static Session* web_session(neui_session_t session)
  {
    return session_by_id(session.session & 0xffff);
  }

  static bool NEUI_ABI web_set_mount(neui_session_t session, neui_widget_t frame,
                                     const char* selector)
  {
    Session* s = web_session(session);
    if (!s || ((frame.id >> 16) & 0xffff) != (s->get_session_id() & 0xffff)) return false;
    const uint32_t idx = frame.id & 0xffff;
    auto* wd = s->get_widget(idx);
    if (!wd || wd->native_handle || !wd->type) return false;
    if (std::strcmp(wd->type, NEUI_W_APPWINDOW) != 0 && std::strcmp(wd->type, NEUI_W_PLUGWINDOW) != 0)
      return false;
    set_mount(s, idx, selector);
    return true;
  }

  static bool NEUI_ABI web_post_message(neui_session_t session, const char* data, uint32_t length)
  {
    if (!web_session(session) || (!data && length)) return false;
    return web_post_to_page(data ? data : "", static_cast<int>(length)) != 0;
  }

  static uint32_t NEUI_ABI web_page_state(neui_session_t /*session*/)
  {
    return g_page_state;
  }

  static uint32_t NEUI_ABI web_flavor(neui_session_t /*session*/)
  {
#if defined(NEUI_WEB_JSPI)
    return NEUI_WEB_FLAVOR_JSPI;
#else
    return NEUI_WEB_FLAVOR_ASYNCIFY;
#endif
  }

  static neui_web_api_t g_web_api = {
    NEUI_VERSION,
    web_set_mount,
    web_post_message,
    web_page_state,
    web_flavor,
  };

  void* platform_web_api() { return &g_web_api; }

} // namespace xpl_host

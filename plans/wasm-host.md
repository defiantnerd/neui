# WebAssembly Host Feasibility

## Context

neuilib ships three hosts today: the Win32 native-control host, the macOS
AppKit native-control host, and the crossplatform self-painted host (xpl) that
drives a pluggable render backend (Direct2D on Windows, CoreGraphics on macOS,
null elsewhere). The question is whether we can add a **WebAssembly target** so
the example app runs in a browser, and what it would cost.

This is an evaluation, written against the porting playbook in
`plans/how-to-port.md`. It documents what the work entails and ends with a
recommendation. There is no implementation here.

The confirmed goal is **full widget parity**: the entire example app (all 16
widget types, menus, clipboard, IME, modal + popup) running in a browser canvas.
The confirmed render path is **HTML5 Canvas 2D**.

## TL;DR

**Feasible, and a genuinely good architectural fit. Recommend proceeding via a
phased milestone path: a Canvas-2D backend plus a `platform_web.cpp` to reach a
core interactive demo first, then layer in the async-bound features (clipboard,
image load, IME) and a rebuilt menubar for full parity.**

Why this is a much better fit than the WinUI3 evaluation concluded for that
target:

1. **Every widget is free.** The web target uses the **xpl host**, which already
   paints all 16 widget types itself through the render backend. Unlike a native
   host (Win32 / macOS / the hypothetical WinUI3), there is **no per-widget
   mapping work**. Get the Canvas-2D backend and `platform_web.cpp` right and
   LABEL / BUTTON / INPUTBOX / MULTILINE / CHECKBOX / LISTBOX / COMBOBOX /
   TREEVIEW / SLIDER / KNOB / IMAGE / SECTION / CUSTOMDRAW all render and behave
   for free, from the same `hosts/crossplatform/host.cpp` code that runs on
   Windows and macOS.
2. **The cooperative-loop problem is already solved in the API.** The public
   `neui_api_t` already exposes a non-blocking `pump_once` (`api.h:69`),
   explicitly "intended for clients that drive their own outer event loop." The
   browser's `requestAnimationFrame` / `emscripten_set_main_loop` is exactly such
   an outer loop. `platform_run()` on web becomes "register the main-loop
   callback and return," driving `platform_pump_once()` per tick. This is the
   single biggest de-risking fact: the top-level run loop is **not** a
   re-architecture.
3. **Canvas 2D maps almost 1:1 to the render backend vtable.** fill/stroke rect,
   the path API (beginPath/moveTo/lineTo/arc/closePath/fill/stroke), the
   transform stack (save/restore/translate/rotate/scale), clip stack
   (save+clip/restore), `globalAlpha`, and `font` + `fillText` + `measureText`
   all have direct Canvas 2D equivalents. The per-frame "one live context" model
   mirrors the existing CoreGraphics backend's `set_current_frame` pattern.
4. **Single UI thread matches.** The framework's "single UI thread, non-atomic
   palette/listener tables" invariant is exactly the browser default. No
   SharedArrayBuffer / COOP-COEP threading needed for the UI.

The real costs are bounded and concentrated in three places: the **one** nested
blocking pump (popup menus), the **async** browser APIs (clipboard, image
decode) versus our synchronous seams, and the **menubar** (no OS menu strip on
the web - rebuild as DOM or in-canvas). None is a hard blocker; each is a
workaround-with-cost, detailed below.

## What gets built

### Two new build targets, one new platform source

| Target | Purpose |
|---|---|
| `neui-backend-canvas2d` | New render backend implementing `neui_render_backend_t` over an HTML5 Canvas 2D context. Per-frame live-context binding via a `set_current_frame` hook (mirrors `backends/cg`). |
| `platform_web.cpp` | New xpl platform layer (`hosts/crossplatform/platform_web.cpp`) implementing `xpl_host::platform_*` via Emscripten `html5.h` callbacks. Reuses the existing `neui-xplhost` target; no new host library. |

There is **no new host library**. The web target is "xpl host + Canvas-2D
backend + platform_web.cpp," exactly the shape the porting playbook calls the
common case (new backend + new platform layer, reuse xpl). It registers as the
existing `"neui.host.crossplatform"`.

### File layout

```
backends/canvas2d/
  CMakeLists.txt          # produces neui-backend-canvas2d (guarded if(EMSCRIPTEN))
  canvas2d_backend.h      # namespace neui_canvas2d_backend { get_backend(); set_current_frame(...); }
  canvas2d_backend.cpp    # 34 vtable slots over Canvas 2D + the per-frame ctx bind
hosts/crossplatform/
  platform_web.cpp        # xpl_host::platform_* via html5.h; the cooperative loop
examples/
  shell.html (optional)   # custom Emscripten shell hosting the <canvas>
```

## 1 - Toolchain (Emscripten + CMake)

Emscripten's CMake integration is first-class and stable: `emcmake cmake -B
out/web` injects `Emscripten.cmake` as the toolchain file and sets the
`EMSCRIPTEN` CMake variable; `cmake --build out/web` then drives `emcc`. Output
is `neui_example.{js,wasm}` plus an HTML shell. This is unlike the WinUI3 case,
where there was no first-class CMake story - here the build wiring is small.

### CMake changes (precise)

The repo's gating pattern (top-level `CMakeLists.txt`) is `if(WIN32) /
if(APPLE) / always(null + xpl)`. The web additions:

1. **Top-level `CMakeLists.txt`** - add a backend subdir gated on `EMSCRIPTEN`,
   alongside the existing per-platform blocks:
   ```cmake
   if(EMSCRIPTEN)
     add_subdirectory("backends/canvas2d")
   endif()
   ```
   No new `NEUI_HAS_*HOST` define is needed: `NEUI_HAS_XPLHOST` is already
   defined unconditionally, and the web target ships only the xpl host.

2. **Top-level `CMakeLists.txt`** - the example executable's per-platform branch
   (currently `if(MSVC)/elseif(WIN32)/elseif(APPLE)/else()`) gets an
   `elseif(EMSCRIPTEN)` arm: a plain `add_executable(neui_example
   examples/main.cpp)` plus Emscripten link options on the target, e.g.
   `-sUSE_WEBGL2=0`, `-sWASM=1`, `--shell-file`, the canvas size flags, and (see
   section 4) `-sASYNCIFY` for the modal-pump path. The example also lists its
   bitmap resources here via `--preload-file` (one per image; see section 5.1) -
   the web analog to the Win32 `.rc` embed and the macOS `.app` Resources copy.
   The example links the same `neui neui-xplhost` it links everywhere; the
   `if(WIN32)` / `if(APPLE)` native-host link blocks are simply not taken.

3. **`hosts/crossplatform/CMakeLists.txt`** - the platform-source selector is
   today `if(WIN32) / elseif(APPLE) / else()`. Insert an `elseif(EMSCRIPTEN)`
   arm that compiles `platform_web.cpp` and links `neui-backend-canvas2d`:
   ```cmake
   elseif(EMSCRIPTEN)
     target_sources(neui-xplhost PRIVATE platform_web.cpp)
     target_link_libraries(neui-xplhost PRIVATE neui-backend-canvas2d)
   ```
   (The current `else()` fall-through to `platform_null.cpp` +
   `neui-backend-null` stays as the catch-all for unrecognised platforms.)

4. **`backends/canvas2d/CMakeLists.txt`** - mirror `backends/null/CMakeLists.txt`:
   a static lib `neui-backend-canvas2d` with `target_include_directories(...
   PUBLIC ${CMAKE_SOURCE_DIR}/include)`. Guard the whole file body on
   `if(EMSCRIPTEN)` so a desktop build never compiles it.

Estimated build wiring: **1-2 days**, including a first `emcmake` configure +
link of an empty frame. Compare with WinUI3's 1-2 weeks of NuGet/MSBuild glue -
this is the easy end.

## 2 - The event-loop question (central architecture point)

This is the part every browser port lives or dies on, so it gets its own
section. There are two distinct loops, and they have very different costs.

### 2.1 The top-level run loop - already solved

Desktop `platform_run()` blocks (`GetMessageW` loop on Win32,
`[NSApp run]` on macOS) until the last window closes. The browser cannot block
the main thread.

We do **not** need to fight this, because the public API already ships a
cooperative drive path: `neui_api_t::pump_once` (`api.h:61-69`) -> the xpl
`platform_pump_once()` (`platform_win32.cpp` uses a non-blocking `PeekMessage`
drain; macOS uses `nextEventMatchingMask:untilDate:distantPast`). On web:

```
platform_run():       emscripten_set_main_loop(tick, 0 /*rAF*/, true);  // returns via unwind
tick():               platform_pump_once();   // drain queued JS events, repaint dirty windows
```

The example's `neui->run(sess)` at `main.cpp:877` therefore works unchanged: on
web, `run` installs the rAF callback. Clients that prefer to own the loop can
call `pump_once` directly, which the API already documents and supports. **No
re-architecture of the outer loop.**

### 2.2 The nested modal pump - the one real blocker

There is exactly **one** re-entrant blocking pump in the codebase:
`platform_run_modal_until(bool* keep_running)`, called from
`Session::open_popup_menu()` at `host.cpp:3046`. `popup_menu` is synchronous: it
opens an overlay, spins a nested OS message loop until the user picks or
dismisses, and returns the 1-based index to its caller. It backs the KNOB
right-click "Reset to default" menu and any client `widgets->popup_menu` call.

Important precision: **modal DIALOGs do not use this path.** Per the CLAUDE.md
"Deferred Issues" note ("Native blocking modal ... Current non-blocking modal
matches the event-loop shape"), `NEUI_W_DIALOG` modality is implemented by
disabling the owner window (`platform_set_window_enabled(owner, false)`), not by
a nested pump. So the modal-dialog case is **already** cooperative and ports for
free. Only `popup_menu` blocks.

Three ways to handle the popup nested pump on web:

- **(A) Asyncify (recommended).** Compile the web build with `-sASYNCIFY` and let
  `platform_run_modal_until` yield to the browser while watching
  `*keep_running`. Smallest code change; the nested pump's control flow stays
  intact. Cost: Asyncify adds roughly +50% wasm size and a baseline runtime
  overhead even when not suspending. We can scope the instrumentation with
  `ASYNCIFY_ONLY` to the modal pump + clipboard read paths to contain the cost.
- **(B) JSPI.** Newer VM-level stack switching with near-zero size/perf overhead,
  but as of this writing it leads in Chromium and trails in Firefox/Safari.
  Re-check browser support at implementation time; if broad, prefer it over (A).
- **(C) Refactor popup_menu to async on web.** Make the web popup return via a
  callback / deferred pick instead of a synchronous return. Cleanest at runtime,
  but it changes the `popup_menu` contract for one host and touches the KNOB
  reset-menu caller. More code, more risk than (A).

Recommended: **(A) Asyncify, scoped**, with (B) as a drop-in upgrade once JSPI
is broadly shipped. Budget for verifying the re-entrant pump behaves under
Asyncify (stack-unwinding reentrancy is the classic Asyncify footgun).

## 3 - Render integration (Canvas 2D backend)

The backend implements the full `neui_render_backend_t` (34 function pointers +
version; see `include/neui/d/renderer.h`). The per-frame model copies
`backends/cg/cg_backend.mm`: a `CanvasContextState` holds the live
`CanvasRenderingContext2D` JS handle, bound each frame by a public
`neui_canvas2d_backend::set_current_frame(ctx, js_context, w_logical,
h_logical)` that `platform_web.cpp` calls at the top of its paint callback
(exactly as `platform_macos.mm:122` calls the CG hook in `drawRect:`).

Vtable -> Canvas 2D mapping:

| Backend slot | Canvas 2D |
|---|---|
| `begin_frame(clear_argb)` | reset transform/clip/alpha; `clearRect` + `fillRect` whole canvas |
| `end_frame` | no-op (immediate); optional `flush` if double-buffered |
| `fill_rect` / `draw_rect` | `fillRect` / `strokeRect` (lineWidth = stroke) |
| `begin_path/move_to/line_to/arc/close_path/fill_path/stroke_path` | native Canvas Path verbs; `arc` Y-down clockwise matches |
| `push_transform/pop_transform` | `save()` / `restore()` (transform-only subset) |
| `translate/rotate/scale` | direct |
| `push_clip/pop_clip` | `save()` + `rect()` + `clip()` / `restore()` (nestable) |
| `push_alpha/pop_alpha` | multiply into `globalAlpha`; maintain a software stack like d2d/cg |
| `push_font/pop_font` | compose `ctx.font = "<weight> <size>px <family>"` (size stays per-call) |
| `draw_text` | `fillText`, manual clip to (x,y,w,h) box |
| `measure_text` | `ctx.measureText(s).width` |
| `create_bitmap/draw_bitmap/destroy_bitmap` | BGRA8-premult -> RGBA `ImageData`/`ImageBitmap`, cache per context, `drawImage` |
| `get_scale_factor` | `emscripten_get_device_pixel_ratio()` |
| `update_dpi` | recompute backing-store size = logical * dpr |
| `get_context_generation` | constant 0 (Canvas has no device-loss, like cg) |

Notes / gotchas:

- **Interop mechanism.** `EM_JS` / Embind to reach the Canvas 2D object. The
  hot-path draw calls (`fill_rect`, `draw_text`, clip/transform) cross the
  JS<->wasm boundary; batch where cheap, but Canvas 2D state is sticky so most
  ops are single calls. This is the same per-call cost shape as the other
  backends' brush lookups.
- **Bitmaps** need a BGRA->RGBA channel swap (Canvas wants RGBA) and the
  create-from-heap path should consume pixels immediately, matching the
  contract. Cache the uploaded `ImageBitmap` keyed by handle for the context's
  lifetime (mirrors d2d/cg per-ctx caches).
- **Fonts** must be loaded before first measure or `measureText` returns
  fallback metrics; gate the first paint on `document.fonts.ready` if custom
  `@font-face` families are used. System families work immediately.

Estimate to parity with a working desktop backend: **~1 to 1.5 weeks**
(primitives + path + transform/clip/alpha are quick; text box-clipping and the
bitmap upload/cache path are the time sinks).

## 4 - Per-widget effort

**Effectively zero incremental per-widget work.** This is the decisive
difference from the WinUI3 evaluation. The web target runs the xpl host, which
already implements every widget by painting through the backend in
`hosts/crossplatform/host.cpp` (LISTBOX/COMBOBOX/TREEVIEW/MULTILINE scrollbars,
KNOB drag, popup overlay, combo overlay, SECTION, CUSTOMDRAW + compound assets).
The same C++ that draws these on Windows/macOS draws them on web once the
backend exists. The CUSTOMDRAW painter API and compound-asset layer walk are
backend-agnostic and come along for free.

The only widget-shaped items needing thought are the ones whose *behaviour*
leans on an OS facility the browser lacks - all of which are platform-layer
work, not widget work:

| Widget / facility | Web treatment | Effort |
|---|---|---|
| `APPWINDOW` | the page's `<canvas>` (one per frame) | part of platform core |
| `PLUGWINDOW` | a `<canvas>` inside a host `<div>`; the DAW-embed semantic is moot on web but the widget still renders | S |
| `DIALOG` (+ modal) | second canvas/overlay layer; modal = pointer-events:none on the owner (already non-blocking - see 2.2) | S |
| `MENUBAR` | no OS menu strip; rebuild as a DOM strip or paint in-canvas; route `set_shortcut`/`set_menu_cmd` via keydown | **L (~1 week)** |
| `IMAGE` | needs async decode (see 5) | M |
| popup menu | the Asyncify path (see 2.2) | M |

Everything else (LABEL, BUTTON, INPUTBOX, MULTILINE, CHECKBOX/3, LISTBOX,
COMBOBOX, TREEVIEW, SLIDER, KNOB, SECTION, CUSTOMDRAW) is **S / free** because
it is already painted by the xpl host.

## 5 - Async + blocking mismatches (the real costs)

These are the items where a synchronous neui seam meets an asynchronous browser
API. None is a hard blocker; each costs either Asyncify instrumentation or a
JS-side cache.

- **Clipboard.** `platform_clipboard_get_text` is synchronous (returns bytes);
  the browser `navigator.clipboard.readText()` is a Promise gated behind a user
  gesture + permission. `set_text` via `writeText()` is easy inside a gesture.
  Read options: (a) Asyncify the get path, or (b) keep a JS-side text cache
  updated on `copy`/`cut`/`paste` + the `clipboardchange` event and answer
  `get_text`/`has_text` synchronously from it. The listener
  (`platform_register_clipboard_listener`) maps to those DOM events. Effort:
  **3-5 days**.
- **Image loading.** This is **not** an async mismatch for build-time-bundled
  resources - see section 5.1. It only becomes async for images fetched at
  runtime from a URL (not the example's case), which need an `emscripten_fetch`
  into MEMFS before the synchronous loader runs. Effort for bundled assets:
  **2-3 days** (mostly the in-wasm decoder + BGRA swizzle).
- **IME / composition.** Browser `compositionstart`/`update`/`end` on a hidden
  contenteditable/input proxy positioned at `WidgetData::caret_rect_local`,
  translated to the existing `WidgetData::on_composition(kind, utf8, len,
  caret_byte)` seam. The seam already exists and is platform-agnostic. Effort:
  **3-5 days** (kiosk builds can stub it; plain `KEYCHAR` typing still works).
- **No native menubar.** Covered above (L). The framework already routes
  `set_shortcut` + `set_menu_cmd` internally to command dispatch; the web layer
  supplies a keydown->command translation and renders the strip.

### 5.1 - Bitmap resources: how images are bundled and loaded

This is its own concern (how files *ship*), separate from the async question
above, and the project already solves it differently per platform:

- **Win32**: PNGs are embedded into the exe as user-defined `"PNG"` resources by
  `examples/neui_example.rc`; `platform_load_image` resolves them via
  `FindResourceW(name, L"PNG")` before falling back to disk (no runtime file
  dependency). See `CMakeLists.txt:107-110`.
- **macOS**: images are copied into the `.app` bundle's `Resources/`
  (`MACOSX_PACKAGE_LOCATION "Resources"`) and resolved by bundle path.

The web target needs the **third** strategy, and it is straightforward:

1. **Bundle at build time** with Emscripten `--preload-file
   <src>@<virtual-path>` (packages assets into a `.data` sidecar the runtime
   fetches automatically *before* `main()` runs) or `--embed-file` (bakes the
   bytes into the wasm/js, best for a few small files). The example's images
   (`myimage.png`, `myimage@2x.png`, `lemur@2x.jpg`, `lion@2x.jpg`,
   `panda@2x.jpg`) are listed in the example's `elseif(EMSCRIPTEN)` CMake arm -
   the direct analog to the `.rc` embed / `.app` Resources copy.
2. **Resolve + decode synchronously at runtime.** Once preloaded, the files live
   in Emscripten's in-memory FS (MEMFS) at their virtual paths.
   `platform_load_image("myimage.png", w, h)` keeps its **exact synchronous
   signature**: `fopen` the MEMFS path, read the bytes, decode them with a
   decoder compiled into the wasm module. `stb_image` is the natural fit
   (header-only, PNG + JPG, outputs RGBA8 which we swizzle to the BGRA8
   premultiplied the contract wants). `platform_free_image` frees the buffer.
3. **No Asyncify for bundled assets.** The only asynchronous step is the runtime's
   automatic `.data` fetch before `main()`, which Emscripten handles for you;
   nothing in the load path yields. The `@2x` / `@3x` resolution in the asset API
   (`create_from_file`) also works unchanged - it just probes MEMFS for
   `name@2x.png` via `fopen`, which succeeds when that variant was preloaded.

The IMAGE widget's path-based load (`set_text` = file path) and the
`NEUI_API_ASSETS` `create_from_file` path both flow through
`platform_load_image`, so both are covered by the in-wasm decoder with no
per-widget changes. Only images fetched from a remote URL *after* startup need
the async `emscripten_fetch` -> MEMFS -> sync-load detour.

## 6 - Input, DPI, focus, resize (platform core)

Straightforward `html5.h` wiring, mirroring the playbook's event contract:

- **Mouse**: `emscripten_set_{mousemove,mousedown,mouseup,dblclick,click,
  mouseenter,mouseleave,wheel}_callback`. Synthesize capture (drag-out) by
  tracking the pressed widget on the platform side (the playbook's capture
  model). Wheel delta normalised to signed "lines."
- **Keyboard**: `keydown`/`keyup` -> `KEYDOWN`/`KEYUP` (translate `code`/`key`
  to `NEUI_KEY_*`, which share Win32 VK values - a translation table is needed,
  modelled on `keys_macos.h`); text input -> `KEYCHAR` per codepoint. Tab /
  Shift-Tab consumed for `focus_next`, not dispatched as KEYDOWN.
- **DPI**: `emscripten_get_device_pixel_ratio()` -> `get_scale_factor`; backing
  store = logical * dpr; re-read on `resize`. Maps cleanly to the per-monitor
  model.
- **Focus**: canvas `focus`/`blur` -> `Session::_os_focused` + `WIDGET_FOCUS`.
- **Resize**: `ResizeObserver` / `emscripten_set_resize_callback` -> resize the
  render context (physical px) + `NEUI_EVENT_RESIZE` (logical px).
- **Paint trigger**: `platform_invalidate` -> request a rAF tick;
  the tick binds the live canvas context via `set_current_frame` then calls
  `Session::paint_frame`.

Estimate for platform core (window-as-canvas + the above + the cooperative
loop): **~1.5-2 weeks**.

## 7 - What we lose vs the desktop hosts

- **Native menubar + system accelerators.** Rebuilt in-app; functional but a
  parallel implementation to maintain.
- **Synchronous clipboard ergonomics.** Behind a JS cache or Asyncify; subject
  to browser permission/gesture rules.
- **The PLUGWINDOW DAW-embed use case.** No web analog; the widget renders but
  the audio-plugin-in-a-DAW scenario that motivates it does not exist on web.
- **True multi-top-level-window.** Each frame is a canvas on one page; multiple
  OS windows would mean `window.open` popups (fragile) or stacked canvases.
- **Pixel-identical text** vs desktop - Canvas 2D AA/hinting is browser-defined
  (acceptable for parity-of-behaviour, not parity-of-pixels).
- **Asyncify tax** on wasm size/startup if used for the modal pump + clipboard.

## 8 - What we gain

- **A real browser target with full widget parity** off the existing painted
  host - the multi-platform story extends to the web with no per-widget rewrite.
- **Zero-install distribution** - the example runs from a URL.
- **The single-threaded model matches the framework's invariants exactly** (no
  threading retrofit, unlike Qt-for-wasm's Asyncify-doesn't-scale pain).
- **Validation of the backend/platform seam** as genuinely portable - a third
  backend + fourth platform layer with no host changes is strong evidence the
  abstractions hold.

## 9 - Effort estimate

Notation matches `winui3-host.md`: S = ~1/2-1 day, M = 2-4 days, L = ~1 week.

| Workstream | Estimate |
|---|---|
| CMake wiring + first empty-frame `emcmake` build | 1-2 days |
| Canvas-2D backend to parity (primitives/path/transform/clip/alpha/font/text/bitmap) | 1-1.5 weeks |
| platform_web.cpp core (window-as-canvas, html5 input, cooperative loop, DPI, focus, resize, paint bind) | 1.5-2 weeks |
| Popup nested-pump via Asyncify (+ reentrancy verification) | 3-5 days |
| Clipboard (async, JS cache + listener) | 3-5 days |
| Image / asset async decode | 2-3 days |
| IME composition proxy | 3-5 days |
| Menubar rebuilt (DOM or in-canvas) + keydown accelerator routing | ~1 week |
| Testing, payload tuning, deployment (shell.html, headers), debugging | 1-2 weeks |

**Realistic total for full parity: ~6-9 weeks of focused work.**

Phased milestones (the recommended execution order):

- **Phase 1 - Static render proof (~1.5-2.5 weeks):** CMake + Canvas-2D backend +
  minimal `platform_web.cpp` that opens one canvas and paints a frame. Verifies
  the backend draws the example's static widgets.
- **Phase 2 - Core interactive demo (~+1.5-2 weeks):** mouse/keyboard/wheel/focus
  + the cooperative loop + resize/DPI + the Asyncify popup pump. The example app
  is usable (button, checkbox, slider, knob drag, input typing, custom-draw).
- **Phase 3 - Full parity (~+2.5-4 weeks):** clipboard, image/asset decode, IME,
  menubar, dialog overlays. Reaches the confirmed full-parity target.

This is materially less than the WinUI3 estimate (10-12 weeks) precisely because
there is no per-widget mapping: ~7 weeks of that estimate was widget work that
the xpl host already does for us.

## 10 - Recommendation

**Proceed, phased.** WASM is a good fit for neui's architecture: the painted xpl
host transfers wholesale, Canvas 2D maps cleanly to the backend vtable, the
single-threaded model matches, and the cooperative event loop the browser
demands is already a first-class API path (`pump_once`). The costs are real but
bounded: one Asyncify-scoped nested pump, two async-bound features behind a JS
cache or Asyncify, and a from-scratch menubar.

Start with **Phase 1 (static render proof)**. That milestone alone validates the
Canvas-2D backend and the `set_current_frame` per-frame binding, and surfaces
the JS<->wasm interop ergonomics before committing to the larger input + async
work. Decide whether to continue to Phases 2-3 after seeing the example paint in
a browser.

Re-check at implementation time: JSPI browser support (may replace Asyncify with
near-zero overhead), and whether any client genuinely needs PLUGWINDOW/multi-
window on web (if not, scope them as no-ops/single-canvas).

## 11 - Critical files to reference (for the work, if authorised)

- `include/neui/d/renderer.h` - the 34-slot `neui_render_backend_t` the new
  backend must fill.
- `backends/null/null_backend.{h,cpp}` - skeleton to copy for
  `neui-backend-canvas2d` (one static fn per slot + aggregate + `get_backend`).
- `backends/cg/cg_backend.{h,mm}` - the per-frame live-context model
  (`set_current_frame`, context valid only between bind and `end_frame`) to
  mirror for the Canvas context.
- `hosts/crossplatform/platform.h` - the full `xpl_host::platform_*` surface
  `platform_web.cpp` must implement.
- `hosts/crossplatform/platform_null.cpp` - minimal stub starting point.
- `hosts/crossplatform/platform_macos.mm` (`drawRect:` ~line 122; `platform_run`
  / `platform_pump_once` / `platform_run_modal_until` ~lines 1103-1133) - the
  closest reference (per-frame context bind; the run/pump/modal shapes).
- `hosts/crossplatform/host.cpp:3046` - the single nested-pump call site
  (`open_popup_menu` -> `platform_run_modal_until(&_popup_running)`).
- `include/neui/d/api.h:61-69` - the `pump_once` contract that makes the
  cooperative loop a non-event.
- `CMakeLists.txt` (root), `hosts/crossplatform/CMakeLists.txt`,
  `backends/null/CMakeLists.txt` - the gating + per-platform-source + backend-lib
  patterns to extend with `EMSCRIPTEN` arms.
- `hosts/shared/macos/keys_macos.h` - model for the browser `code`/`key` ->
  `NEUI_KEY_*` translation table.

## 12 - Verification

This evaluation produces a document. Its "verification" is the user reading it
and either accepting "proceed, phased" or redirecting. There is no code to run.

If implementation is authorised, the end-to-end verification per phase follows
the `how-to-port.md` test checklist, run in a browser:

- **Phase 1:** `emcmake cmake -B out/web && cmake --build out/web`; serve and open
  `neui_example.html`; confirm the frame clears to the expected colour and all
  static widgets (labels, button, checkbox, section, knob, slider) render.
- **Phase 2:** hover/click the button (onclick fires); drag the knob clockwise
  (value rises - verifies MOUSE_MOVE + synthesized capture); right-click the knob
  (popup appears, "Reset to default" dispatches through the Asyncify modal pump);
  Tab cycles focus + focus ring shows; type into INPUTBOX; resize the window
  (layout reflows, RESIZE fires); DPI scaling correct on a HiDPI display.
- **Phase 3:** Ctrl/Cmd-C/X/V in INPUTBOX round-trips via the clipboard cache;
  IMAGE widget shows an aspect-fitted picture; menubar picks dispatch and bound
  accelerators fire; modal DIALOG blocks the owner; IME composition (if wired)
  commits as one undo unit.

Run an open/close stress loop to confirm no Canvas context / ImageBitmap leaks
across window lifecycles.

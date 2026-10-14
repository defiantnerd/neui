<!-- neui reference: the web (WebAssembly) xpl host. Read before working on platform_web.cpp, backends/canvas2d or the embedding page side. -->

## Web host (Emscripten / WebAssembly)

**What it is**: the crossplatform (xpl) host built with Emscripten, paired with the Canvas 2D backend (`backends/canvas2d/`, `neui-backend-canvas2d`) and the browser platform layer `hosts/crossplatform/platform_web.cpp`. No new host: it registers as `"neui.host.crossplatform"`, client code is the desktop code. Every `<canvas>` is one neui frame; the app can fill a page or be mounted into an element of an existing page (the product goal). Design record, per-stage results and measurements: `plans/wasm-host.md`.

### Build

emsdk **4.0.23** (pinned; CI uses the same) and Ninja (Emscripten has no Makefiles generator on Windows):

```bash
emcmake cmake -B out/web-jspi     -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEUI_WEB_FLAVOR=jspi
emcmake cmake -B out/web-asyncify -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEUI_WEB_FLAVOR=asyncify
cmake --build out/web-jspi && ctest --test-dir out/web-jspi
```

Options (web only):

| Option | Default | |
|---|---|---|
| `NEUI_WEB_FLAVOR` | `jspi` | stack switching for blocking calls: `jspi` or `asyncify` (see Flavours). One flavour per build directory. |
| `NEUI_WEB_SIMD` | `ON` | the shader executor's 4-wide math as wasm SIMD128 (`-msimd128` on `neui-shader` only). `OFF` builds the portable path, for an engine without SIMD. |
| `NEUI_WEB_ASYNCIFY_STACK` | `262144` | Asyncify's unwind buffer (the 4 KB default is far too small for nested modals). |

Outputs: every example is a page (`name.html` + `.js` + `.wasm`, resources in `name.data` through `neui_link_example(... WEB_PRELOAD "src|dst" ...)`, which `--preload-file`s them into the in-memory file system under the names the code uses). `neui_link_example(<t> WEB_MODULE)` links an ES module instead (`.mjs`, no page) for embedding: `neui_web_embed_example` and `neui_example_module`. Serve the build directory over HTTP (wasm needs a real origin; no COOP / COEP headers: there are no threads), e.g. `python -m http.server -d out/web-jspi`, and open `neui_example.html` or `embed.html`.

Tests: `neui_tests` runs under node through ctest (both flavours; the shader goldens are bit-exact on wasm, with and without SIMD128). `neui_canvas2d_smoke` is a page: `node tests/web/run_page.mjs <build>/tests neui_canvas2d_smoke.html` runs it headless (Playwright; `NEUI_WEB_ENGINE=firefox|webkit`, `NEUI_WEB_BROWSER=msedge|chrome`). CI builds both flavours with `-DNEUI_WERROR=ON`, runs ctest and the smoke page in Chromium.

Gotchas: Emscripten sets `UNIX`, so a new `UNIX AND NOT APPLE` gate meant for X11 must add `AND NOT EMSCRIPTEN`. An `EM_JS` body is a C string literal: no regex backslashes and no `''` (use `""`). Never keep a `HEAPU8` view across a call that can allocate (memory growth replaces the buffer).

### Flavours

Desktop neui has blocking calls (`run`, a modal `show`, `popup_menu`, `message_box`, `begin_drag`); a browser never blocks. Both flavours keep the desktop semantics by suspending the wasm stack while the browser runs:

| | JSPI | Asyncify |
|---|---|---|
| Mechanism | JS promise integration (`WebAssembly.Suspending`) | Asyncify instrumentation (unwind / rewind) |
| Exceptions | native wasm exceptions (exnref) | JS exceptions |
| `neui_example` Release (`-O3`), wasm / gzip | 1.07 MB / 365 KB | 1.99 MB / 568 KB |
| Browsers | where `WebAssembly.Suspending` exists (checked: Edge 155, Firefox 157, Playwright WebKit 27.2) | any browser with wasm (and SIMD128, unless `NEUI_WEB_SIMD=OFF`) |

JSPI is the main target; Asyncify is the fallback and is tested less. `neui-web.js` picks per browser (`WebAssembly.Suspending` present → JSPI). Asyncify can suspend only one stack at a time, so neither flavour ever suspends two loops at once (the service loop below steps aside when `main` already runs a loop).

Size options, measured on `neui_example` (wasm / gzip -9; the shader library keeps `-O2` in every case):

| Link flags | JSPI | Asyncify |
|---|---|---|
| `-O3` (CMake Release default) | 1071 KB / 365 KB | 1991 KB / 568 KB |
| `-Os` | 879 KB / 322 KB | 1809 KB / 519 KB |
| `-Oz` | 754 KB / 289 KB | 1752 KB / 490 KB |
| `-O3 --closure=1` | wasm unchanged; JS 105 → 56 KB | wasm unchanged; JS 132 → 67 KB |

`-Oz` is the choice for size (-30 % wasm for JSPI, -12 % for Asyncify, whose instrumentation dominates) and costs little: steady-state paints measured the same within noise (shader example 8.2 → 8.5 ms, grid scrolling 1.7 → 1.8 ms), only the first frame is slower (17 → 31 ms). Closure only shrinks the JS glue. Set them with `CMAKE_CXX_FLAGS_RELEASE` / `CMAKE_EXE_LINKER_FLAGS_RELEASE`.

### The event loop

`main` runs on one suspendable stack and `run()` really blocks (code after it runs on unmount; stack-allocated client state stays valid). There is no `emscripten_set_main_loop`.

- DOM listeners (registered by `platform_web.cpp`'s JS half, `Module.neuiWeb`) only **queue** records keyed by frame id and wake the loop; they never call into neui. Record types are listed at the top of `platform_web.cpp`.
- `platform_run` = drain the queue (dispatch into the Session / client) → paint dirty frames on `requestAnimationFrame` → `web_wait(next deadline)`, an `EM_ASYNC_JS` import that suspends until a record arrives, a paint is due or the deadline (animation clock, toasts, scroll bounce, the 16 ms heartbeat) passes. Hidden tabs sleep (no rAF).
- Nested loops (modal `show`, `popup_menu`, `message_box`, in-page drag, async clipboard reads) are the same loop with another exit condition, on the same stack: `platform_run_modal_until`.
- **Paint never suspends**: while a frame paints, every blocking call logs and returns at once (0 / no-op).
- **Verdicts after dispatch** (`preventDefault` on keydown / wheel / contextmenu, `dropEffect` on dragover): the listener keeps the event; the stack resumes in the microtask checkpoint right after the listener, still inside event dispatch, and the drain applies the verdict. neui only cancels the browser's default where it consumed the event (page scrolling, browser shortcuts and the context menu stay with the page otherwise). Drag-over is the exception: over a registered canvas the default is always cancelled and refusal is `dropEffect = "none"`, so a refused file never opens in the tab.
- **Service loop**: a client that never calls `run()` (an embedded PLUGWINDOW whose `main` returns, as in a DAW, or an app driving `pump_once`) gets its frames run by `neui_web_service`, a second suspendable export started from a frame's first `show` when no loop runs. It returns at once if `main` has entered `run()` meanwhile.
- `pump_once` from a suspendable stack yields once. A client loop that sleeps must yield instead (`emscripten_sleep`), as `neui_partial_repaint_example` does: a `sleep_for` spins without handing control back.

### Frames, dialogs, popups

- **APPWINDOW / PLUGWINDOW**: a wrapper `<div>` + `<canvas>`. Mounted into the app's mount element (`NeuiApp.mount`), else `#neui-root`, else `<body>`; `NEUI_API_WEB::set_mount` / `NEUI_API_EMBED::set_parent` name another element by CSS selector. A container with a size of its own is filled and followed (ResizeObserver, `device-pixel-content-box`, so the backing store is exact at any zoom); otherwise the canvas takes the create() size. Size constraints map to CSS min / max. The title goes to `document.title` only when the app owns the page; icons are ignored.
- **DPR**: a `matchMedia('(resolution: Ndppx)')` watcher re-reports every frame; the backend gets the exact device pixel ratio (not an integer DPI) through a side channel.
- **DIALOG**: floats over the page (`position:absolute` in document coordinates) with a DOM title bar (title, close button through `handle_close` so `APP_QUIT` can veto, drag to move), coloured from the palette through CSS custom properties. x / y keep their offset from the owner's create() position and are clamped into view. A modal dialog's owner gets a backdrop that dims it and takes its pointer input.
- **message_box**: `hosts/shared/message_box_painted.h` (shared with Linux) on its own floating canvas, run on the modal loop, so timers, toasts and other frames keep going. Enter = default, Esc = cancel. Boxes can stack.
- **popup_menu**: painted in the frame it opened in (`Session::_popup_frame`), so it clips at that canvas's edge. Esc / Enter / Space / Up / Down drive it; a press in another frame dismisses it.
- **Menubar**: the in-frame band (`platform_menubar_in_frame()` = true, as on Linux) with `try_menubar_accel`. Browser-reserved shortcuts never reach the page: Ctrl/Cmd + N / T / W (and Shift variants), Ctrl+Tab, F11.
- Closing a frame destroys only its canvas; the widgets stay (the desktop contract).

### Input

- **Pointer**: Pointer Events with pointer capture (a drag keeps reporting outside the canvas and its release always arrives). Routing is copied from `platform_linux.cpp` ("mirrors" comments): overlay pre-checks popup → toast → combo → menubar, double-click synthesis (400 ms / 4 px), right button → RBUTTON. Pen and touch arrive as the left button.
- **Touch**: `touch-action:none` on the canvas (a drag is a neui drag, not a page pan), no callout or text selection. A **long press** (500 ms within 8 px, the primary touch only) is the right click: the left press ends (BUTTON_UP, no CLICK; a knob grab ends its gesture), then RBUTTON_DOWN / UP go to the widget under the finger, so a knob's "Reset to default" popup opens while the finger is down; the finger's release is swallowed.
- **Wheel**: `deltaMode` 0 (pixels) takes the smooth GRID / SECTION kinetics, lines / pages are notches; Chrome's ±100 px mouse notches are normalised. Page scrolling continues where neui has nothing to scroll.
- **Keys**: `hosts/shared/web/keys_web.h` (letters from `KeyboardEvent.key`, so Ctrl+Z follows the layout; digits and named keys from `code`; Cmd = `NEUI_KMOD_CTRL` on Apple; AltGr is not Ctrl+Alt). Order as on Linux: popup keys, Tab (focus traversal), open menubar, accelerators, KEYDOWN, KEYCHAR.
- **Focus**: canvas `focus` / `blur` → the frame's OS focus. Mounting never takes focus; a click or an explicit request does.
- **Cursor**: `canvas.style.cursor`.

### Text input, IME, clipboard

- A hidden `<textarea>` per frame holds DOM focus while an INPUTBOX / MULTILINE has neui focus, placed at the caret (`caret_rect_local`), so the IME candidate window and virtual keyboards appear there. Composition events → `on_composition` START / UPDATE / RESULT + END (plain underline: the browser gives no clause attributes); keyless input (virtual keyboards, emoji picker) → KEYCHAR. The GRID cell editor has no IME, as on every host.
- Clipboard: an in-process copy plus `navigator.clipboard`. Writes go to the system clipboard within the user activation (text/plain, text/html, image/png). Ctrl/Cmd+V waits for the paste event (no permission prompt); any other read (menu Paste, `NEUI_API_CLIPBOARD` from a click) suspends on `navigator.clipboard.read[Text]` (may prompt; falls back to the copy). `has_text` answers from the copy. No PRIMARY.

### Drag and drop

- **Drop target**: DOM drag events over a registered canvas → `dispatch_dnd_*`. Formats from `DataTransfer.types`; dropped files are written to the in-memory file system as `/tmp/neui-drop/<n>/<name>` and listed as `file://` URIs in `text/uri-list` (desktop code that opens dropped paths works unchanged); the first file of each type is also there as bytes under its MIME type. Files over 256 MB are left out.
- **Drag source**: `begin_drag` is an **in-page** drag between neui frames (any session on the page), run on the modal loop with window-level capture listeners; Ctrl / Shift pick copy / move / link, Esc cancels, the preview follows the pointer. Dragging out of the page is not supported: browsers start drags only from a native `dragstart`.

### Resources, fonts, images, debug

- Images through the stb loader (`hosts/shared/image_loader_stb.h`) from the in-memory file system; the `@2x` ladder works unchanged. There is no fetch of image URLs at runtime: bundle with `WEB_PRELOAD` or supply bytes through `NEUI_API_RESOURCE_CLIENT` (preferred for products).
- Client fonts: `FontFace` from the bytes, family and weight read from the font's own `name` / `OS/2` tables; usable for measuring at once (no relayout on load). Fonts are removed from `document.fonts` when the app unmounts.
- Theme: `prefers-color-scheme`, live (`hosts/shared/web/theme_provider_web.h`); `prefers-reduced-motion` feeds the animation clock. The page can override with `app.setTheme`.
- Debug switches: any `NEUI_*` URL query parameter becomes the environment variable before neui reads it (`?NEUI_DEBUG_PAINT_STATS=1`, `?NEUI_DEBUG_DAMAGE=1`). stderr goes to `console.warn` / `console.error`.

### Embedding into a page

`examples/web/neui-web.js` is the page side (copy it next to the modules):

```js
import { NeuiApp } from './neui-web.js';
const app = await NeuiApp.mount(document.getElementById('slot'), {
  module: { jspi: './jspi/my_app.mjs', asyncify: './asyncify/my_app.mjs' },  // or one URL
  flavor: 'auto',            // 'jspi' / 'asyncify' / 'auto'
  args:   ['--name=left'],   // argv[1..] of main
  theme:  'auto',            // 'dark' / 'light' / 'auto'
});
app.on('message', (text) => { /* NEUI_API_WEB post_message */ });
app.on('exit', () => { /* the app closed its last window */ });
app.postMessage({ hello: 1 });   // -> NEUI_EVENT_WEB_MESSAGE (non-strings as JSON)
app.setTheme('dark');            // frames with NEUI_ATTR_FOLLOW_SYSTEM_THEME follow it live
await app.unmount();
```

- Each mount is its own instance (own heap, own neui state); a module's wasm is compiled once and shared. Messages the app posts before the page listens are held for the first `message` listener.
- **`NEUI_API_WEB`** (`include/neui/d/web.h`, NULL on every other host): `set_mount(frame, selector)`, `post_message(data, length)`, `page_state()`, `flavor()`. Page → app arrives as events in category 0x000C: `NEUI_EVENT_WEB_MESSAGE` and `NEUI_EVENT_WEB_PAGE` (`visibilitychange` / `pagehide` → `NEUI_WEB_PAGE_VISIBLE / HIDDEN / LEAVING`), payload `neui_event_web_t`, delivered to every live session with `widget` = its oldest frame.
- **`NEUI_API_EMBED`**: `set_parent` takes a CSS selector (copied); `event_fd` is -1 and `pump_and_tick` a no-op, because the service loop drives the frame.
- **Unmount** closes every frame through the "gone" path (`APP_QUIT` is delivered but cannot keep a frame), ends open message boxes, drags and modal loops, lets `run()` return so the client's cleanup runs, then removes every canvas, page-level listener and font the app added. Verified flat over 100 mount / unmount cycles (DOM nodes, fonts, live wasm instances).
- **Page etiquette**: no `preventDefault` unless neui consumed the event, no global listeners beyond media queries, `visibilitychange` and `pagehide`, focus only on a click.
- **Deploy layout**: the `web` install component. Install a JSPI and an Asyncify build into one prefix:

  ```bash
  cmake --install out/web-jspi     --prefix dist --component web
  cmake --install out/web-asyncify --prefix dist --component web
  # dist/neui-web.js, dist/embed.html, dist/jspi/*.mjs|.wasm|.data, dist/asyncify/...
  ```

  `embed.html` (the demo: two apps side by side, a PLUGWINDOW in a resizable panel, the full `neui_example`, page ↔ app messages, mount / unmount, theme) loads from the flavour directories there and from its own directory when served from a build directory. Serve `.wasm` as `application/wasm` (streaming compile; other types fall back to a buffered compile).

### Rendering backend (Canvas 2D)

- One 2D context per canvas, drawn directly (`EM_JS` calls behind an internal `c2d_*` layer; the JS half is `Module.neuiC2D`). Measured in Stage 0 and again on real examples: the JS ↔ wasm crossings are a small part of a frame, so there is no command buffer.
- The context's own current path, not `Path2D`: points are mapped by the transform current when they are added (the Cairo / CG behaviour). Clips use a separate `Path2D` so the path survives.
- Text is laid out by the browser (shaping, fallback fonts); `draw_text` reproduces Cairo's box rules; measurements are cached per (font, string). Pixels differ from the desktop backends; behaviour does not.
- Gradients: linear and radial; REPEAT / MIRROR by stop expansion (the gradient is stretched over every tile the fill can reach, each tile gets a copy of the stops, reversed on odd tiles for MIRROR; past 256 tiles it clamps).
- Bitmaps: BGRA premultiplied → straight RGBA in a backing canvas (lossy at very low alpha, as every `putImageData`); tinted copies are cached. Off-screen contexts: `OffscreenCanvas` with `willReadFrequently`, or a hidden canvas where there is none (WebKit); `read_pixels_bgra` re-premultiplies.
- Partial repaint: the canvas keeps its pixels, so damage rects clip and clear only themselves; `set_frame_damage` returns false until one full frame exists at the current size.
- Shaders: no GPU program slots, so `.nsl` shaders take the CPU executor (bit-exact with the desktop) and are drawn as bitmaps, with SIMD128 by default.

### Performance (Edge, Release JSPI, headless)

| Workload | Per paint | |
|---|---|---|
| `neui_grid_example` smooth wheel scrolling | ~1.8 ms | 60 fps; CPU mostly `fillText` / clips in the browser |
| `neui_partial_repaint_example` (4 of 48 knobs per tick) | 0.3 ms (partial) | 55 ticks/s |
| `neui_shader_example` animated row | 8.2 ms (SIMD128) / 15 ms (portable) | CPU executor, partial frames |

Shader executor under node (`neui_shader_bench`, ns per pixel, portable → SIMD128): ring_glow 44.6 → 19.5, plasma 147 → 53, fbm5 563 → 201, metal_knob 210 → 79, tex_blur9 655 → 408.

### Not supported / limits

- Dragging out of the page; overlays (popups, combo lists) reaching beyond their canvas; image URLs fetched at runtime; multiple browser windows; threads.
- A GPU shader path (WebGL2) and accessibility (an ARIA mirror; canvas UIs are invisible to screen readers, as the desktop xpl host has no accessibility either) are deferred (`docs/deferred-issues.md`).
- Not verified here (no device): Safari on macOS / iPadOS, a real IME, Android / iOS virtual keyboards, real touch screens (the long press is checked with emulated touch), a real desktop drag from Explorer / Finder. Playwright's WebKit is not Safari.

# WebAssembly port of the xpl host (plan)

## Context

neui runs on Windows, macOS, Linux and iOS. The crossplatform ("xpl") host paints every widget itself through a render backend plus a thin platform layer, which makes it the natural vehicle for a browser target. An earlier feasibility note in this file (see git history) proposed this, but it predated most of today's code: it assumed 16 widget types and 34 backend slots and said modal dialogs don't block. Today the backend has 51 function slots, the xpl host has GRID / TABVIEW / shaders / partial repaint / animation clock / DnD / toasts, `widgets->show` on a modal DIALOG **blocks**, and Linux brought an in-frame menubar, a neui-painted message box and the stb image loader, which the web can reuse.

Goal (confirmed with the user): **an embeddable product UI**: real neui apps mounted into an existing web page, sized by CSS and talking to page JS. A demo page alone is not the target.

Confirmed decisions:
1. **Render backend: Canvas 2D** (new `neui-backend-canvas2d`). Browser text shaping and system fonts, a small wasm, and a retained canvas that makes partial repaint cheap.
2. **Blocking calls keep desktop semantics** through wasm stack switching: **JSPI** where the browser has it, an **Asyncify** build otherwise.
3. **One `<canvas>` per frame.** APPWINDOW fills its mount element; DIALOGs float above it.
4. **No new host.** The web target is xpl + Canvas2D backend + `platform_web.cpp`, registered as `"neui.host.crossplatform"`.

This plan replaces that feasibility note. Status: Stages 0-8 done (2026-10-14, results below each stage). The reader-facing reference is now `docs/host-web.md`. Safari on macOS and the iPad simulator were checked on a Mac afterwards (see "Mac check" under Stage 8). Open: a real CJK IME, a file drag from Finder / Explorer, real iPad and Android devices.

---

## Architecture

```
 page JS / DOM events (pointer, key, wheel, composition, paste, drag,
   │                   resize, DPR, theme, visibility, page messages)
   │        handlers ENQUEUE (by frame id) and resolve the pending wait
   ▼
 WebEventQueue (C++, platform_web.cpp)
   ▲ drained on main's stack
 main() → neui->run() → platform_run():
          loop { drain queue → dispatch into Session / client
                 paint dirty frames (Canvas2D backend; never suspends)
                 web_wait(next deadline)   ← suspends (JSPI / Asyncify)
               }
 nested loops (modal show, popup_menu, message_box, in-page drag)
   = the same loop with another exit condition, on the same stack
```

### Event-loop rules

1. **Everything dispatches on one suspendable stack.** DOM listeners (a custom `--js-library`, not `html5.h`, whose callbacks call into wasm synchronously and use page-wide selectors) only push records keyed by **frame id** (never a `WebFrame*`: teardown frees it) and resolve the pending wait. Session dispatch, client `onevent`, theme `onchange` (`host.cpp:273-278`), RESIZE, focus and page messages all run inside `platform_run` / `platform_run_modal_until`. A handler that calls `show(modal_dialog)` or `message_box` runs a nested loop on the same stack, which is the Linux `select()` model with `web_wait()` in place of `select()`.
2. **The blocking set** is every caller of `platform_run_modal_until` plus the calls that run their own loop:
   - modal `w_show` (`widgets.cpp:451`)
   - `open_popup_menu` (`host.cpp:4439`), reached from public `popup_menu` (`widgets.cpp:789`), behavior `ctx.popup_menu` (`host.cpp:3368`) and KNOB reset (`host.cpp:3522`)
   - `message_box` (`widgets.cpp:2026`)
   - DnD `begin_drag` (`widgets.cpp:3869`, behavior DRAG `host.cpp:3372`)
   - async clipboard reads (Stage 5)
3. **Paint never suspends.** `paint_frame` calls the client (PREUPDATE / WIDGET_PAINT, `host.cpp:2385-2517`), and Canvas 2D draws straight into the visible canvas. A guard flag makes every blocking API return 0 and log while a paint is in progress.
4. **`web_wait(timeout)`** is an async import (`EM_ASYNC_JS` / `ASYNCIFY_IMPORTS`). It resolves on the first of these:
   - a queue push
   - a rAF tick when a paint is pending (rAF, not polling, so hidden tabs sleep)
   - the next deadline (animation clock, toast, scroll bounce, modal 16 ms tick)

   This mirrors `platform_linux.cpp` `platform_run` (:2166), including the rule "don't sleep while a paint is pending".
5. **Synchronous verdicts** (`preventDefault` on keydown / wheel / contextmenu, `dropEffect` on dragover) are decided only after dispatch. Primary design (proven in Chromium and WebKit in Stage 0; real Firefox still to check): the listener resolves the wait directly, the stack resumes in the microtask checkpoint right after the listener, which is still inside event dispatch, and the drain calls `preventDefault` on the event it kept. Fallback: small query-only exports that never dispatch (`would_consume_key/wheel`, a drop-accept query).
6. **`run()` really blocks.** Code after `run()` (cleanup in `examples/main.cpp:1356-1412`) runs on unmount, and stack-allocated client state (`AppState app`) stays valid. There is no `emscripten_set_main_loop`.
7. **Service loop for clients that never call `run()`.** Two cases need it:
   - an embedded PLUGWINDOW: the embed contract says "never call run()", and `platform_run` returns at once with no APPWINDOW (`:2172`)
   - an app that only uses `pump_once`

   On the first `show` without an active `run`, the platform starts its own promising loop export. `pump_once` from a suspendable stack yields once. `pump_once` from rAF (not suspendable) never blocks, and blocking calls there are refused like in paint.

   **At most one loop owns the suspendable stack** (Stage 0: Asyncify corrupts memory with two suspended stacks; JSPI copes, but one rule for both flavours keeps them identical). The service loop starts only when no `run()` is active, and `run()` called while the service loop is active joins it rather than starting a second loop.
8. **Two flavours from one source.** `-sJSPI` is preferred, `-sASYNCIFY` is the fallback, and a loader picks one by detecting `WebAssembly.Suspending`. Asyncify can't be scoped narrowly here, because client handlers reach blocking calls through function pointers. It instruments broadly. Exceptions differ per flavour: `-fwasm-exceptions` with JSPI, `-fexceptions` (JS) with Asyncify (Stage 0 item 5).

---

## Stages

Each stage ends green on all existing CI jobs plus the new web job. Rough sizes: S ≈ 1-2 days, M ≈ 3-5 days, L ≈ 1-2 weeks.

### Stage 0: spikes and toolchain decisions (M, throwaway code on a scratch branch)

Small standalone programs answer the questions the later stages depend on:
1. **Pin an emsdk version** (record it in the doc and in CI).
2. **JSPI loop:**
   - `main` suspends in a loop while DOM listeners re-enter wasm (queue push)
   - nested suspend from inside a function-pointer callback
   - a second promising export (the service loop)
   - shadow-stack integrity
3. **Microtask-resume verdict (rule 5):** does `preventDefault` from the resumed drain still take effect for keydown, wheel, contextmenu and dragover in Chrome, Firefox and Safari, under both JSPI and Asyncify?
4. **Asyncify:** the same tests; size and per-frame cost against the JSPI build; `ASYNCIFY_STACK_SIZE` (the 4 KB default is far too small for nested modals).
5. **Exceptions.** `catch` is needed in:
   - `src/shader/nsl_compile.cpp` (compile errors)
   - `hosts/shared/widget_paint_compound.h:338-367` (QR `data_too_long`, compiled into `host.cpp`)
   - the `bad_alloc` guards in `hosts/shared/shader_asset.h:179,523`

   Verify `-fwasm-exceptions` with JSPI and with Asyncify. If Asyncify plus wasm exceptions fails, choose between JS exceptions for that flavour and a QR capacity pre-check (then `bad_alloc` aborts).
6. **Canvas 2D call cost:** 10k `fillRect` and 2k `fillText` / `measureText` per frame through direct `EM_JS` calls against a command buffer decoded once per frame. All calls go through one internal `c2d_*` layer, so a later switch stays local.
7. **`FontFace` from an `ArrayBuffer`:** when can `measureText` use it?
8. **Browser matrix** for JSPI (Chrome / Edge, Firefox, Safari incl. iPadOS).

Exit: a "Stage 0 results" section in the in-repo plan with the decisions (exception mode, verdict mechanism, call model, flavour matrix).

#### Stage 0 results (2026-10-13)

Spikes ran on Windows with emsdk **4.0.23** against Edge 155 (the installed Chromium) and Playwright 1.64's Firefox 157 and WebKit 27.2. The spike code was throwaway: a loop with canaries, a Canvas 2D bench, a font timing page, and a Playwright driver.

| Question | Result | Decision |
|---|---|---|
| 1. emsdk pin | 4.0.23 installs and builds. Bootstrapping emsdk needs Python ≥ 3.10; emsdk then brings its own Python 3.13 + node 24. emcc warns that `-sJSPI` is "still experimental" (`-Wexperimental`). | Pin 4.0.23 in CI and the docs; pass `-Wno-experimental` on the JSPI link. |
| 2. JSPI loop | Passes in all three engines. 266 events were pushed from JS while `main` was suspended; each push used a 16 KB stack frame, and no canary broke. Modals nested 3 deep through a function-pointer handler. A **second promising export** (service loop) ran alongside the suspended `main`, and both canaries stayed intact. Exports that suspend must be listed in `-sJSPI_EXPORTS`; otherwise `SuspendError`. | Design as planned. List the service loop in `JSPI_EXPORTS`. |
| 3. Verdict after dispatch (rule 5) | **Works** in Edge and WebKit for keydown (text insertion prevented), wheel (no scroll), contextmenu and dragover (drop fires), under all three builds. The `queueMicrotask` control behaved the same and the `setTimeout` control failed, so the test separates the cases. Firefox: wheel and contextmenu worked; keydown and dragover did not. A plain-JS `queueMicrotask` control failed there too, while a synchronous `preventDefault` worked. That points to Playwright's Firefox input injection (script still on the stack when the event is dispatched), not to Firefox itself. | Microtask resume is the primary mechanism. **Verify by hand in a real Firefox** during the Stage 3 browser pass. Keep the query-export fallback designed but unbuilt until then. |
| 4. Asyncify | Loop, nesting, canaries and verdicts pass in all three engines **as long as only one stack is suspended**. With the service loop suspended next to `main`, Asyncify corrupts memory (`memory access out of bounds` in later calls). Spike size: JSPI 28 KB wasm against Asyncify 40 KB (+43 %, wasm EH) / 38 KB (JS EH), with `ASYNCIFY_STACK_SIZE=65536`. | **Rule: at most one suspended loop at a time, in both flavours** (see rule 7). Real-code size is measured in Stage 1. |
| 5. Exceptions | JSPI + `-fwasm-exceptions`: works, including a suspend inside a `try`. Asyncify + `-fwasm-exceptions`: emcc warns "not compatible … parts that mix will not compile"; the spike happened to run, but this is unsupported. Asyncify + JS exceptions (`-fexceptions`): works. | JSPI flavour: `-fwasm-exceptions`. Asyncify flavour: `-fexceptions`. No QR capacity pre-check needed. |
| 6. Canvas 2D call cost | Per frame of 10k `fillRect` + 2k clipped `fillText` + 2k `measureText` (headless, medians): Edge direct 17.4 ms against buffered 15.2 ms; Firefox 52 against 42; WebKit 17 against 15. 2k uncached `measureText`: 1–4 ms. The time is canvas work, not JS↔wasm crossings. | **Direct `EM_JS` calls** behind the `c2d_*` layer. The command buffer is dropped from the plan unless Stage 8 profiling says otherwise. Keep the measure cache. |
| 7. `FontFace` from `ArrayBuffer` | In all three engines `status` is `loaded` straight after construction; `measureText` uses the face right after `document.fonts.add`, and `load()` resolves in about 0 ms. | `register_font` is **synchronous**; no relayout on font load. `get_context_generation` is bumped only for context loss. |
| 8. Browser matrix | `WebAssembly.Suspending` exists, and JSPI runs, in Edge 155, Firefox 157 and Playwright WebKit 27.2. Playwright's WebKit is not Safari; it also lacks `OffscreenCanvas`. Real Safari / iPadOS were not tested (no Mac here). | JSPI is the main target; Asyncify stays as the fallback for older browsers and is tested less. Off-screen contexts **must** have the hidden-canvas fallback. Check Safari on a Mac in Stage 8. |

### Stage 1: build plumbing, null-on-web, tests under node (M)

- **CMake gates.** Emscripten's toolchain sets `UNIX`, so today every `UNIX AND NOT APPLE` gate fires and configure fails on Cairo / X11 / fontconfig marked REQUIRED. Add `EMSCRIPTEN` arms before them:
  - root `CMakeLists.txt:160-162`: skip cairo on EMSCRIPTEN.
  - `hosts/crossplatform/CMakeLists.txt:63`: an `elseif(EMSCRIPTEN)` arm before `elseif(UNIX)` with `platform_web.cpp`, the stb include and `NEUI_PLATFORM_WEB`. It links `neui-backend-null` until Stage 2 and does **not** copy the null arm's `NEUI_NO_SHADERS=1`.
  - `backends/cairo/CMakeLists.txt:5-7`: return early on EMSCRIPTEN.
  - `tests/CMakeLists.txt:135-209` (cairo smoke + X11 harnesses): add `AND NOT EMSCRIPTEN`.
  - Examples (`CMakeLists.txt:221+`, ~17 repeated if/elseif blocks): one helper `neui_add_example(name sources... RESOURCES ...)`. On web it sets the `.html` suffix (per target, never global, so `neui_tests` stays `.js` for node), the shell file, the flavour flags and `--preload-file`.
  - A `NEUI_WEB_FLAVOR=jspi|asyncify` cache option.
  - Link flags: `-sALLOW_MEMORY_GROWTH`, `-sSTACK_SIZE=1MB` or more (paint recursion, mujson depth 128, the recursive-descent `.nsl` compiler), `-sSTACK_OVERFLOW_CHECK=2` in Debug.
- **`platform_web.cpp`** starts as a copy of `platform_null.cpp`.
- **Shader lib:** keep `-O2 -ffp-contract=off`. Plain wasm32 takes the portable SIMD path (`nsl_math.h:296-336`), and `FpEnvGuard` is empty because wasm has no FTZ, so this is correct by construction. Do not pass `-msse2`.
- **Warnings:** emsdk's clang is newer than any CI compiler; budget a fix-up pass so `-DNEUI_WERROR=ON` holds. Fix the warnings, don't suppress them.
- **Tests under node.** `neui_tests` uses no files, threads or env, and runs under node through `CMAKE_CROSSCOMPILING_EMULATOR`. Shader goldens must stay bit-exact. Watch `test_shader_exec.cpp:372`: it CRCs raw floats that may be NaN, and wasm doesn't pin NaN payloads. If it fails, canonicalise in the test, never in the executor.
- **CI:** a `web` job on `ubuntu-latest` with the pinned emsdk runs `emcmake cmake -DNEUI_WERROR=ON`, builds both flavours and runs `ctest`.
- `readme_example` links and loads (blank page).

Exit: the web CI job is green, `neui_tests` passes under node, desktop jobs are unchanged.

#### Stage 1 results (2026-10-13)

- **Configure + build:** `emcmake cmake -G Ninja` (Ninja on Windows; emsdk ships none) builds all 90 targets, including all 17 examples, warning-free with `-DNEUI_WERROR=ON`, in both flavours, Debug and Release.
- **Warnings emsdk's clang found:** `FpEnvGuard` (`nsl_math.h`) and the test's `HostFlushToZero` have no members on the portable path, so their locals were "unused". Both now have an explicit `#else` arm with user-provided constructor and destructor. `readme_example.c` compiled as C++ needs `-xc++` on em++. Nothing was suppressed.
- **Tests:** `neui_tests` passes under node 24 in both flavours: 469 cases, 6885 checks. The shader goldens are bit-exact on wasm; the NaN-payload concern did not materialise.
- **Exceptions:** JSPI uses `-fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0` (the current exnref encoding; the legacy `try` encoding makes Firefox warn). Asyncify uses `-fexceptions`.
- **Release sizes**, before any size work (`neui_example`): JSPI 854 KB wasm + 79 KB JS; Asyncify 1.67 MB wasm + 106 KB JS (×2.0; JS exceptions + broad instrumentation). `readme_example` is 840 KB / 1.64 MB, so most of that is the host + shader library, not the example. Stage 8 does the size pass.
- **Browser smoke:** `readme_example` and `neui_example` load and run `main` to completion in Edge 155, Firefox 157 and WebKit 27.2, in both flavours. On the null layer `run()` returns at once, and the example's own cleanup after `run()` runs without errors.
- **CMake:** 15 of the 17 per-example if/elseif blocks collapsed onto `neui_add_example` (byte-identical Windows projects: same `.rc`, same `/MANIFEST:NO`). `neui_example` and `readme_example` keep their custom creation and use `neui_link_example`. Web settings live in one `if(EMSCRIPTEN)` block in the root `CMakeLists.txt`. The flavour's link flags sit on `neui-xplhost` as INTERFACE options, so `neui_tests` never needs stack switching.
- **Not yet verified:** the new `web` CI job (it runs on the next push), and the Linux / macOS / iOS jobs after the CMake refactor (Windows was rebuilt and tested locally).

### Stage 2: Canvas 2D backend + minimal window (L)

**2a: minimal `platform_web` window (no input).** Frame wrapper `<div>` + `<canvas>`, the loop (`run` / `web_wait` / paint), invalidate + damage, ResizeObserver, DPR. This is just enough to see paint; it is Stage 3's core, done early on purpose.

**2b: backend.** New `backends/canvas2d/`:
- `CMakeLists.txt`: an EMSCRIPTEN-only static lib
- `canvas2d_backend.{h,cpp}`
- `canvas2d.js`: a js-library linked through `target_link_options(INTERFACE --js-library ...)`. It holds handle tables (ctx → canvas + context + stacks, bitmap → canvas, fonts) and never keeps a `HEAPU8` view across calls that can allocate (memory growth).

Slot mapping (`renderer.h:34-418`):

| Slots | Canvas 2D |
|---|---|
| create/destroy_context, resize | `native_handle` = frame id. Context `alpha:true`. Backing store from ResizeObserver `devicePixelContentBoxSize` (rounded on Safari). Resize clears the canvas, so the backend marks "no complete frame". |
| get_scale_factor / update_dpi | `update_dpi` takes an integer DPI (`renderer.h:73`); 96×DPR is not an integer at 90 % / 110 % zoom. The exact DPR comes from `platform_web` through a backend side channel. |
| begin/end_frame | Reset transform to `scale(dpr)` and the clip / alpha / font stacks; `clearRect`, then fill with the clear colour (alpha < 255 stays transparent, as in the overlay example). end_frame is a no-op: the canvas presents at the end of the task. |
| fill/draw_rect | `fillRect` / `strokeRect`; colour strings cached per ARGB |
| draw_text | Same semantics as Cairo (`cairo_backend.cpp:653-686`): split on `\n`, centre the line block using font ascent + descent (`fontBoundingBoxAscent/Descent`, cached per font, `textBaseline='alphabetic'`), clamp the top to y, clip to the box. Skip save/clip/restore when the measured text fits. |
| measure_text | `text_len` counts bytes, so slice in C++ before `UTF8ToString`. A dedicated measuring context with an LRU cache keyed (family, weight, size, string), because MULTILINE wrap measures heavily. |
| push/pop_font | Compose `ctx.font` with quoted, escaped family names (an invalid string is silently ignored). An empty family maps to `system-ui, sans-serif` (`widget_font.h:57`). |
| paths: begin / move / line / arc / close / cubic_to / quad_to, fill / stroke_path, set_fill_rule, stroke_path_styled | Use the context's **own current path**, not `Path2D`. Points are recorded under the transform current when they are added, which is the Cairo / CG behaviour; a `Path2D` would be transformed at fill time. The current path survives `fillRect` / `strokeRect` / `fillText`. `arc` passes `anticlockwise = end < start` (`renderer.h:160`, Cairo `:861-864`). `fill('evenodd' or 'nonzero')`; `lineCap` / `lineJoin` / `miterLimit` / `setLineDash` / `lineDashOffset`. |
| push/pop_clip | `save()` + `clip(new Path2D(rect))` (keeps the current path) / `restore()`. A C++ transform mirror serves `get_device_transform`. |
| push/pop_transform, translate / rotate / scale | `save` / `restore` subset; the C++ mirror is updated alongside |
| push/pop_alpha | `globalAlpha` through the `backend_util.h` alpha stack |
| gradients (rect / path / stroke) | `createLinearGradient` / `createRadialGradient` (start = centre, end = focal, `gradient.h:44-58`). REPEAT / MIRROR degrade to CLAMP like CG in v1; stop expansion comes later. |
| bitmaps create / destroy / draw (+ tint) | BGRA premultiplied → straight RGBA → `putImageData` into a backing **canvas** (`createImageBitmap` is async). The source rect is in the bitmap's logical units, so multiply by `scale` (`:792-795`). Tint ≠ 0xFFFFFFFF: a cached tinted copy keyed (bitmap, tint). Pool backing canvases by size for per-frame CPU-shader uploads. |
| create_offscreen_context / read_pixels_bgra | `OffscreenCanvas`, with a hidden `<canvas>` fallback that is required (Playwright's WebKit has no `OffscreenCanvas`), with `willReadFrequently:true`; `getImageData`, premultiply, BGRA. Lossy at low alpha (unpremultiply round trip); documented, with a tolerance in tests. |
| register_font / register_font_file / unregister_font | `new FontFace(family, bytes)` + `document.fonts.add`; the family comes from the font's own `name` table (small sfnt parser in C++). An `ArrayBuffer` face is usable as soon as it is constructed in every engine tested (Stage 0), so registration is synchronous; drop the measure cache entries for that family on register / unregister. |
| set_frame_damage | The canvas keeps its pixels: clip to the damage rects and clear only those. Returns false until one full frame has been drawn at the current size / DPR (Cairo rule, `:550`). |
| get_context_generation | Bumped on 2D `contextlost` / `contextrestored` |
| shader program slots | NULL, so shaders take the CPU executor path, drawn as bitmaps |

Reuse `backends/shared/backend_util.h` (`argb_unpack`, `quad_to_cubic`, the alpha stack, `font_size_q10`, `damage_rect_to_device`).

**Test:** `tests/canvas2d_smoke`, the web twin of `tests/cairo_backend_smoke.cpp`: an off-screen context, rect / arc / bitmap / text, `read_pixels_bgra` assertions with alpha tolerance, damage. Built as `.html`, run in headless Chromium by a small Playwright script in the web CI job (a CI-only dependency) that waits for a `PASS` / `FAIL` console line.

Exit: smoke test green in CI; `path_example`, `arc_example` and `neui_example` paint statically and match desktop screenshots side by side.

#### Stage 2 results (2026-10-13)

- **Backend** (`backends/canvas2d/`, ~900 lines): every slot except the GPU shader programs, through `EM_JS`. The JS half lives on `Module.neuiC2D`. One gotcha: an `EM_JS` body is a C string literal, so it cannot contain regex backslashes. Text, paths, clips, transforms, alpha, fonts (CSS strings, plus a measure cache in C++), gradients (CLAMP only), bitmaps with tint, off-screen contexts with readback, partial damage and context generation all work. Client fonts read their family + weight from the font's own `name` / `OS/2` tables.
- **Platform 2a** (`platform_web.cpp`): frames as `<canvas>` in a wrapper `<div>` (the APPWINDOW in `#neui-root`, else `<body>`). The loop is `drain → (paint on rAF | wait)` with `web_wait` as an `EM_ASYNC_JS`. It also covers ResizeObserver with `device-pixel-content-box`, the DPR change path, focus / blur, the 16 ms heartbeat, stb images over MEMFS, the in-frame menubar band (painted, not yet clickable) and the cursor.
- **`canvas2d_smoke`**: 35 checks (the Cairo smoke checks plus round trip, tint, gradient and scale 2) pass in Edge 155, Firefox 157 and WebKit 27.2. Firefox's premultiplied round trip is off by one step, inside the tolerance. CI runs it in Playwright's Chromium (`tests/web/run_page.mjs`).
- **On screen**: `neui_example` renders completely in Edge, Firefox and WebKit, at 1× and 2× (crisp), in both flavours and in Debug. That covers the menubar band, all widgets, JPEG / PNG images with the `@2x` ladder, compound knobs, QR, the section and the CPU-shader panel. `path_example` and `arc_example` are correct. `neui_shader_example` compiles 7 shaders and bakes 391 images in about 2 s; its animated row runs on the session clock at ~25 paints/s against a 30 fps cap.
- **Examples** use a minimal shell (`examples/web/shell.html`) instead of Emscripten's default page.
- **Release `neui_example` wasm** grew to 1.0 MB (JSPI) / 1.87 MB (Asyncify) now that the host actually draws.

### Stage 3: `platform_web` core: input, focus, timers, theme, resources (L)

Builds on 2a (`hosts/crossplatform/platform_web.{cpp,js}`, helpers in `hosts/shared/web/`). Logic that Linux already solved is **copied with "mirrors platform_linux.cpp:NNN" comments**, not extracted: the Linux routing is tied to `LinuxWindow` fields, X event types and the XI2 state.
- **Frames:**
  - APPWINDOW mounts into the configured container (default `#neui-root`, else `document.body`) and fills it.
  - create() w/h is the size used when the container has none.
  - APPWINDOW `set_pos` / `set_size` (`widgets.cpp:481-513`) resize the container only when neui created it; otherwise CSS wins.
  - `show` / `hide` use `display`; the title goes to `document.title` only in full-page mode; the icon is a no-op; size constraints become CSS min/max.
  - Closing a frame destroys only its canvas; the widgets stay (the documented contract).
- **Pointer:**
  - Pointer Events with `setPointerCapture` as the capture model; enter / leave like Linux `do_motion` (:685).
  - Press / motion overlay pre-check order: popup → toast → combo → menubar (:578-589, :694-709).
  - Double-click synthesis at 400 ms / 4 px (:591-605); right button → RBUTTON; `contextmenu` suppressed only when neui consumed the event (rule 5); touch / pen → left button.
- **Wheel:**
  - An explicit pixel-precise path: `deltaMode 0` → the shared `scroll_kinetics` smooth path (GRID / SECTION as the XI2 path does, :1246-1317).
  - Line / page `deltaMode` → notches.
  - Normalise Chrome's ±100 px mouse notches and read `deltaMode` first (Firefox).
  - `preventDefault` only when neui consumed the event, so page scrolling is not trapped.
- **Keyboard (no IME yet):**
  - `hosts/shared/web/keys_web.h` maps to `NEUI_KEY_*`. Letters and digits come from `key` (VK letters follow the layout; `code` would break Ctrl+Z on QWERTZ), everything else from `code`.
  - Cmd = `NEUI_KMOD_CTRL` on Apple browsers.
  - Key order as Linux (:735-793): Tab → `focus_next`, open menubar, `try_menubar_accel`, KEYDOWN, `handle_input_key`, KEYCHAR (one codepoint, no Ctrl / Cmd).
  - `preventDefault` only when the key was handled.
- **Focus:** canvas `focus` / `blur` → `_os_focused` + `WIDGET_FOCUS`.
- **Cursor:** `canvas.style.cursor`.
- **Timers:** animation timer, toast, scroll bounce → `web_wait` deadlines; `platform_now_ms` from `performance.now()`. An IntersectionObserver skips paints of canvases that are off-screen.
- **Theme:** `hosts/shared/web/theme_provider_web.h` (modelled on `theme_provider_linux.h`) reads `prefers-color-scheme` in `platform_init`, before any Session exists. The change listener enqueues. `platform_reduced_motion` comes from `prefers-reduced-motion`.
- **Metrics:** add a `measure` seam through `metrics_add_measure_seam` (`metrics.h:110-117`) backed by `measureText` (the default is a 0.5×size estimate), and a safe-area seam from `env(safe-area-inset-*)` for iPad.
- **Images and resources:**
  - `platform_load_image[_bytes]` via `hosts/shared/image_loader_stb.h`.
  - Example resources are bundled into MEMFS by `--preload-file <src>@<name>` under the names the code uses (`myimage.png`, `@2x` JPGs, `components/`), so the `@2x` ladder (`asset_store.h:1033-1081`) and `resource_provider.h` work unchanged.
  - `font_loading_example`'s system font paths don't exist in a browser (`:76-88`), and there is no `examples/fonts/`. Bundle one OFL font for it.
  - Product clients should prefer `NEUI_API_RESOURCE_CLIENT`.
- **Debug:** URL query parameters `?NEUI_DEBUG_PAINT=1` are mapped into `ENV` in `preRun` (`getenv` is cached, `host.cpp:2390,2426`). stderr goes to `console.error`. `main` gets `argv` from the mount options.

Exit: `readme_example`, `path_example`, `overlay_example`, `neui_grid_example`, `neui_tabview_example`, `neui_section_scroll_example`, `neui_shader_example` (CPU, animated) and `neui_partial_repaint_example` run interactively. The last one needs its `pump_once + sleep_for` loop (`:150-182`) ported to the web pattern. The how-to-port §12 checks for mouse, keyboard, focus, resize, DPI, theme and images pass in Chrome and Firefox, including a manual check in a real Firefox that the keydown / dragover verdicts (rule 5) take effect.

#### Stage 3 results (2026-10-13)

- **Input** (`platform_web.cpp`, routing copied from `platform_linux.cpp`):
  - Pointer Events with pointer capture. Overlay pre-checks: popup → toast → combo → menubar.
  - Double-click synthesis (400 ms / 4 px), right button, leave.
  - Wheel: a pixel path into the smooth GRID / SECTION kinetics plus a line path for stepped widgets, with spring-back on the heartbeat.
  - Keys in Linux order: Tab, open menubar, accelerators, KEYDOWN, KEYCHAR.
  - Consecutive pointer moves coalesce in the queue.
- **Key mapping**: `hosts/shared/web/keys_web.h` (letters from `key`, digits / named keys from `code`, Cmd = `NEUI_KMOD_CTRL` on Apple, AltGr is not Ctrl+Alt), unit-tested in `tests/test_keys_web.cpp` (7 cases).
- **Verdicts (rule 5)**: the deferred `preventDefault` works for wheel, Tab, Space and context menu in Edge and WebKit. With a 400 px viewport:
  - a wheel over the listbox scrolls the list, not the page;
  - a wheel over an empty canvas area scrolls the page;
  - Space typed into the INPUTBOX does not scroll;
  - PageDown with nothing focused scrolls the page.

  In Playwright's Firefox, wheel and Tab work but Space still scrolls (its injected input runs the keypress default before microtasks). **Still to check by hand in a real Firefox.**
- **Driven in Edge, Firefox and WebKit** (`neui_example`, both flavours):
  - typing into the INPUTBOX, buttons, checkboxes;
  - slider and rotational knob drags, double-click reset to default;
  - knob right-click popup (through the nested modal loop);
  - listbox wheel, Tab traversal;
  - File menu by mouse + arrow keys, combo selection, toast animation.
- **Theme**: `prefers-color-scheme` read in `platform_init` and followed live (`hosts/shared/web/theme_provider_web.h`); "no preference" keeps the dark default.
- **Container sizing**: an APPWINDOW in a `#neui-root` with its own size fills it and follows it (700×420 → 900×520 checked); otherwise the canvas takes the create() size. Size constraints map to CSS min / max.
- **DPR changes**: a `matchMedia('(resolution: Ndppx)')` watcher re-reports every frame's size. The CDP device-scale override fires neither it nor ResizeObserver; with the record queued the backing store follows (1010 → 1515 px at 1.5).
- **Metrics**: a `NEUI_API_METRICS` measure seam backed by the browser's text measurement.
- **Debug**: `?NEUI_DEBUG_DAMAGE=1` (any `NEUI_*` query parameter) sets the environment variable before the host reads it.
- **`neui_partial_repaint_example`** yields with `emscripten_sleep(1)` on the web (a `sleep_for` spins without handing control back). It runs at 55 ticks/s and ~0.3 ms pump per tick.
- **Not done here**: the iPad safe-area seam (moved to Stage 8, needs a device), and `argv` from mount options (Stage 7).

### Stage 4: blocking UI: dialogs, popups, message box, menubar (M-L)

- **Message box extraction (desktop change, done just before it is needed):**
  - Move the backend-only layout / render / hit-test from `platform_linux.cpp:1684-1766, 1817-1868` into `hosts/shared/message_box_painted.h`; the X11 window and loop stay in place.
  - `message_box_linux.h` is gated on `__unix__`, which Emscripten defines, so it is reusable as is (done differently: it moved into the new header, see the results).
  - Linux is verified by CI and by an on-screen pass by the user.
- **Modal DIALOG** (`widgets.cpp:451`):
  - An absolutely positioned wrapper with a **DOM title bar** (title, close button, draggable, styled from the palette through CSS custom properties).
  - x / y are relative to the viewport (`position:fixed`), clamped into view; centred over the owner when they would land off-screen.
  - `platform_set_window_enabled(owner,false)` = a backdrop blocking pointer input + the input gate (Linux :1387-1437).
  - `teardown_window` ends the pump (`host.h:283-288`) and re-enables the owner. The close button goes through `handle_close` → `APP_QUIT` veto.
- **popup_menu:** works through the nested loop. Painted in the frame canvas, so it clips at the canvas edge, which matters for small embedded canvases (limitation, noted). Fix the latent bug that a multi-canvas page exposes: `paint_popup_menu` (`host.cpp:4358`) paints on every frame while open; gate it on the owning frame.
- **message_box** (`widgets.cpp:2026`): a modal wrapper + canvas painted by `message_box_painted.h`, run on `platform_run_modal_until`, so timers, toasts and other frames keep running (the Linux box loop has no timer, :1881-1886).
- **Menubar:**
  - `platform_menubar_in_frame()` = true, which reuses the in-frame band (`host.cpp:4679-5019`) and `try_menubar_accel` (:5135).
  - `platform_menubar_create` returns the non-null sentinel, as on Linux.
  - Document the browser-reserved shortcuts: Ctrl / Cmd + W / T / N can't be caught; Ctrl + S / P / F can, through `preventDefault` when handled.

Exit: `neui_example`'s About dialog (open / close / reopen), knob right-click reset, message box → toast, menus by mouse, keyboard and accelerators.

#### Stage 4 results (2026-10-13)

- **Message box extraction**: `hosts/shared/message_box_painted.h` holds the `NEUI_MB_*` decode (moved from `message_box_linux.h`, now deleted) and `MsgBoxPainted` (layout through an off-screen context, render, hit-test, Enter / Esc results). `platform_linux.cpp::run_message_box` keeps its X window and loop and calls it; the drawing is unchanged. **Linux still to verify**: CI build + an on-screen look at the box (no X11 build here).
- **DIALOG**: a floating wrapper (`position:absolute` in document coordinates, so it scrolls with the page) with a DOM title bar: title, close button (→ `handle_close`, so `APP_QUIT` can veto), drag to move. Colours come from the session palette through CSS custom properties and follow theme changes. The canvas stays the client area.
  - Placement: x / y keep their offset from the owner's create() position (the About dialog at 240,240 with its owner at 100,100 lands 140,140 into the owner's canvas), then clamped into the visible viewport. That replaces the planned "viewport coordinates": neui frames have no screen position on the web.
  - A dialog comes up focused (canvas focus); keys go to it.
  - `platform_set_window_enabled(owner, false)` adds a backdrop over the owner's frame: it dims it and takes its pointer input (wheel passes to the page). A press on it focuses the newest open modal. The input gate drops what still arrives.
- **message_box** on its own floating canvas (kind 3, not a `WebFrame`), centred over the owner, run on `platform_run_modal_until`: timers, toasts and other frames keep going (a toast animated while a box was up). Enter = default, Esc = cancel, close button = cancel. Boxes can stack.
- **Popups**: `Session::_popup_frame` records the frame a popup opened in. `paint_popup_menu` paints only there, and hover / key repaints go to that frame (they used to go to the session's first frame). On the web, a press in another frame dismisses the popup and moves there are ignored. Desktop hosts keep their routing.
- **Paint guard**: `platform_run_modal_until` and `platform_message_box` refuse (log, return at once / 0) while a frame paints.
- **Menubar**: the in-frame band and `try_menubar_accel` needed no web changes. Ctrl+O / Ctrl+S reach neui (`preventDefault` when handled). Browser-reserved shortcuts never reach the page: Ctrl/Cmd + N / T / W (and Shift variants), Ctrl+Tab, F11; `neui_example`'s "New" accelerator Ctrl+N is one of them.
- **Driven** in Edge (JSPI), Firefox and WebKit (Asyncify), WebKit (JSPI), dark and light: message box → Esc → toast "Cancel"; box → hover + click "No" → toast; a click on the blocked owner does nothing; Help → About by keyboard, close by OK, reopen, close by the title-bar button, reopen; Ctrl+O / Ctrl+S activate their items; the knob's right-click popup.

### Stage 5: text input, IME, clipboard (M-L)

- **Hidden text proxy:**
  - A visually hidden `<textarea>` per frame, focused while an INPUTBOX / MULTILINE / GRID editor has focus.
  - Positioned at `WidgetData::caret_rect_local` (`host.h:242`), so the IME candidate window and the mobile virtual keyboard appear at the caret.
  - `beforeinput` / `input` → KEYCHAR; `compositionstart` / `update` / `end` → `on_composition(START/UPDATE/RESULT/END, utf8, len, caret_byte, attrs)` (`host.h:223-236`; reference translations: win32 `:1288-1373`, macOS `:944-1113`).
  - Key events come from the proxy while it has focus, so dead keys work.
- **Clipboard** (synchronous seams `platform.h:203-227`; callers `host.cpp:1833-1862, 6203-6232, 7264-7284`):
  - `set_text` / `write_item`: update the in-process cache, then `navigator.clipboard.writeText` / `write(ClipboardItem)` (text/plain, text/html, image/png), fire-and-forget inside the transient user activation.
  - Ctrl / Cmd + V: the drain holds the paste KEYDOWN until the proxy's `paste` event delivers `clipboardData` (short timeout), fills the cache, then dispatches. No permission prompt.
  - Programmatic reads (menu "Paste", `NEUI_API_CLIPBOARD` from a click): suspend on `navigator.clipboard.read/readText()` (added to the blocking set). On denial, fall back to the cache.
  - `has_text` answers from the cache (possibly stale; clients already poll).
  - PRIMARY is a no-op.

Exit: Ctrl / Cmd + C / X / V round-trip inside neui and with other apps (Chrome, Firefox, Safari); a CJK IME composition commits as one undo unit; the Android / iOS virtual keyboard types into an INPUTBOX.

#### Stage 5 results (2026-10-13)

- **Text proxy**: a hidden `<textarea>` per frame (not on message boxes). While an INPUTBOX / MULTILINE of the frame has neui focus, DOM focus sits in it, placed at `caret_rect_local` after every drain and paint (`sync_text_proxy`). The page side keeps the wanted target (`W.applyFocus`), so a click that focuses the canvas is moved back at once, inside the gesture. Focus moving between canvas and proxy is not reported as focus / blur.
- **Keys**: plain typing still comes from keydown (`KeyboardEvent.key`). Keys an IME is processing (`isComposing`, `Process`, 229) are left to it. The proxy is kept empty: `beforeinput` cancels every non-composition edit; edits whose keydown reached neui are just cancelled, the rest become neui input:
  - `insertText` → one KEYCHAR per codepoint (virtual keyboards, emoji picker; `keyboard.insertText` checked);
  - `deleteContentBackward` / `Forward` → Backspace / Delete; `insertLineBreak` / `Paragraph` → Enter.

  Side effect: Space typed into a text widget no longer scrolls the page in Playwright's Firefox (the open item from Stage 3): the textarea takes the default action, not the page.
- **IME**: `compositionstart` / composing `input` (the proxy's value and selection = text + caret) / `compositionend` → `on_composition` START / UPDATE / RESULT + END. The browser gives no clause attributes, so the composition draws with the plain underline. Checked in Edge through CDP `Input.imeSetComposition` + `insertText`: the composition shows inline, commits, and one Ctrl+Z removes the whole commit.
- **Clipboard** (in-process copy `g_clip` + the browser):
  - writes (`set_text`, `write_item` for text/plain, text/html, image/png) update the copy and go to `navigator.clipboard.writeText` / `write(ClipboardItem)` within the key's activation; outside a secure context the copy command over a temporary textarea stands in;
  - Ctrl/Cmd+V is held (focus into the proxy first, so the paste event fires in every engine) until the paste event fills the copy, then dispatched as a "fresh" key record; a paste from the browser's own menu becomes a Ctrl/Cmd+V of its own; 300 ms without a paste event releases it unfilled;
  - every other read (`get_text` / `read_item` while the copy is not fresh, e.g. a menu Paste) suspends on `navigator.clipboard.readText` / `read` (5 s cap; may prompt) and falls back to the copy when refused; a paste carrying no text, HTML or file also keeps the copy (only a failed write of neui's own leaves that);
  - `has_text` answers from the copy; no PRIMARY.
- **Checked**: Edge (both flavours): copy reaches the system clipboard, an external `writeText` pastes in, the async read path (an untrusted Ctrl+V, so no paste event) pastes through `readText` and the loop keeps running. Firefox and WebKit (no clipboard permission in Playwright): copy → paste round-trips through the copy. Stage 3 / 4 interaction and page-scroll checks still pass.
- **Not checked here**: a real IME (Windows / macOS), Android / iOS virtual keyboards (no device), Safari's clipboard prompts. Not done: IME in GRID cell editing (no host does it: the GRID editor has no `on_composition`).

### Stage 6: drag and drop (M)

- **Drop target:**
  - `dragenter` / `over` / `leave` / `drop` → `Session::dispatch_dnd_*` (`host.cpp:833-866`); the `dropEffect` verdict through rule 5.
  - `drop` captures `DataTransfer` strings inside the event. Files are read with `File.arrayBuffer()` into a `DataItem` before the drop is queued.
- **Drag source:** browsers start drags only from a native `dragstart`. So `platform_dnd_begin_drag` (blocking set) supports **in-page** drags between neui frames and widgets: a nested `run_modal_until` with pointer capture and direct `dispatch_dnd_*`, as Linux does for internal drags. Dragging out of the page is deferred.

Exit: `neui_dnd_example` accepts text and files from the desktop and from page elements; `neui_dnd_source_example` drags between its own widgets.

#### Stage 6 results (2026-10-13)

- **Drop target**: `platform_dnd_register_window` arms the frame's canvas; `dragenter` / `dragover` → `dispatch_dnd_enter` / `move`, with enter / leave matched per page (`g_dnd_over`), because the browser enters the next canvas before it leaves the last. Formats come from `DataTransfer.types` (`text/plain` → `NEUI_MIME_TEXT`, `Files` → `text/uri-list`, other MIME types as they are, plus a file's own type when `items` tells it before the drop). Suggested action: `effectAllowed` + Ctrl / Shift through the shared `dnd_suggest_action`.
- **Verdict**: the drain sets the kept event's `dropEffect` (rule 5); the canvas also applies the last answer at once, so a browser that misses the microtask is one event late, not wrong. Over a registered canvas the default is always cancelled and refusal is `dropEffect = "none"`. That differs from the plan's "preventDefault only when consumed", deliberately: otherwise a file dropped where neui refuses opens in the tab and unloads the app.
- **Drop**: strings are read inside the event, files afterwards (`File.arrayBuffer()`); then the drop is queued. Files are written to the in-memory file system as `/tmp/neui-drop/<n>/<name>` and listed as percent-encoded `file://` URIs in `text/uri-list` (appended to any list the drag carried), so desktop client code that opens dropped paths works unchanged. The first file of each type is also there under its own MIME type, as bytes. A drop's files stay until the next drop; a file over 256 MB is left out.
- **Drag source**: `platform_dnd_begin_drag` runs an in-page drag on `platform_run_modal_until`. Window-level capture listeners take every pointer and key event while it runs and report the neui frame under the pointer (`elementFromPoint`, frames registered as drop targets, any session on the page). Esc / pointercancel / the window losing focus cancel; Ctrl / Shift pick copy / move / link as on the desktop. The preview (`begin_drag_with_preview`) follows the pointer as a fixed canvas; the cursor shows the answer (`copy` / `grabbing` / `alias` / `no-drop`). The item goes to the drop as it is. After the drag the source session's pressed widget is cleared (its release went to the drag). Dragging out of the page is not supported (deferred).
- **Checked**:
  - Edge (JSPI): a desktop drag with text + two files (CDP `Input.dispatchDragEvent`) drops both as URIs, and the files are in MEMFS with the right sizes; the same drag over the margin is refused and the page stays the app; a real drag from a page element drops its text and the element sees `dropEffect = copy`.
  - Firefox (Asyncify): script-made drags with text and files drop.
  - All four builds (Edge JSPI, Firefox / WebKit Asyncify, WebKit JSPI): `neui_dnd_source_example` left → right gives copy, Shift = move, Ctrl+Shift = link; Esc, releasing over the source pane and releasing outside the canvas cancel; a plain click and a second drag work afterwards; no drag DOM is left behind.
  - Stage 3-5 regression scripts pass; `neui_tests` passes under node.
- **Not checked here**: a real desktop drag (Explorer / Finder), a touch drag (there is no long-press-to-drag yet; Stage 8 touch work).

### Stage 7: embedding and page interop (L): the product goal

- **Mounting:**
  - APPWINDOW mounts into the container the page chooses.
  - PLUGWINDOW + `NEUI_API_EMBED::set_parent` (`widgets.cpp:2041-2088`) takes a CSS selector, which the platform **copies** (`embed_parent` is stored as a raw `uintptr_t`, `:2020`), and embeds there.
  - The service loop (rule 7) drives it; `embed_event_fd` = -1 and `embed_pump_and_tick` is a no-op.
- **Module shape:**
  - `-sMODULARIZE -sEXPORT_ES6`, with `.wasm` / `.data` resolved from `import.meta.url` (`locateFile`).
  - Compile once and instantiate per mount (`instantiateWasm`), so several apps can share a page.
  - Wrapper `neui-web.js`: `await NeuiApp.mount(element, {flavour:'auto', args, theme})`, `app.unmount()`, `app.postMessage()`, `app.on('message')`.
- **`NEUI_API_WEB`** (`include/neui/d/web.h`), optional and exposed on web only, following the `NEUI_API_IOS` pattern:
  - mount element per frame (set before show)
  - send a string / JSON to the page, plus a client callback interface for page messages (delivered through the queue)
  - page lifecycle events (`visibilitychange`, `pagehide`)
  - theme mode set by the page (product pages run their own dark mode)
  - query the flavour

  Appended per the vtable-append rule.
- **Unmount:**
  - Enqueued. Closes **every** frame (`endsession` closes only the first root, `host.cpp:430-443`) through the no-veto "gone" path (`platform_linux.cpp:500,509`), so a client that vetoes `APP_QUIT` cannot hang it.
  - `run()` returns, the client cleans up, the module exits.
  - All canvases, listeners, observers and `FontFace`s are removed.
- **Leak test:** mount / unmount 100× and check that DOM node counts stay flat and no detached instance stays alive (heap snapshot: count `WebAssembly.Instance`). The wasm heap size says nothing once an instance is dropped.
- **Page etiquette:** no `preventDefault` unless neui consumed the event; no global listeners beyond `matchMedia`; focus only on a click or an explicit request; respect `inert` / hidden containers.
- **Deliverables:**
  - `examples/web/`: a host page with two apps side by side, a PLUGWINDOW in a resizable panel, and page↔app messaging.
  - A `dist/` layout (`.wasm`, `.js`, `neui-web.js`, `.data`). No COOP / COEP is needed, since there are no threads.

Exit: the demo page mounts / unmounts both apps, messages flow both ways, the leak test is flat.

#### Stage 7 results (2026-10-14)

- **Module shape**: `neui_link_example(<target> WEB_MODULE)` links an ES module (`-sMODULARIZE -sEXPORT_ES6 -sEXPORT_NAME=createNeuiApp -sENVIRONMENT=web`, `.mjs` + `.wasm` [+ `.data`]). Built here: `neui_web_embed_example` (new, `examples/web_embed_example.cpp`) and `neui_example_module` (`examples/main.cpp` with its resources).
- **Page side**: `examples/web/neui-web.js`. `NeuiApp.mount(element, { module, flavor, args, theme })` imports the module once, compiles its wasm once (`compileStreaming`, cached per URL) and instantiates it per mount (`instantiateWasm`). `module` may be `{ jspi, asyncify }`; 'auto' picks JSPI where `WebAssembly.Suspending` exists. Then `app.on('message' | 'exit' | 'error')`, `app.postMessage(data)` (non-strings as JSON), `app.setTheme('dark' | 'light' | 'auto')`, `await app.unmount()`. Messages the app sends before the first listener (its startup runs before `mount()` resolves) are held for it. `.data` packages are fetched per mount (the browser cache covers repeats).
- **`NEUI_API_WEB`** (`include/neui/d/web.h`, exposed by the web xpl host only, NULL elsewhere): `set_mount(frame, selector)`, `post_message(data, length)`, `page_state()`, `flavor()`. Page → app arrives as events in a new category 0x000C: `NEUI_EVENT_WEB_MESSAGE` and `NEUI_EVENT_WEB_PAGE` (`visibilitychange` / `pagehide` → `NEUI_WEB_PAGE_VISIBLE / HIDDEN / LEAVING`), payload `neui_event_web_t`. They go to every live session, `widget` = its oldest frame. Events instead of the planned client callback interface, following the iOS precedent (`NEUI_EVENT_IOS_ENVIRONMENT_CHANGED`).
- **Mounting**: a frame's selector is searched in the app's mount element first, then in the document; no selector = the mount element (then `#neui-root`, then `<body>`). The frame fills a container that has a size and follows it. `NEUI_API_EMBED::set_parent` takes the selector as its `native_parent` (copied); `event_fd` = -1, `pump_and_tick` a no-op. Dialogs still float in `<body>`.
- **Service loop** (rule 7): `neui_web_service`, a `JSPI_EXPORTS` export, started in a fresh task from a frame's show when no loop runs. It returns at once if `main` has entered `run()` by then. It runs the frames of a client whose `main` returned (the embedded PLUGWINDOW of the demo calls `set_parent` and returns, as in a DAW), and an unmount that comes after `run()` returned.
- **Unmount**: a queued record. Every frame closes through the "gone" path (APP_QUIT is delivered, a veto is ignored); open message boxes, an in-page drag and modal loops end; `run()` returns and the client's cleanup runs. No new frame or blocking call is accepted after that. When the outermost loop ends, the page side is told; after an unmount every page-level listener (media queries, `visibilitychange`, `pagehide`), every DOM node and the client fonts (`document.fonts`) are removed. An app that ends by itself (its last window closed) reports `exit` and is unmounted the same way.
- **Fixed along the way**: `loop_turn` waited for one more record after a drain that set `g_quit`, so `run()` returned only at the next input event after the last window closed (on unmount: never).
- **Checked** on `examples/web/embed.html` (two APPWINDOW apps side by side, a PLUGWINDOW in a CSS-resizable panel, the full `neui_example`):
  - Edge (JSPI), Firefox (Asyncify), WebKit (Asyncify and JSPI):
    - messages page → app (all three apps show the page's text) and app → page ("ready", button pings, knob values);
    - the panel resize reaches the PLUGWINDOW (RESIZE, status 560 × 200);
    - the page's theme switch (frames with `NEUI_ATTR_FOLLOW_SYSTEM_THEME`);
    - mounting doesn't take focus (`document.activeElement` stays `<body>`);
    - unmounting `neui_example` with its message box open (the box goes, `run()` returns);
    - unmounting and remounting the PLUGWINDOW app (service loop).
  - **Leak test**, 100 mount / unmount cycles of one app in about 1 s: DOM nodes 50 → 50, no fonts left. In Edge, live wasm instances after forced GC stayed at 2 (the two apps still mounted) of 105 created; counted through `WeakRef`s on every instance, because DevTools' `Runtime.queryObjects` never finishes on a page with JSPI apps running. Firefox / WebKit ran 30 cycles with DOM counts only (no forced GC there).
- **Not done here**: a `dist/` layout with both flavours side by side (the page loads modules from its own build directory; `{ jspi, asyncify }` in `mount()` is ready for it). That goes with the Stage 8 docs.

### Stage 8: performance, polish, documentation (M-L)

- **Profile** `neui_grid_example` (10k rows), `neui_partial_repaint_example` (48 knobs) and `neui_shader_example`, plus filters over surface readback. Stage 0 measured the JS↔wasm crossings at ~15 % of a heavy frame, so the command buffer is only revisited if profiling of real examples disagrees.
- **Shaders:** an optional native wasm SIMD128 path in `nsl_math.h` next to SSE2 / NEON (`-msimd128`), bit-exact against the goldens (no relaxed SIMD, no FMA); the portable path stays selectable.
- **Size:** compare `-Oz` and `-O3`, `wasm-opt`, `--closure 1`; record numbers for both flavours.
- **Gradients:** REPEAT / MIRROR through stop expansion.
- **Touch:** long-press → right click (as on iOS).
- **Browser matrix** (Chrome / Edge, Firefox, Safari on macOS and iPadOS) over the full how-to-port §12 checklist.
- **Docs:**
  - `docs/host-web.md` (build, flavours, embedding API, what works, what is stubbed, limits)
  - CLAUDE.md (build line, platform selection, file map, `NEUI_API_WEB`, Plans)
  - `plans/how-to-port.md` (web as a worked example, including stack switching)
  - `docs/deferred-issues.md`

#### Stage 8 results (2026-10-14)

- **Profiling** (Edge, Release JSPI, headless; `?NEUI_DEBUG_PAINT_STATS=1` + a CDP CPU profile):
  - `neui_grid_example`, smooth wheel scrolling: ~1.8 ms per paint at 60 fps; the busy time is mostly the browser's own `fillText` / `save` / `clip`, wasm ~10 %, the JS glue ~17 %. (The example has 500 rows; a GRID paints only the visible ones, so 10k rows change nothing.)
  - `neui_partial_repaint_example`: 0.3 ms per partial paint, 55 ticks/s.
  - `neui_shader_example`'s animated row: 15 ms per paint, 90 % of it in the CPU shader executor.

  The JS ↔ wasm crossings are not the cost anywhere, so **no command buffer**: the decision from Stage 0 stands.
- **SIMD128**: a wasm SIMD128 arm in `nsl_math.h` next to SSE2 / NEON (only exactly specified operations: no relaxed SIMD, no `f32x4.min / max`, whose NaN / signed-zero rules are not `m_min / m_max`). `-msimd128` on `neui-shader` only (`NEUI_WEB_SIMD`, default ON). The shader goldens stay bit-exact in both flavours. `neui_shader_bench` under node, ns per pixel portable → SIMD128: ring_glow 44.6 → 19.5, plasma 147 → 53, fbm5 563 → 201, metal_knob 210 → 79, tex_blur9 655 → 408. The shader example's paint dropped from 15 to 8.2 ms, CPU from 491 to 286 ms/s.
- **Size** (`neui_example`, wasm / gzip -9, the shader library at `-O2` throughout):

  | Link flags | JSPI | Asyncify |
  |---|---|---|
  | `-O3` (Release) | 1071 / 365 KB | 1991 / 568 KB |
  | `-Os` | 879 / 322 KB | 1809 / 519 KB |
  | `-Oz` | 754 / 289 KB | 1752 / 490 KB |
  | `-O3 --closure=1` | wasm the same, JS 105 → 56 KB | wasm the same, JS 132 → 67 KB |

  `-Oz` costs little speed (steady-state paints the same within noise, the first frame 17 → 31 ms). Documented as the option for size; the CMake Release default is unchanged.
- **Gradients**: REPEAT / MIRROR by stop expansion in the Canvas 2D backend: the gradient is stretched over every tile the fill can reach (the rect, or for a path the visible canvas mapped back through the transform), each tile gets the stops, reversed on odd tiles for MIRROR; radial tiles grow from the focal point; past 256 tiles it clamps. Four new `canvas2d_smoke` checks (linear repeat, mirror, path repeat both ways, radial clamp / repeat / mirror at t = 1.25) pass in Edge, Firefox and WebKit.
- **Touch**: a long press (500 ms within 8 px, the primary touch) is the right click: the left press ends without a CLICK, RBUTTON_DOWN / UP go to the widget under the finger, the finger's release is swallowed. Checked in Edge with emulated touch (CDP `Input.dispatchTouchEvent`): the knob's reset popup opens while the finger is held and stays after release; a tap and a moved touch open nothing. The canvas also suppresses the touch callout and text selection.
- **Popup menus** (pre-existing bugs found by the Linux check): hover counted items from 0 while paint, click and keys count from 1, so the highlight sat one row off on every xpl platform; fixed in `Session::handle_popup_hover`. Linux and the web now route keys to `handle_popup_key` first, as win32 does (Esc / Enter / Space / Up / Down). macOS / iOS still don't (`docs/deferred-issues.md`). Checked on the web, and on Linux by `neui-linux` (926d0ba, Xvfb): hover highlights under the pointer, Esc closes without resetting, Down + Enter and a click reset, a click outside dismisses; everything else pixel-identical to 4e41031 in both themes.
- **Deploy layout**: the `web` install component. `cmake --install <build> --prefix dist --component web` for a JSPI and an Asyncify build gives `dist/{neui-web.js, embed.html, jspi/, asyncify/}`; `embed.html` loads from the flavour directories there. Checked: the Stage 7 script against `dist/` in Edge and Firefox, and with `WebAssembly.Suspending` removed the page loads the Asyncify modules and all four apps run.
- **CI**: the first web run showed the Asyncify job killed while linking: `cmake --build --parallel` without a number is an unbounded `make -j`, and twenty Asyncify links at once ran the runner out of memory. Now `--parallel "$(nproc)"`. JSPI and every desktop job were green.
- **Linux check** (the peer session `neui-linux`, of `4e41031`): GCC build warning-free with `-DNEUI_WERROR=ON`, ctest 2/2, all 18 examples and 7 X11 harnesses build, and every on-screen check (message box, knob popup, About, menus, accelerators, DnD) is pixel-identical to the pre-wasm baseline `0546745`.
- **Docs**: `docs/host-web.md` (new), CLAUDE.md, `plans/how-to-port.md` (the web as the worked example of a platform that cannot block), `docs/deferred-issues.md`, `d/gradient.h`.
- **Browser matrix**: Edge (JSPI), Firefox and WebKit (both flavours) through Playwright for every stage; Safari in the Mac check below. Still open: real touch devices, a real CJK IME, the iPad safe-area seam (needs a device).
- **Mac check** (the peer session `neui-mac`, of `5f452b4`): the native macOS host, the macOS xpl host, both web flavours built there warning-free with ctest green, and Safari 26.6 (no JSPI, so Asyncify) plus the iPad simulator's Safari 26.5 over the Stage 3-7 checklists. Bugs it found and fixed on the Mac (pre-existing on every xpl platform unless noted): `WIDGET_PREUPDATE` carried the tree slot instead of the widget id (so `neui_example`'s knob value text never showed); `open_popup_menu` placed the popup without the menubar band (24 px above the pointer on Linux and the web); a stale `swallow_release` after Help > About ate the next click (web, Linux); popup keys on the macOS xpl host. Fixed here after its report: pixel wheel slivers over a line-scrolling widget scrolled the page (Safari's 40 px notch and trackpad deltas never reached a line at 100 px per notch); a JSPI page in a browser without JSPI hung at "Downloading"; Apple shortcut labels in the in-frame menubar; the `embed.html` plug panel's resize grip under the canvas (Safari). Recorded: Safari keeps Cmd+O. Not verified: a real CJK IME (dead keys compose), a file drag from Finder, real devices.

### Deferred (out of scope, recorded in the docs)

- A WebGL2 GPU shader path (a new `NEUI_SHADER_LANG_*` + a GLSL ES emitter next to `nsl_gpu.cpp`).
- Accessibility (an ARIA mirror tree). Canvas UIs are invisible to screen readers; the desktop xpl host has no accessibility either, but web reviewers will ask.
- Drag out of the page; overlays reaching beyond the canvas; runtime URL image fetch; multi-window through `window.open`; threads.

---

## Critical files

- **New:**
  - `backends/canvas2d/{CMakeLists.txt, canvas2d_backend.h, canvas2d_backend.cpp, canvas2d.js}`
  - `hosts/crossplatform/{platform_web.cpp, platform_web.js}`
  - `hosts/shared/web/{keys_web.h, theme_provider_web.h, clipboard_web.h}`
  - `hosts/shared/message_box_painted.h`
  - `include/neui/d/web.h`
  - `tests/canvas2d_smoke.cpp` + Playwright runner
  - `examples/web/` (shell, host page, `neui-web.js`)
  - `docs/host-web.md`
  - CI web job
- **Modified:**
  - root `CMakeLists.txt` (EMSCRIPTEN arms, `neui_add_example` helper)
  - `hosts/crossplatform/CMakeLists.txt`, `backends/cairo/CMakeLists.txt`, `tests/CMakeLists.txt`
  - `hosts/crossplatform/platform_linux.cpp` (message box extraction only)
  - `hosts/crossplatform/host.cpp` (popup paint gate, paint-guard for blocking APIs, `NEUI_API_WEB` in `get_interface`)
  - `hosts/crossplatform/widgets.cpp` (refuse blocking calls in paint / non-suspendable context)
  - `.github/workflows/ci.yml`, `CLAUDE.md`
- **References:**
  - `include/neui/d/renderer.h`, `hosts/crossplatform/platform.h`
  - `platform_linux.cpp` (loop, frames, input order, close / teardown, message box, menubar)
  - `backends/cairo/cairo_backend.cpp` (text box, arc, path, damage rules), `backends/cg/cg_backend.mm`
  - `tests/cairo_backend_smoke.cpp`
  - `hosts/shared/image_loader_stb.h`, `hosts/shared/theme_provider_linux.h`, `hosts/shared/metrics.h`
  - `platform_ios.mm` (platform-only interface precedent)

## Risks

| Risk | Mitigation |
|---|---|
| Microtask-resume verdict doesn't hold in real Firefox (unverified, harness limitation) | manual Firefox check in Stage 3; query-only exports (rule 5 fallback) if it fails |
| JSPI missing in Safari / Firefox at ship time | Asyncify flavour + auto-pick loader |
| Asyncify instruments broadly → larger and slower; breaks with two suspended stacks | accepted as the fallback; single-loop rule (rule 7); real-code size measured in Stage 1 |
| Exceptions + Asyncify | resolved in Stage 0: JS exceptions for the Asyncify flavour |
| JS call overhead per draw | measured small in Stage 0; `c2d_*` indirection keeps a command buffer possible |
| Premultiplied round-trip loss in surfaces / filters | documented; test tolerance; `willReadFrequently` |
| Paste needs a gesture / permission | paste-event cache primary, async read fallback |
| Message box extraction regresses Linux | mechanical move; CI + on-screen pass by the user |
| Text pixels differ from desktop | accepted (behaviour parity, not pixel parity); shader goldens are CPU-side and unaffected |

## Verification

- **Every stage:** existing CI jobs green with `-DNEUI_WERROR=ON`; the web job green: both flavours built, `neui_tests` under node, and `canvas2d_smoke` in headless Chromium from Stage 2 on.
- **Locally on this Windows machine:**
  - Install the pinned emsdk.
  - `emcmake cmake -B out/web -DCMAKE_BUILD_TYPE=Release && cmake --build out/web`.
  - Serve `out/web` with `python -m http.server` or `emrun`.
  - Drive Chrome (the claude-in-chrome skill can open, click, type and screenshot) through each stage's exit list and the `plans/how-to-port.md` §12 checklist.
- **Linux regression (Stage 4):** the user runs `neui_example` on X11 and checks the message box.
- **Embedding (Stage 7):** demo page with two apps, a resizable PLUGWINDOW panel, page↔app messages, and 100× mount / unmount with flat DOM count and no detached instances.
- **Browser matrix (Stage 8):** Chrome / Edge (JSPI); Firefox and Safari (whichever flavour they get); iPadOS Safari for touch and the virtual keyboard.

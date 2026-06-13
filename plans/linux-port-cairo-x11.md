# Linux Port of neui — Plan

> **Storage note:** On execution this document is to be saved into the repo at
> `plans/linux-port-cairo-x11.md` and committed, so it travels with the code and
> is available on the Linux machine. It is written to be executed by Claude
> (Opus) running **directly on a Linux box** where compilers and toolchain are
> already present.

---

## Execution instructions for Claude (Opus), running on Linux

**Read these before starting, then work strictly phase by phase.**

### Environment assumptions
- You are on a Linux machine with a working C/C++ toolchain (gcc/clang), CMake ≥ 3.15, and an X11 desktop session (`$DISPLAY` set). A real display is needed for the Phase 2+ manual checks; the Phase 1 backend smoke test needs no display.
- Required dev packages (install with the distro package manager if `cmake` configure fails to find them). Debian/Ubuntu names:
  `libx11-dev libxext-dev libcairo2-dev libfreetype-dev libfontconfig1-dev`.
  Fedora: `libX11-devel libXext-devel cairo-devel freetype-devel fontconfig-devel`.
  `stb_image.h` is vendored into the repo (Phase 4) — no package.
- If a configure step fails on a missing dependency, install it and re-run; do not work around it by stubbing the dependency out.

### Working rules
1. **Branch first.** Create a branch (e.g. `linux-port`) off `main` before any edits. Commit only when the user asks, or at the end of each green phase if the user has authorized commits.
2. **One phase at a time, in order (0 → 4).** Each phase has an **Acceptance check**. Do not start the next phase until the current phase's acceptance check passes. Phase 0 must configure+link before any real code.
3. **Build command** (Linux is single-config Make/Ninja, *not* the macOS Xcode flow):
   ```bash
   cmake -B out/build -DCMAKE_BUILD_TYPE=Debug && cmake --build out/build -j
   ```
   Run the example: `./out/build/neui_example` (path may vary by generator — check `out/build/`). Run tests: `ctest --test-dir out/build` or the `neui_tests` binary directly.
4. **Reuse, don't reinvent.** Before writing each new TU, read its model file end to end: the Cairo backend models `backends/cg/cg_backend.mm`; `platform_linux.cpp` models `platform_macos.mm` (closest — software pressed-widget tracking, no OS grab) cross-checked against `platform_win32.cpp` (pump/modal shape); `image_loader_linux.h` models `hosts/shared/macos/image_loader_macos.h`; keys model `hosts/shared/macos/keys_macos.h`. Match the existing call shapes into `Session` exactly — do not invent new Session entry points.
5. **Keep the dependency tree minimal.** Cairo + FreeType + Fontconfig + Xlib/Xext + vendored stb_image only. Do **not** add GLX/OpenGL, Pango, HarfBuzz, GTK, Qt, or any plugin SDK. The DAW plugin-format adapters (VST3/CLAP/LV2) are explicitly **out of scope** — implement only the embedding *seams* in Phase 3.
6. **Don't regress Windows/macOS.** All edits to shared files (`platform.h`, `host.h`, `widgets.cpp`, top-level `CMakeLists.txt`) must keep the Win32/macOS/null builds compiling. New seam params get ignored-but-present implementations on the other platforms.
7. **After each phase:** build clean, run that phase's acceptance check, run `ctest`, then report status (what passed, any deviations) before proceeding. If an acceptance check can't pass on the current machine (e.g. no display for a manual check), say so explicitly rather than marking it done.

### Phase gate checklist (tick before advancing)
- **Phase 0** — `cmake` configures, finds Cairo/FreeType/Fontconfig/X11, and `neui_example` links (behavior still null is fine).
- **Phase 1** — backend offscreen smoke test passes (rect/text/arc/bitmap + `read_pixels_bgra` + `measure_text` sanity); `ctest` green.
- **Phase 2** — `neui_example` opens a real X11 window, paints widgets, mouse + keyboard + resize + close-quit work.
- **Phase 3** — embedding test harness renders neui inside a fake-host parent window, input + animation work with no neui-owned loop.
- **Phase 4** — image widget loads a PNG; deferrables remain clean no-op stubs.

---

## Context

**neui** is a C/C++ GUI framework that splits a client C API from per-platform host implementations. Windows and macOS are fully implemented; Linux currently falls back to a no-op null platform + null backend (it compiles and registers the host, but opens no window and paints nothing).

This plan ports neui to Linux by reusing the existing **crossplatform ("xpl") host** unchanged and adding only two new compilation units: a **Cairo software rendering backend** and a **`platform_linux.cpp`** X11 implementation of the platform seam. The required functional scope is **font rendering, image loading, graphics rendering, and keyboard/mouse input**, working in two modes:

1. **Standalone** — a top-level application window (the "main window that may host other plugins").
2. **Embedded** — a plugin window embedded inside a DAW (Reaper, Bitwig) via the host's own X11 event loop.

**Decisions made with the user:**
- **Renderer: Cairo software surface** (blitted via XShm), FreeType+Fontconfig for text. Maps 1:1 to neui's existing immediate-mode 2D backend interface, minimizes the dependency tree, and avoids GPU/driver variance — the most robust choice inside DAWs and over remote X.
- **Plugin scope: embedding seams only.** Add the neui-side primitives a DAW adapter needs (foreign-parent child window, connection fd, host-driven pump+tick). The actual VST3/CLAP/LV2 SDK glue is a separate later effort, out of scope here.

---

## Research backing: how Linux audio-plugin GUIs are built

The central constraint that shapes everything is the **event-loop ownership inversion** on Linux:

- A Linux audio plugin **cannot own its own blocking event loop**. The DAW owns the X11 event loop, and **Xlib is not thread-safe to share** across connections — a plugin that calls `XNextEvent` on the host's `Display`, or spins its own loop on a thread, produces hangs-on-exit and warnings ([JUCE forum](https://forum.juce.com/t/x-display-connections-across-threads-and-opengl-components/2890)). The established fix: the plugin **opens its own `Display` connection**, creates its window as a **child of the host-provided X11 window id (XID)**, and **registers its connection fd + a periodic timer with the host's run loop**.
- **VST3**: host passes the parent XID to `IPlugView::attached(parent, kPlatformTypeX11EmbedWindowID)`; the plugin registers fds/timers via `Steinberg::Linux::IRunLoop::registerEventHandler` / `registerTimer`. Reference implementations: [Ardour `vst3_x11_plugin_ui.cc`](https://github.com/Ardour/ardour/blob/master/gtk2_ardour/vst3_x11_plugin_ui.cc), [VSTGUI `vst3editor.cpp`](https://github.com/steinbergmedia/vstgui/blob/develop/vstgui/plugin-bindings/vst3editor.cpp). The Linux IRunLoop path has known host-quirks.
- **CLAP**: the `gui` extension is X11-agnostic about drawing; embedding uses [`clap.timer-support`](https://github.com/free-audio/clap) + [`clap.posix-fd-support`](https://github.com/free-audio/clap/blob/main/include/clap/ext/posix-fd-support.h) so the plugin renders/pumps from host callbacks "to avoid dealing with X11 threads."
- **LV2**: the UI `idleInterface` — the host calls `idle()` periodically; the plugin pumps non-blocking there.

**Rendering / dependency choices in this space:**
- [**pugl**](https://drobilla.net/files/pugl_docs/overview.html) (drobilla) is the canonical minimal embedding library — "no implicit context or mutable static data, can be statically linked," X11/Mac/Windows with **Cairo / OpenGL / Vulkan** backends. It is the closest analog to neui's platform seam and validates the architecture.
- [**DPF (DISTRHO)**](https://github.com/DISTRHO/DPF) uses **NanoVG over OpenGL** for GPU vector rendering; pulls GL deps + driver variance.
- **VSTGUI**'s Linux backend uses **X11 + Cairo** (software) — the same stack chosen here.
- For text, the universal Linux trio is **FreeType** (rasterize) + **Fontconfig** (match/fallback) + optional **HarfBuzz** (shaping; skippable for Latin). Cairo's FT backend wraps FreeType+Fontconfig directly.
- For images, **stb_image.h** (header-only, zero external deps) is the minimal-dependency decoder.

**Conclusion:** Cairo (software) + FreeType/Fontconfig + stb_image + Xlib is a small, universally-present dependency set that maps cleanly onto neui's existing backend interface and is robust for DAW embedding. GLX is avoided entirely.

---

## Architecture (what changes, what doesn't)

Linux **reuses the xpl host** (`hosts/crossplatform/host.{h,cpp}`, `widgets.cpp`). Host registration is unchanged — `neui_register_xplhost()` still fires; only the platform TU and backend swap. Two new TUs:

1. `backends/cairo/cairo_backend.cpp` — implements `neui_render_backend_t` (`include/neui/d/renderer.h`), modeled on `backends/cg/cg_backend.mm`.
2. `hosts/crossplatform/platform_linux.cpp` — implements the `platform_*` seam (`hosts/crossplatform/platform.h`), modeled on `platform_macos.mm` + `platform_win32.cpp`; replaces `platform_null.cpp` on Linux.

Plus a `hosts/shared/linux/` helper dir (mirrors `hosts/shared/macos/`) and a vendored `third_party/stb/stb_image.h`.

**Key facts grounded in the code:**
- Cairo is **top-left, Y-down** natively — drop all of CG's Y-flip logic.
- The Cairo backend **owns its image surface for the window's lifetime** (unlike CG, which rebinds an AppKit context per `drawRect:`), so **no `set_current_frame` seam** is needed.
- The xpl host already does **software pressed-widget tracking** (no OS pointer grab) — `Session::dispatch_mouse_event` / `_pressed_widget`, mirrored from `platform_macos.mm`.
- `WidgetData` (`host.h:72-78`) carries `void* native_handle`, `neui_render_ctx_t render_ctx`, `uint32_t dpi`. Color = `0xAARRGGBB`; bitmaps = BGRA8 premultiplied, top-down; coords = logical px @96 DPI. Shared backend math in `backends/shared/backend_util.h` (`argb_unpack`, `alpha_stack_*`, `font_size_q10`).
- A 64-bit XID does not fit the int/string `AttrBag` cleanly → embedding uses **dedicated seams**, not attrs.

---

## Phase 0 — Build wiring

**Create:**
- `backends/cairo/CMakeLists.txt` — `add_library(neui-backend-cairo cairo_backend.cpp)`; `pkg_check_modules(CAIRO REQUIRED cairo)`, `find_package(Freetype REQUIRED)`, `pkg_check_modules(FONTCONFIG REQUIRED fontconfig)`. Mirror `backends/cg/CMakeLists.txt`.
- `backends/cairo/cairo_backend.h` — `namespace neui_cairo_backend { neui_render_backend_t* get_backend(); }`. No `set_current_frame`.

**Modify:**
- `hosts/crossplatform/CMakeLists.txt`: add an `elseif(UNIX AND NOT APPLE)` branch selecting `platform_linux.cpp`, linking `neui-backend-cairo X11::X11 X11::Xext ${CAIRO_LIBRARIES} ${FONTCONFIG_LIBRARIES} Freetype::Freetype`, and adding `third_party/stb` to the include path. (`X11::Xext` for XShm.)
- top-level `CMakeLists.txt`: `if(UNIX AND NOT APPLE) add_subdirectory("backends/cairo") endif()` next to the APPLE branch; keep `backends/null` as fallback. No `src/neui.c` / `NEUI_HAS_*` change — Linux keeps `NEUI_HAS_XPLHOST`.

**Acceptance:** `cmake -B build && cmake --build build --target neui_example` configures and links on Linux (dependency discovery confirmed) before any real behavior.

---

## Phase 1 — Cairo backend (`backends/cairo/cairo_backend.cpp`)

Implement all 37 vtable entries, structured like `cg_backend.mm`. Process-wide font cache keyed `"family|weight|size_q10"` (identical to CG).

**Per-context struct** `CairoCtx`: `cairo_surface_t* surface` + owned `cairo_t* cr` (lifetime = ctx, not per-frame), `w_px/h_px`, `float scale`, `uint32_t dpi`, current path, `alpha_stack` (vector<float>), `font_stack` (vector<{string family,int weight}>), `is_offscreen`, and the X handles for blitting (`Display*`/`Window` from the `LinuxNativeSurface*` passed as `native_handle`).

**Mapping (each maps to the obvious Cairo call):**
- `create_context(native,w,h)`: read `Display*/Window/Visual/depth/scale` from the `LinuxNativeSurface*`; `cairo_image_surface_create(ARGB32, w*scale, h*scale)`; `cr=cairo_create`; baseline `cairo_scale(cr,scale,scale)` so draws take logical px. **No Y-flip.**
- `resize`: destroy + recreate surface/`cr` at new physical size; reapply baseline scale. (Platform recreates its XImage/SHM segment.)
- `begin_frame(clear_argb)`: `cairo_save`; clear alpha/font stacks; paint clear color with `OPERATOR_SOURCE`; restore `OVER`.
- `end_frame`: `cairo_restore`; `cairo_surface_flush`; **blit** the surface data to the window (XShmPutImage, fallback XPutImage). Backend holds the per-window `Display*/Window` so the blit targets exactly that connection — correct for both standalone and embedded.
- `fill_rect`/`draw_rect`: `cairo_rectangle` + `cairo_fill` / `cairo_set_line_width`+`cairo_stroke`; source via `argb_unpack(argb, rgba, alpha_stack_current())`.
- `push_clip/pop_clip`: `cairo_save`+`cairo_rectangle`+`cairo_clip` / `cairo_restore` (clip+transform share Cairo's gstate stack, as in CG).
- transform stack: `cairo_save/restore` + `cairo_translate/rotate/scale` (post-multiply; Y-down → positive sweep is CW, no inversion).
- alpha stack: software (`alpha_stack_push/pop`), folded into every draw's alpha — do **not** use `cairo_push_group`.
- path API: `cairo_new_path/move_to/line_to`; `arc` → `cairo_arc` (positive) / `cairo_arc_negative` (`end<start`); `close_path`; `fill_path`/`stroke_path` use `*_preserve` so fill-then-stroke works.
- `create_bitmap`: `cairo_image_surface_create_for_data` over a **copied** BGRA8 buffer (`ARGB32` == BGRA premul little-endian = our format). `draw_bitmap`: translate/scale + `set_source_surface` + `paint_with_alpha` for `tint==0xFFFFFFFF`; tint path (mask+multiply, mirroring CG) is lower priority.
- `get_scale_factor`/`update_dpi`: return/set `scale=dpi/96`.
- `get_context_generation`: return `0` (software never device-loses).

**Fonts (must be accurate — layout depends on it):**
- `push_font/pop_font`: software `FontState` stack.
- Resolution: cache `cairo_scaled_font_t*` keyed `"family|weight|size_q10"`. Build via **Fontconfig**: `FcPattern` with `FC_FAMILY` (empty → unset, `FcDefaultSubstitute` picks default sans), `FC_WEIGHT` (CSS 100–900 → `FC_WEIGHT_*`, mirror `css_weight_to_nsfontweight` in `cg_backend.mm`), `FC_PIXEL_SIZE=size*scale`; `FcConfigSubstitute`+`FcDefaultSubstitute`+`FcFontMatch` → `cairo_ft_font_face_create_for_pattern`. Fontconfig always substitutes (never null) → unknown families degrade gracefully, matching CG.
- `draw_text`: `cairo_font_extents` → vertical-center within `h` (baseline = top+ascent, like CG); clip to `(x,y,w,h)`; `cairo_show_text`.
- `measure_text`: same scaled font as draw; `cairo_text_extents` → return `x_advance` (advance width). Accuracy is required (input caret math, button auto-size).

**Offscreen:** `create_offscreen_context` = `cairo_image_surface_create(ARGB32,w_px,h_px)` + baseline scale, **no Y-flip** (simpler than CG). `read_pixels_bgra` = flush + **per-row** copy honoring `cairo_image_surface_get_stride` into a tight `w*4` buffer (contract forbids padding).

**Acceptance:** offscreen smoke harness (no X11) draws rect+text+arc+bitmap, `read_pixels_bgra`, asserts a known center pixel == fill color and `measure_text("Hello",14) > 0` within a few px of expected.

---

## Phase 2 — Standalone X11 platform (`hosts/crossplatform/platform_linux.cpp`)

`namespace xpl_host`. Implement the required subset; copy no-op bodies from `platform_null.cpp` for everything else.

**Per-frame `LinuxWindow`** (stashed as `wd.native_handle`): `Display* dpy`, `Window win`, `Visual*`/`depth`, `GC`, `XImage* ximage`, `XShmSegmentInfo shm`, `bool use_shm`, `Session*`, `widget_index`, animation flags (`toast_anim_active`, `last_tick_ms`). Platform fills a `LinuxNativeSurface{dpy,win,visual,depth}` and passes its address to `backend->create_context`.

**Window creation:**
- `platform_create_appwindow`: `XCreateWindow` child-of-root; `WM_DELETE_WINDOW` protocol; `XStoreName` from `wd.text`; event mask `Exposure|KeyPress|KeyRelease|ButtonPress|ButtonRelease|PointerMotion|StructureNotify|FocusChange`; create GC; allocate backbuffer (SHM-first, malloc fallback); `backend->create_context`; set `wd.dpi` from `Xft.dpi`/RANDR (default 96); bump appwindow count.
- `platform_create_plugwindow(...,parent_xid=0)`: standalone = borderless top-level. Embedded variant in Phase 3.
- `platform_create_dialog`: top-level with `WM_TRANSIENT_FOR`; defer input-block.

**`dispatch_x_event(XEvent&)`** (shared by run/pump/modal), translating to the same `Session` calls macOS makes:
- **Expose** → coalesce → `Session::paint_frame(render_ctx, widget_index)` → blit.
- **ConfigureNotify** → `backend->resize` + recreate XImage/SHM + `Session::resize_render_ctx(idx, w_phys, h_phys)` + repaint (mirrors win32 `WM_SIZE`).
- **ButtonPress** 1/2/3 → `widget_at` → `set_focus`+`set_pressed` (btn1) → `neui_event_t` `MOUSE_BUTTON_DOWN`/`RBUTTON_DOWN` with `buttonmap` (MK_* bits, numeric per `platform.h:13-17`) → `dispatch_mouse_event`. Synthesize `DBLCLICK` from two presses within a time+distance window (X has no native double-click).
- **Button4/5** (+6/7 horizontal) → scroll event (shape per `platform_macos.mm` `scrollWheel:`). XInput2 smooth scroll = later nice-to-have; classic notch first.
- **MotionNotify** → `set_hovered(hit)`; if `_pressed_widget` set and btn1 held, route to pressed widget (mirrors macOS `mouseDragged:`) → `MOUSE_MOVE`. No `XGrabPointer` initially (software tracking covers in-window drags).
- **KeyPress** → `XLookupString` (UTF-8) + keysym → `NEUI_KEY_*` via new `hosts/shared/linux/keys_linux.h` (mirror `keys_macos.h`); modifiers `Shift/Control/Mod1` → `NEUI_KMOD_*` (primary modifier `NEUI_KMOD_CTRL` = Control on Linux). Fire focused `on_keydown`, then `on_keychar` for printable codepoints.
- **FocusIn/Out** → focus-outline repaint.
- **ClientMessage** `WM_DELETE_WINDOW` → close; if last appwindow, set quit flag.

**DPI/scale:** `platform_get_scale_factor` parses `Xft.dpi` (`XResourceManagerString`+`XrmGetResource`), falls back to RANDR, default 96→1.0.

**Event loop + 16ms heartbeat (standalone):**
- `platform_run()`: `select()`/`poll()` on `ConnectionNumber(dpy)` **+ a `timerfd`** at 16ms. X readable → drain via `XPending`/`XNextEvent`→`dispatch_x_event`. Timer tick → for frames with active animation (toast / grid+section spring-back), repaint to advance phase using `platform_now_ms()`. Exit when appwindow count hits 0.
- `platform_pump_once()`: non-blocking `while(XPending) dispatch`; return `false` on quit (mirrors `PeekMessage`).
- `platform_run_modal_until(bool* keep)`: nested blocking `select` loop draining while `*keep`; re-signal quit on exit (mirrors win32). Used by popup menus.
- `platform_start/stop_toast_animation`: set/clear `toast_anim_active`. `platform_now_ms`: `clock_gettime(CLOCK_MONOTONIC)`.

**Acceptance (first-runnable milestone):** `neui_example` opens a real X11 window; widgets paint via Cairo; clicks toggle buttons / focus inputs; keyboard types into an input box; resize repaints; close button quits.

---

## Phase 3 — Embedding seams (DAW plugin window)

Expose the primitives a DAW adapter needs; neui owns **no** event loop in embedded mode.

**Foreign parent (new seams in `platform.h`):**
```c
// Reparent target for the next plugwindow create; xid is the DAW-provided X11 Window.
void platform_set_embed_parent(Session*, uint32_t widget_index, unsigned long parent_xid);
// parent_xid 0 = standalone.
void platform_create_plugwindow(Session*, uint32_t idx, WidgetData&, unsigned long parent_xid);
```
- Add `unsigned long embed_parent_xid = 0` to `WidgetData`; update the one caller in `widgets.cpp` (~line 280) to pass it (default 0). Win32/macOS/null get the extra param (ignored on Win32/null).
- When `parent_xid != 0`: open a **dedicated `Display`** (`XOpenDisplay`) for this instance, `XCreateWindow(parent=parent_xid)` (create-as-child, not `XReparentWindow` — avoids the WM-intercept race), no WM protocols/decorations.

**Event source + host-driven tick (new seams in `platform.h`):**
```c
int  platform_embed_event_fd(void* native_handle);        // ConnectionNumber(dpy)
void platform_embed_pump_and_tick(void* native_handle);   // drain X events; if >=16ms, one anim tick + repaint
```
`platform_embed_pump_and_tick` = `while(XPending) dispatch_x_event` then a 16ms-gated animation tick + repaint (the host's periodic callback is the **only** heartbeat — no neui timerfd in embedded mode). The DAW adapter (out of scope) registers `platform_embed_event_fd` with `IRunLoop::registerEventHandler` / CLAP posix-fd, and calls `platform_embed_pump_and_tick` from `IRunLoop::registerTimer` / CLAP timer / LV2 `idle`.

**Acceptance:** a test harness (not a real DAW) creates an X11 window as a fake "host parent," calls `platform_set_embed_parent` + `platform_create_plugwindow(...,parent_xid)`, then drives `platform_embed_pump_and_tick` on a 16ms cadence while selecting on `platform_embed_event_fd`: neui renders inside the parent, mouse/keyboard work, and a toast animates smoothly with no neui-owned loop.

---

## Phase 4 — `hosts/shared/linux/` helpers + deferrables

**Essential (Phase 2):**
- `hosts/shared/linux/image_loader_linux.h` — header-only, mirrors `image_loader_macos.h`: `load(path,&w,&h)->uint8_t*` (BGRA8 premul) + `free_pixels`, satisfying the `AssetStore<Loader>` policy in `hosts/shared/asset_store.h`. Uses **stb_image** (`stbi_load(...,4)` → RGBA8, then swap R/B + premultiply by A). `platform_load_image`/`platform_free_image` delegate here.
- `third_party/stb/stb_image.h` — vendored; `STB_IMAGE_IMPLEMENTATION` defined in exactly one TU.
- `hosts/shared/linux/keys_linux.h` — keysym→`NEUI_KEY_*` + X modifier→`NEUI_KMOD_*`.

**Deferrable (stub as null no-ops first; slot defined):**
- `clipboard_linux.h` — X11 selections (`PRIMARY`/`CLIPBOARD`).
- `dnd_linux.h` — XDND.
- `theme_provider_linux.h` — `org.freedesktop.appearance` portal dark-mode; default light.
- `message_box_linux.h` — neui-drawn modal via `platform_run_modal_until`, or stub 0.
- Native menubar — X11 has none; render in-UI or keep null no-ops.
- `platform_set_cursor` — `XCreateFontCursor(XC_sb_h_double_arrow)` for EW-resize; cheap, can do early.
- Window icon / size constraints — `_NET_WM_ICON` / `XSetWMNormalHints`.

---

## Risks / gotchas

1. **Xlib thread-safety:** `XInitThreads()` in `platform_init`; **separate `Display` per plugin instance** in embedded mode (never share the DAW's connection).
2. **Multiple plugin instances per host process:** no global `Display`/`Window`/quit-flag for embedded windows — each carries its own `Display` in `LinuxWindow`. Standalone-only state (`g_display`, appwindow count, timerfd) stays standalone. Backend state is per-`ctx`; the process-wide font cache is read-mostly (guard with a mutex only if instances paint on different threads — neui UI is single-threaded per Display).
3. **Create-as-child over `XReparentWindow`** to avoid the WM-intercept race.
4. **GLX avoided** — software Cairo + XShm; no driver/version matrix.
5. **XShm fallback:** probe `XShmQueryExtension`/local-display; fall back to malloc `XImage` + `XPutImage` (remote X / denied SHM). `use_shm` per window.
6. **Font fallback:** Fontconfig always substitutes; empty family → default sans.
7. **measure_text accuracy:** same scaled font as draw; return `x_advance`.
8. **Cairo stride:** `read_pixels_bgra` copies per-row honoring stride into tight `w*4`.
9. **No Y-flip:** Cairo is top-left Y-down — drop CG's flip logic.
10. **Double-click synthesis** from two presses within time+distance.
11. **Drag past window edge:** add `XGrabPointer` on button-down later if needed (software tracking covers in-window).

---

## Verification

- **Backend unit smoke (no X11):** offscreen ctx → draw rect/text/arc/bitmap → `read_pixels_bgra` → assert known pixel + `measure_text` sanity. Add under `tests/` or a tiny standalone harness.
- **Existing Tier-1 tests** (`neui_tests`) must still build/pass on Linux (`ctest --test-dir build`) — they link no host/backend, so they validate the shared logic compiles in the Linux toolchain.
- **Standalone manual:** run `neui_example` (and `neui_grid_example`) on an X11 desktop — verify window opens, widgets paint, mouse click/drag/scroll, keyboard typing into INPUTBOX, resize, close-to-quit, image widget loads a PNG.
- **Embedding manual:** a small test harness (fake host parent window) exercising `platform_set_embed_parent` + `platform_create_plugwindow(parent_xid)` + `platform_embed_pump_and_tick`/`platform_embed_event_fd` on a 16ms cadence — verify render-inside-parent, input, and animation with no neui-owned loop. (Real VST3/CLAP/LV2 adapter is a later, separate effort.)

## Critical files
- `backends/cairo/cairo_backend.{h,cpp}` *(new — models `backends/cg/cg_backend.mm`)*
- `hosts/crossplatform/platform_linux.cpp` *(new — models `platform_macos.mm` + `platform_win32.cpp`)*
- `hosts/shared/linux/image_loader_linux.h`, `keys_linux.h` *(new — mirror `hosts/shared/macos/`)*
- `third_party/stb/stb_image.h` *(new — vendored)*
- `hosts/crossplatform/platform.h` *(modify — embedding seams)*
- `hosts/crossplatform/host.h` / `widgets.cpp` *(modify — `embed_parent_xid` field + create-plugwindow caller)*
- `hosts/crossplatform/CMakeLists.txt`, top-level `CMakeLists.txt`, `backends/cairo/CMakeLists.txt` *(modify/new — build wiring)*

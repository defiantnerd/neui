# Design notes

Distilled design rationale for **shipped** neui features — the "why we chose X
over Y" calls, trade-offs, and rejected alternatives. Implementation detail lives
in `CLAUDE.md`; open/deferred work lives in `TODO.md`; accepted quirks live in
`known_issues.md`.

These notes were condensed from the per-feature plan documents that used to live
in `plans/`. Those plans were removed once the work shipped — their **full
verbatim text remains in git history** if the complete narrative is ever needed.

---

## Painter + asset API

- **Curated paint interface over raw backend**: `WIDGET_PAINT` previously handed clients the full `neui_render_backend_t`, mixing draw primitives with lifecycle calls (`begin_frame`/`create_context`/`resize`); a client calling those mid-paint leaks or wipes the surface. A separate `neui_painter_api_t` excludes lifecycle by construction so the footgun is a compile error, not a doc warning.
- **Handle-based assets vs raw bitmap pointers**: bitmap create/destroy lived on the backend and required a `neui_render_ctx_t`, forcing clients to defer creation to first paint and re-cache on context loss (worst case: GPU upload every frame). A session-scoped `neui_asset_api_t` lets clients preload at init; `draw_asset(handle)` hides the per-ctx GPU cache and `void* bitmap` entirely.
- **Named "asset" not "bitmap" from day one**: the same API is intended to later serve SVG/vector/font/audio, so the role-based name avoids a future rename; a `kind` enum reserves slots so new kinds don't break ABI.
- **Callback dispatch with framework-owned brackets**: the framework pushes clip(bounds)/transform around the dispatch and pops after, so a client that forgets a pop is rebalanced — chosen over trusting client discipline.
- **Rejected for v1**: push/pop depth-checking on the painter stack (punted until a real debug need); extending `draw_asset` to expose raw pixel access.

## Render-to-surface (`NEUI_ASSET_KIND_SURFACE`)

- **Reuse over new kind**: once painted, a SURFACE is deliberately *indistinguishable* from a BITMAP for every downstream consumer (`draw_asset`, compound layers, `get_pixels_for_export`) — it reuses `AssetEntry::pixels` and the existing per-ctx GPU upload path, so the only genuinely new pieces are an off-screen backend context and a painter aimed at it.
- **Same painter API as `WIDGET_PAINT`**: clients render into surfaces with the interface they already know, rather than a separate offscreen-drawing API — lower conceptual cost.
- **Callback (`paint_surface(fn)`) over begin/end pair**: lifecycle is bulletproof — the framework owns `begin_frame`/`end_frame`/clip/readback/cache-invalidation. A begin/end pair would let clients leak half-drawn surfaces or recurse into themselves.
- **Graceful degradation on unsupported backends**: null backend returns `nullptr` from `create_offscreen_context`, so `create_surface` returns `asset_none` rather than failing — same precedent the framework uses elsewhere.
- **CPU readback-copy over zero-copy pointer return**: `read_pixels_bgra` copies into the entry buffer; returning a pointer+length was rejected because it complicates lifetime, and the copy is cheap.
- **Rejected/out of scope**: framework-tracked repaint-on-dirty (clients drive re-render themselves), threaded paint (UI-thread only, matching all other backend calls), cross-session sharing.

## Backend tint effect

- **Backend-native tint over CPU pre-tint cache**: the original CPU approach built a tinted BGRA buffer per `(asset, ctx, tint)` and cached it as a separate GPU bitmap — 4× GPU memory per tint, per-pixel CPU cost on miss, no eviction (animated/continuous tints thrash), and triplicated `tinted_bitmaps` lifecycle across three hosts. Moving tint to a `draw_bitmap` parameter applied natively (D2D effect / CG multiply) makes animated tint free and deletes the entire cache + helpers.
- **`0xFFFFFFFF` passthrough sentinel**: untinted draws bypass effect setup entirely so they stay byte-for-byte identical and cost nothing extra.
- **D2D 1.0→1.1 migration accepted as the real cost**: reaching the effects framework requires `ID2D1DeviceContext` (+ D3D11/DXGI swap chain). Justified not by tint alone (the "small visible win") but by unlocking shadow/blur/color-matrix for future compound layers — wiring the effect-graph model before the first feature needs it, rather than as a later parallel refactor.
- **CG uses `kCGBlendModeMultiply` + `ClipToMask`** over `CIFilter`: cheaper per-draw and matches D2D's multiply semantics for premultiplied BGRA; CIFilter (`CIColorMatrix`) kept as a documented fallback if the mask-clip mismatches D2D on partially-transparent pixels.
- **Tint stays internal/compound-only**: the public painter `draw_asset` does *not* gain a tint arg; tint routes through the internal thunk because it's a compound-attribute concern — keeps the public surface minimal.

## Path API completion (curves, fill-rule, stroke style)

- **Why now**: the path API was line-segments + arc only, forcing an SVG feeder (asvglib) to flatten every curve to a polyline and unable to express even-odd fills or dashed/rounded strokes. All three real backends support the missing primitives natively, so the flattening was pure loss.
- **By-value stroke struct over a state setter**: `stroke_path_styled(width, argb, const neui_stroke_style_t*)` mirrors the `fill_path_gradient` precedent — immediate-mode, describe-at-call-site, ABI-appended; plain `stroke_path` stays as the zero-overhead default (NULL style). A `set_stroke_style` state setter was rejected: it makes stroke stateful and the dash array (variable length) rides naturally in the struct.
- **Fill-rule as path-build state, not a fill param**: `set_fill_rule` is path state (reset to nonzero on `begin_path`) because D2D fixes the geometry sink's fill mode at sink-open, before any figure — a per-fill param couldn't be honoured there. Documented contract: set it before the first path verb. CG/Cairo set it at fill time (`EOFillPath`/`EOClip`, `cairo_set_fill_rule`), so the state model is the cross-backend least-common-denominator.
- **Per-backend quirks accepted**: D2D dash lengths are multiples of the stroke width, so absolute-px dashes are divided by width before building the transient `ID2D1StrokeStyle`; D2D's sink default is *alternate*, so `begin_path` explicitly sets winding for deterministic nonzero default. Cairo has no native quadratic, so `quad_to` elevates to a cubic via the shared `quad_to_cubic` (the one pure, Tier-1-tested piece). CG/Cairo bracket the styled stroke in save/restore so cap/join/dash don't leak into later strokes; D2D's style is per-call so it can't leak.
- **Compound `neui_path_cmd_t.args` widened `[5]`→`[6]`**: a cubic needs six floats; the struct grows (rebuild required, called out in CLAUDE.md). Accepted pre-1.0 rather than encoding a cubic as two commands. The component-JSON path mini-language gained `c`/`q` ops alongside `m`/`l`/`a`/`z`.

## Compound + declarative component format

- **Host-side first-class capability, not a client helper library**: an earlier draft proposed a client-side helper whose only justification was keeping a vendored JSON parser out of the hosts. Since `neui::mujson` is now compiled into core `libneui`, that premise is void — so a component is a real session asset parsed/materialized inside the host, killing the helper lib, the separate `neui_part_t` handle, and a bespoke `key=val` override parser.
- **The widget's `AttrBag` *is* the data plane**: a compound carries no parallel per-widget store; text-layer `{key}` templates and numeric `bind`s resolve against the widget's attrs at paint time. One compound shape is shared across N widgets, attrs are per-widget — so repetition isn't duplicated at runtime.
- **New `NEUI_ASSET_KIND_COMPONENT` bundling COMPOUND + BEHAVIOR + defaults**: reuses the shape-shared/AttrBag-per-widget reuse model so N instances share one compound+behavior; replaces ~45 lines of imperative `add_layer`/`bind`/`add_handler` with one load + one instantiate. (A *component* bundles a *compound* plus a *behavior* — deliberately close but distinct kinds.)
- **Vtable-append, no new `get_interface` entry**: the 4 component methods append to the existing `assets` vtable and `create_from_component` to `widgets` — every JSON field maps 1:1 onto an existing compound-layer/behavior prop, so adding a prop is one table row, not new plumbing.
- **Reuse mujson despite non-conformance**: deliberately leans on its bare keys / trailing comma / `//`+`/* */` comments so the shipped JSON can be annotated; `as_num` must accept both int and double arms because mujson resolves `0` to int and `0.5` to double.
- **Visual/behavioral split in distinct top-level keys**: layers/assets are tool-emittable, while `bind`/`behavior`/`params` are human annotation — kept separate so a designer tool can regenerate visuals without clobbering behavior (serialize is minimal-diff, structure-only).
- **Asset ownership policy**: path-resolved assets are owned by the component (released with it); `resolve_asset`-callback handles are borrowed — per-layer owned/borrowed flag.

## Font loading (`NEUI_ASSET_KIND_FONT`)

- **Push (register up front), not pull (request-on-miss)**: a plugin already knows exactly which files it ships, so lazy discovery buys nothing; pull's trigger point is font *resolution* during paint/measure (the hot path), where a synchronous callback forcing a DirectWrite collection rebuild mid-frame is the worst case. Push is purely additive — unknown families still fall back to host default.
- **Reference by family-name string, not by handle**: the backend font stack is already name-based (`push_font(family, weight)`), so registration introduces zero new draw-path addressing — it just widens which names resolve. The `neui_asset_t` owns only the *registration lifetime*; one `get_font_family` query lets the client learn the internal name when it differs from the filename.
- **Collisions documented last-wins, no alias mechanism**: plugin authors fearing a clash ship a uniquely-renamed family (common practice), avoiding the complexity of client-chosen private names.
- **Both memory and path forms**: `create_font` (bytes) is the general primitive (framework copies+owns the buffer since FreeType/DirectWrite in-memory loaders need it live); `create_font_from_file` is the convenience form, more robust on path/URL-preferring backends.
- **Two-pronged seam, asymmetric by platform**: painted text needs a process-level backend `register_font`; native-control text (HFONT/NSFont) is free on macOS (CTFontManager process scope also feeds `NSFont fontWithName:`) but Win32 native needs a *second* registration (`AddFontMemResourceEx`) because a DirectWrite custom collection does not expose faces to GDI `CreateFontW`. This asymmetry is the main cross-cutting cost.
- **Factory/process-level registration, not per-ctx**: unlike offscreen contexts, font registration takes no `ctx` and affects all contexts; `release_context` must not drop FONT entries (only session teardown does), contrasting with SURFACE which holds a ctx.
- **Italic/variable axes out of scope**: the font stack is (family, weight) only; multi-weight families compose by registering each file under a shared name.

## GRID widget — macOS native port

- **Painted custom view, not NSTableView**: chose to keep the GRID a painted `NEUINativePaintedView` matching the xpl + win32 hosts rather than wrapping NSTableView — avoids cell-template/data-source plumbing, keeps the layerable-cells path open, and lets all the heavy logic stay in the shared `grid_model.h` / `widget_paint_grid.h` / `scrollbar.h` headers.
- **win32 native as the porting template** (not xpl): win32 is also a painted-widget host with its own session + widget hierarchy, so its `painted_msg_grid_w32` dispatch + `grid_api` table translate almost mechanically; the port is a near-verbatim translation with only coordinate/modifier/repaint idioms swapped.
- **Lazy-allocated `GridModel` per widget**: a `unique_ptr` field so every non-GRID widget pays only a single pointer.
- **Cursor via direct `[NSCursor set]` from `mouseMoved:`, not cursor-rects**: divider hit zones depend on live scroll_x + column widths, which `addCursorRect:` can't track accurately — direct sets match the win32 path.
- **`paint_grid` owns the whole surface**: it issues an unconditional `fill_rect(body_bg)`, so the GRID branch must not do the section-style transparent clear.

## GRID column sorting

- **Indirection layer, not in-place reorder**: `rows` stays in insertion order; a `display_order` (visual→logical) + `logical_to_visual` inverse hold the sort. This preserves the load-bearing contract that every public row index (`set_cell_text`, `cell_overrides` keys, `selected_row`, event payloads, `hit_test`) is a *stable logical* index — no client migration needed, and "unsorted" is just empty vectors falling through to the existing direct path.
- **Events always carry logical row**: clicking the topmost sorted row reports its logical record index, matching the data API; a client wanting on-screen position calls `logical_to_visual_row`. Keeps the data path translation-free.
- **`std::stable_sort` is load-bearing**: equal keys keep insertion order, making results deterministic and making the multi-column tie-break simply "insertion order at the deepest tie".
- **Lazy re-sort via `sort_dirty`**: mutations only set a flag; `paint_grid` / `hit_test` / translation getters rebuild before reading — avoids re-sorting on every cell mutation.
- **Three-state cycle + multi-column with FIFO soft cap (8)**: plain click replaces the stack, Shift+click pushes/cycles a level; a 9th Shift+click evicts the oldest (level 0) to keep the most recent user intent.
- **Per-column sortable (default true) + explicit sort kind (default STRING), no auto-detect**: numeric columns must be opted into INT/FLOAT to avoid the "9 < 10" string trap; non-sortable columns ignore header clicks but still honor programmatic `set_sort`.
- **Column-structure mutations clear sort**: `clear_rows` / `clear_columns` / `remove_column` wipe sort state because column indices would shift — safer than remapping.

## Clipboard expansion + drag & drop

- **Unified `neui_data_item_t` for clipboard and DnD**: both move MIME-typed data across the app boundary and share storage + lifetime rules, so one `DataItem` (mime→bytes) / `DataItemStore` primitive backs both; convenience `set_text`/`get_text` still bypass the item path for the hot Ctrl+C/X/V case.
- **No push-style clipboard onchange callback**: the old `NEUI_API_CLIPBOARD_CLIENT` was removed; clients poll `has_text`/`item_has_format` on demand (e.g. in a menu-popup handler) since the OS has no reliable change-notification anyway.
- **Synchronous `begin_drag` and `accept`**: `begin_drag` blocks until the OS drag loop ends (matching Win32's `DoDragDrop` contract); macOS replicates this with a self-contained runloop pump in the shared header rather than the xpl `platform_run_modal_until` seam (the native macOS host doesn't implement it). `accept(action)` is the synchronous "what I'll take" call cached in `_last_accepted_action`.
- **Frame-level drop-target registration only**: child HWNDs / child painted views don't register their own targets — the OS routes to the frame (Win32 walks the parent chain; macOS keeps `NEUINativeContentView` as the single `<NSDraggingDestination>`) and the framework hit-tests the widget tree in software, giving per-widget targeting with one registration. (Hard constraint: never mix `WS_EX_ACCEPTFILES` with `RegisterDragDrop`.)
- **Drop data item is dispatch-scoped**: the `DataItem` is live only during the DROP callback and released the instant it returns, so clients must copy bytes via `item_get_format` during dispatch.
- **`NSDragOperation` ≠ `DROPEFFECT_*` numerically**: requires explicit `nsop_to_dnd_action` mapping (whereas `neui_dnd_action_t` deliberately matches the Win32 `DROPEFFECT_*` values).
- **Per-host DnD hit-test walkers left un-unified by design** (a documented dedup non-goal): the xpl host walks descendants through the virtual `hit_test` with cached `abs_x/abs_y`, while native hosts accumulate parent-relative coords on the fly — different contracts that shouldn't be forced together.

## Drag-source (and code-review hardening)

- **Pump with a short timeout + watchdog, never `distantFuture`**: AppKit teardown paths that skip the ended-delegate (window destroyed mid-drag, pasteboard-writer exception, mode mismatch) would otherwise freeze the app; a 50 ms timeout in `NSEventTrackingRunLoopMode` (AppKit drives drags in tracking mode, not default) plus a "session never began" watchdog guarantees unwinding.
- **Explicit modifier-intent rejection over silent substitution**: when the user holds Ctrl but COPY isn't available, return NONE (no-drop cursor) rather than falling through to MOVE — an honest cursor prevents a confirmed drop from silently deleting source content under a misread intent.
- **Shared modifier convention in one header**: `dnd_suggest_action` (Ctrl+Shift=Link, Ctrl=Copy, Shift=Move, none=Copy>Move>Link) lives in `dnd_modifier_suggest.h` so Win32 and both macOS paths produce identical `suggested_action`, eliminating the "modifier cursor on Windows, constant on macOS" divergence.
- **Self-fallback added at the `begin_drag` shim, not in the parent walkers**: `find_parent_hwnd`/`find_parent_native_handle` keep their parent-only semantics (other callers depend on it); the frame-as-source case is handled by falling back to the widget's own native handle at the call site.
- **Two-tier re-entry guard**: `_in_dnd_dispatch` only catches `begin_drag` from inside a DnD callback; a separate `_drag_source_active` flag additionally blocks a recursive drag started from a non-DnD event (timer/animation tick) firing in an idle gap during an active drag.
- **Composite-source format model on both platforms**: macOS stamps the shared text/HTML/MIME payload onto *every* per-URL `NSPasteboardItem` so each item carries full context, mirroring Win32's single `IDataObject`; Win32 de-dups duplicate `CF_UNICODETEXT` (first-wins) and falls back to `CF_UNICODETEXT` for non-file URIs that can't become a `CF_HDROP`.

## Scrolling SECTION + smooth kinetics

- **Reused the existing parent/child container relationship on every host** (xpl paint-walk translate, Win32 HWND parenting, macOS NSView subviews) rather than building a new container abstraction — the scroll feature is just offset + clip + scrollbar layered onto what already existed.
- **Extracted GRID's rubber-band/bounce math into a generic `scroll_kinetics.h` primitive** (position-px in, position-px out) so SECTION and GRID share one kinetics implementation; GRID keeps its row-index decomposition as a thin shim, so smooth-scroll feel is identical across both widget kinds with no behavior change to GRID.
- **Chose a generic `NEUI_ATTR_SCROLL_KINETICS` over renaming the GRID kinetics key**: renaming would break clients setting the literal GRID key; the new SECTION+GRID key adds capability while `grid.scroll_mode` stays a GRID-only back-compat alias.
- **Children store body-local (x,y); the host subtracts the section's scroll at native-positioning time** (`SetWindowPos`/`setFrame:`), keeping the stored client coords stable rather than mutating them on every scroll.
- **Adopted the inner-body container idiom** (Win32 `body_hwnd` / macOS `NEUISectionBodyView`) after the first cut's per-child clipping proved too slow at 60 Hz (200+ children) and let children overpaint the chip/gutter; native default subview clipping confines children for free. The body container is created lazily on first scroll and **kept for the section's lifetime even after flipping back to `"none"`** — destroying it dropped children from body-local to section-local coords, jumping them into the chip band.
- **macOS body view is a transparent, layer-clipped structural container that does NOT paint its own background** — an opaque `NSRectFill` broke sibling non-layer-backed painted-view rendering; the section's own paint fills the body bg underneath.
- **No new public events in v1** (scroll position kept as internal state); `SCROLL_CHANGED` + the scroll API shipped as a deliberate later follow-up, with `last_notified_x/y` gating so no-op ticks stay silent.

## Linux port (Cairo/X11)

- **Reused the crossplatform (xpl) host unchanged**, adding only two TUs (Cairo backend + `platform_linux.cpp`) — no new host, no new Session entry points; Linux keeps `NEUI_HAS_XPLHOST`.
- **Chose Cairo software-surface rendering (blitted via XShm) + FreeType/Fontconfig over GPU options** (NanoVG/OpenGL, GLX): maps 1:1 onto neui's existing immediate-mode 2D backend, minimizes the dependency tree, and avoids GPU/driver variance — the most robust choice inside DAWs and over remote X.
- **Deliberately minimal dependency set** (Cairo + FreeType + Fontconfig + Xlib/Xext + vendored stb_image): rejected Pango, HarfBuzz (shaping skippable for Latin), GTK, Qt, and any plugin SDK.
- **Embedding designed around the event-loop ownership inversion**: a Linux plugin cannot own a blocking loop and Xlib isn't thread-safe to share, so each instance opens its **own `Display`** connection, creates its window as a child of the host XID, and exposes `event_fd` + `pump_and_tick` seams for the DAW's run loop to drive — neui owns no loop when embedded.
- **Create-as-child (`XCreateWindow(parent=xid)`) over `XReparentWindow`** to avoid the WM-intercept race during reparenting.
- **64-bit XID doesn't fit the int/string AttrBag cleanly, so embedding uses dedicated seams, not attrs.**
- **Cairo's native top-left/Y-down convention let CG's Y-flip logic be dropped**, and the Cairo backend owns its image surface for the window's lifetime (vs CG rebinding per `drawRect:`), so no `set_current_frame` seam is needed.
- **Plugin-format adapters (VST3/CLAP/LV2) kept out of scope** — only the neui-side embedding primitives are built.

## Linux in-UI menubar + XInput2 smooth scroll

- **Menubar drawn in-frame (inside the frame's render ctx) rather than as a child overlay** — matches how the toast and message box are already done; hit-test and paint live in the frame's existing passes, and the band reserves a top inset that paint/hit-test/`ensure_visible` all account for consistently.
- **Menubar uses the combo-overlay interaction model (non-blocking, hover-to-switch + cascade) instead of the blocking `popup_menu` pump** — a menubar must switch top-level menus and open submenus on hover, which the single-shot blocking popup pump can't express; it still reuses the popup's layout constants and row styling.
- **Menu geometry recomputed on demand from `(_menu_open, _menu_path)` using the frame's own render ctx, not cached rects** — keeps it resize- and multi-frame-safe.
- **XInput2 / libXi is an optional, auto-detected build dependency** (`NEUI_HAS_XI2`): present → pixel-precise kinetic scroll; absent → the whole block compiles out and only classic core-Button 4-7 stepped scroll is built, so libxi-dev is never a hard requirement.
- **Classic core-button scroll stays active and is suppressed only after the first real XI2 scroll arrives** (`g_xi2_scroll_seen`) — servers/XWayland without scroll valuators degrade cleanly to stepped scroll and never double-count. The same flag flips the PLATFORM kinetics default to SMOOTH on Linux, so the default is data-driven by actual device capability rather than hardcoded per-OS.
- **Reused the existing host-neutral kinetics math** (`scroll_kinetics.h` / `grid_model.h` / `widget_section_scroll.h`) and the existing `dispatch_wheel_event` ancestor-routing — the Linux job was only feeding pixel-precise deltas and extending the existing 16 ms timerfd heartbeat to step active grid/section bounces.

## Crossplatform host (early sketch)

*This was an early design sketch, superseded by the shipped xpl host — it captured initial intent, not the final implementation. Retained here only for the founding premises.*

- **Core premise: one host that delivers the same look and behavior across platforms** — registers like the win32 host in the `neui_api_t` registry, with an identical control hierarchy on every platform.
- **OS-specific rendering backends behind a common C interface, selected via CMake** — Direct2D on Windows first, other platforms deferred; backends placed alongside host implementations so other hosts can reuse them.
- **Split native vs. shared: only APPWINDOW/PLUGWINDOW are native controls** (platform-specific files chosen by CMake); all other controls share one base class.

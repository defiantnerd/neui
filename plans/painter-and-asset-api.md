# Plan: `neui_painter_t` + `neui_asset_api_t`

Follow-up to the `NEUI_W_CUSTOMDRAW` widget. Closes two design gaps in
the current shape:

1. The `WIDGET_PAINT` event hands clients the full `neui_render_backend_t`,
   which mixes drawing primitives with lifecycle functions
   (`create_context`, `destroy_context`, `begin_frame`, `end_frame`,
   `resize`, `update_dpi`). A client calling any of those mid-paint
   either leaks resources or wipes the surface. Documentation alone
   doesn't close this.
2. Bitmap creation lives inside `neui_render_backend_t` and requires a
   `neui_render_ctx_t`, so clients can't preload bitmaps at startup -
   they have to defer to the first paint, cache the handle, and re-cache
   on context recreation. That's the worst paint-time footgun in the
   current API (GPU upload per frame if the client doesn't cache).

The renderer-internals (`neui_render_backend_t`) stay; only the
client-facing surface changes.

## Naming

`neui_asset_api_t`, not `neui_bitmap_api_t`. The same API will later
serve SVGs, vector graphics, fonts, and possibly audio/sample assets.
Naming it for the role from day one avoids a rename later.

## Part A - `neui_painter_t` (curated paint interface)

### New file: `include/neui/d/painter.h`

```c
typedef struct neui_painter neui_painter_t;  // opaque handle

typedef struct neui_painter_api {
  uint32_t neui_version;

  // --- Read-only queries (cheap) ---
  float (NEUI_ABI *get_scale_factor)(neui_painter_t* p);
  float (NEUI_ABI *measure_text)    (neui_painter_t* p, const char* utf8,
                                      int text_len, float font_size);

  // --- Shapes ---
  void (NEUI_ABI *fill_rect)(neui_painter_t* p, float x, float y,
                              float w, float h, uint32_t argb);
  void (NEUI_ABI *draw_rect)(neui_painter_t* p, float x, float y,
                              float w, float h, float stroke_w, uint32_t argb);
  void (NEUI_ABI *draw_text)(neui_painter_t* p, float x, float y,
                              float w, float h, const char* utf8,
                              float font_size, uint32_t argb);

  // --- Path API ---
  void (NEUI_ABI *begin_path)(neui_painter_t* p);
  void (NEUI_ABI *move_to)   (neui_painter_t* p, float x, float y);
  void (NEUI_ABI *line_to)   (neui_painter_t* p, float x, float y);
  void (NEUI_ABI *arc)       (neui_painter_t* p, float cx, float cy,
                               float r, float a0, float a1, bool ccw);
  void (NEUI_ABI *close_path)(neui_painter_t* p);
  void (NEUI_ABI *fill_path) (neui_painter_t* p, uint32_t argb);
  void (NEUI_ABI *stroke_path)(neui_painter_t* p, float stroke_w, uint32_t argb);

  // --- State stack ---
  void (NEUI_ABI *push_clip)     (neui_painter_t* p, float x, float y,
                                   float w, float h);
  void (NEUI_ABI *pop_clip)      (neui_painter_t* p);
  void (NEUI_ABI *push_transform)(neui_painter_t* p);
  void (NEUI_ABI *pop_transform) (neui_painter_t* p);
  void (NEUI_ABI *translate)     (neui_painter_t* p, float dx, float dy);
  void (NEUI_ABI *rotate)        (neui_painter_t* p, float radians);
  void (NEUI_ABI *scale)         (neui_painter_t* p, float sx, float sy);

  // --- Assets (handle-based; resolution happens inside the host) ---
  void (NEUI_ABI *draw_asset)(neui_painter_t* p, neui_asset_t asset,
                               float x, float y, float w, float h);
} neui_painter_api_t;
```

### Excluded from the curated surface (vs. `neui_render_backend_t`)

- `create_context` / `destroy_context` / `resize` / `update_dpi` -
  window/context lifecycle, never client business.
- `begin_frame` / `end_frame` - frame lifecycle.
- `create_bitmap` / `destroy_bitmap` - moved to the asset API.
- The old `draw_bitmap(ctx, void*, ...)` - replaced by handle-based
  `draw_asset(painter, neui_asset_t, ...)`.

### `WIDGET_PAINT` event payload

```c
typedef struct neui_event_paint {
  neui_widget_t       widget;
  neui_painter_api_t* painter;   // curated drawing API (was: render_backend)
  neui_painter_t*     p;         // opaque handle bound to ctx + backend
  float               width;
  float               height;
  bool                focused;
} neui_event_paint_t;
```

Same shape as today, just `(painter, p)` instead of `(backend, ctx)`.

### Internal implementation

A single static `neui_painter_api_t` table in `hosts/shared/painter.h`.
Each entry forwards to the active `neui_render_backend_t` plus the
current `neui_render_ctx_t`. The opaque `neui_painter_t*` carries
`(backend, ctx)` and any per-paint scratch state (e.g. asset resolution
cache scoped to this paint call - see Part B).

Internally the painter struct is small enough to be stack-allocated by
the host inside the WIDGET_PAINT dispatch site. ~50 LOC of forwarding
glue, ODR-safe via `inline static` like the rest of `hosts/shared/`.

Per-call overhead vs. today: one extra struct pointer dereference per
draw call. Negligible.

## Part B - `neui_asset_api_t` (session-scoped asset cache)

### New file: `include/neui/d/assets.h`

```c
#define NEUI_API_ASSETS "com.defiantnerd.neui.extension.assets/0"

typedef struct neui_asset { uint32_t id; } neui_asset_t;
static const neui_asset_t asset_none = { UINT32_MAX };

// Future-proofing: callers can ask what kind of media a handle represents.
typedef enum neui_asset_kind {
  NEUI_ASSET_KIND_NONE   = 0,
  NEUI_ASSET_KIND_BITMAP = 1,
  // Reserved (not implemented yet):
  // NEUI_ASSET_KIND_SVG    = 2,
  // NEUI_ASSET_KIND_VECTOR = 3,
  // NEUI_ASSET_KIND_FONT   = 4,
} neui_asset_kind_t;

typedef struct neui_asset_api {
  uint32_t neui_version;

  // Load a bitmap from raw BGRA8 (premultiplied) pixel data. Logical
  // size is width_px / scale, height_px / scale (so an @2x bitmap
  // declared with scale=2 draws at half its pixel size).
  neui_asset_t (NEUI_ABI *create_bitmap)(neui_session_t session,
                                           uint32_t width_px,
                                           uint32_t height_px,
                                           const uint8_t* bgra_premul,
                                           float scale);

  // Load a bitmap from a path. PNG / JPG / BMP via the existing
  // platform loader (icon_win32.h / image_loader_macos.h). The host
  // also looks the name up as an embedded resource on win32 (matching
  // current AssetManager behavior).
  neui_asset_t (NEUI_ABI *create_from_file)(neui_session_t session,
                                              const char* path_utf8);

  // Release the CPU-side pixels and drop all cached GPU uploads.
  // Safe to call at any time; pending draws of this handle resolve
  // to a no-op.
  void (NEUI_ABI *destroy)(neui_session_t session, neui_asset_t asset);

  // Logical-pixel size of the asset (i.e. pixel size / declared scale).
  // Returns false if the handle is invalid or the asset has no
  // intrinsic size (e.g. future SVG without a viewBox).
  bool (NEUI_ABI *get_size)(neui_session_t session, neui_asset_t asset,
                             float* out_logical_w, float* out_logical_h);

  // Kind discriminator. Returns NEUI_ASSET_KIND_NONE for invalid
  // handles. Lets future code do `if (kind == NEUI_ASSET_KIND_SVG)`
  // without breaking ABI.
  neui_asset_kind_t (NEUI_ABI *get_kind)(neui_session_t session,
                                           neui_asset_t asset);
} neui_asset_api_t;
```

### Typical client usage

```c
neui_asset_api_t* assets = get_interface(sess, NEUI_API_ASSETS);

// At init time:
neui_asset_t meter_bg = assets->create_from_file(sess, "meter.png");
neui_asset_t logo     = assets->create_bitmap   (sess, 64, 64, my_bgra, 1.0f);

// Inside WIDGET_PAINT:
painter->draw_asset(p, meter_bg, 0, 0, width, height);
painter->draw_asset(p, logo, 8, 8, 32, 32);

// At shutdown:
assets->destroy(sess, meter_bg);
assets->destroy(sess, logo);
```

### Internal storage: extend `AssetManager`

`hosts/crossplatform/asset_manager.{h,cpp}` already implements exactly
this pattern, just keyed on file path. We extend it:

- Add a non-path-keyed entry table: handle slot -> `AssetEntry`.
- `AssetEntry` gains a `kind` field (`bitmap` today, others later).
- Existing `get_bitmap(path, ctx, scale)` lookups stay; they share
  the per-ctx GPU cache with handle-based lookups.
- Sessions own one `AssetManager` already (`Session::_asset_manager`
  in `hosts/crossplatform/host.h:587` - reuse).
- Handle layout: `session_id << 16 | slot`, matching `neui_widget_t`.

The native macOS host (`hosts/macos/`) doesn't have an `AssetManager`
yet - it loads bitmaps lazily in `drawRect:` via `ensureImageBitmap`.
For phase 1 we add a minimal session-scoped manager that just wraps
`CGImageRef` (device-independent, so no per-ctx cache needed - the
agent's report confirms this collapses to a single entry per asset).
The native win32 host's per-IMAGE D2D context stays; the bitmap source
becomes the shared manager via a thin lookup.

### `painter->draw_asset` resolution path

1. Painter has `(backend, ctx, session*)`.
2. `draw_asset(p, asset, x, y, w, h)` calls
   `session->asset_manager.draw(asset, backend, ctx, x, y, w, h)`.
3. Manager looks up the entry by handle, finds (or creates) the per-ctx
   GPU upload, calls `backend->draw_bitmap` internally.
4. For future SVG: manager rasterizes to a per-(ctx, target-size) cache,
   then `backend->draw_bitmap`. Or emits paths via the path API if we
   wire that.

The client never sees `void* bitmap` - the renderer struct's
`draw_bitmap(ctx, void*, ...)` stays for internal use, the public
surface is handle-only.

## Files affected

| File | Change |
|---|---|
| `include/neui/d/painter.h` (NEW) | `neui_painter_t`, `neui_painter_api_t` |
| `include/neui/d/assets.h` (NEW) | `neui_asset_t`, kinds enum, `neui_asset_api_t` |
| `include/neui/neui.h` | Include the two new headers |
| `include/neui/d/events.h` | `neui_event_paint_t` carries `painter_api*` + `painter*` instead of `backend*` + `ctx`. |
| `include/neui/d/renderer.h` | Removes `create_bitmap` / `destroy_bitmap` from `neui_render_backend_t` (or keeps them internal-only with a `// internal` comment block). `draw_bitmap` stays - internal call from the painter forwarder. |
| `hosts/shared/painter.h` (NEW) | The static `neui_painter_api_t` table; the `neui_painter` struct definition; inline forwarders. |
| `hosts/crossplatform/asset_manager.{h,cpp}` | Handle table; `kind` field; `create_from_bgra`, `create_from_file`, `destroy`, `get_size`, `get_kind`. |
| `hosts/crossplatform/host.{h,cpp}` | `get_interface(NEUI_API_ASSETS)` plumbing; CustomDrawWidget::paint constructs a `neui_painter` and dispatches with the new payload. |
| `hosts/crossplatform/widgets.cpp` | Asset API vtable. |
| `hosts/win32/host.{h,cpp}` + `widgets.cpp` | Session gets an `AssetManager`; `paint_customdraw_w32` constructs a `neui_painter` and dispatches with the new payload; `get_interface(NEUI_API_ASSETS)` plumbing. Native IMAGE widget loading switches to the manager. |
| `hosts/macos/widgets.mm` + `window.mm` | Minimal session-scoped asset manager wrapping `CGImageRef`; `get_interface(NEUI_API_ASSETS)` plumbing. `NEUI_W_CUSTOMDRAW` on native macOS stays unimplemented per the prior decision; macOS clients targeting CUSTOMDRAW use the xpl host. |
| `examples/main.cpp` | CUSTOMDRAW demo updated to use `painter->draw_asset` (drop a pre-loaded PNG into the meter background as a smoke test). |
| `CLAUDE.md` | Document `neui_painter_t`, `neui_asset_api_t`, the new event payload shape, and the asset-kind enum. |

## Deferred (revisit later)

These came up during the analysis and are real, but out of scope for
this revision.

1. **Stack-depth checking on `push_clip` / `push_transform`.** The
   framework's bracketed push+pop around the WIDGET_PAINT dispatch
   catches the "forgot one pop" case. It does not catch a client that
   pushes more than it pops. Adding `get_clip_depth(p)` /
   `get_transform_depth(p)` to the painter would let the framework
   forcibly rebalance after the client returns. Cost: two extra
   functions, depth tracking on `D2DContext` / `CGContext`. Punt until
   we hit a real debug session that wants it.
2. ~~**D2D device-loss recovery.**~~ Done. The D2D backend stashes
   `hwnd` / `width` / `height` / `dpi` / `generation` on `D2DContext`,
   so `d2d_end_frame` can release the lost target+brush and the next
   `d2d_begin_frame` rebuilds in place (same ctx pointer, bumped
   generation). `neui_render_backend_t` gained
   `get_context_generation(ctx)`; both asset managers (xpl + win32)
   key their per-ctx GPU cache by `(ctx, generation)` and re-upload
   when the values disagree. The native win32 `IMAGE` widget keeps a
   CPU pixel copy on `WidgetData` for the same reason. Backends
   without device-loss (CG, null) return a constant. See
   `backends/d2d/d2d_backend.cpp`, `hosts/crossplatform/asset_manager.*`,
   `hosts/win32/asset_manager_w32.h`, `hosts/win32/window.cpp`.
3. **`NEUI_W_CUSTOMDRAW` on the native macOS host.** Needs a
   `NEUIDrawView` NSView subclass that brackets CG `begin_frame` /
   `end_frame` around a WIDGET_PAINT dispatch. Currently macOS clients
   use the xpl host for CUSTOMDRAW (already works via the CG backend).
4. ~~**`NEUI_W_IMAGE` migration to the public asset API.**~~ Done.
   `neui_widget_api_t` gained `set_asset(widget, neui_asset_t)`; on
   both real hosts the IMAGE widget accepts either a file path
   (`set_text`) or a pre-loaded asset handle (`set_asset`),
   last-set-wins with mutual clearing. On the native win32 host the
   path source now allocates an internal slot via the session's
   `W32AssetManager` rather than carrying its own CPU/GPU cache, and
   IMAGE runs on the shared `"neui.painted"` seam (`paint_image_w32`
   in `hosts/win32/widgets.cpp` - aspect-fit + rotation + lazy GPU
   upload with device-loss generation tracking). `image_asset_owned`
   on `WidgetData` distinguishes widget-owned-from-text from
   client-supplied handles so destroy / re-set release the slot only
   when the widget owns it. The macOS native host still ships a no-op
   `w_set_asset` stub until the `NEUI_W_IMAGE` widget itself lands
   there (see `plans/macos-port.md`). See `include/neui/d/widgets.h`,
   `hosts/win32/{widgets.cpp,window.cpp,window.h,host.h}`,
   `hosts/crossplatform/{widgets.cpp,host.cpp,host.h}`.
5. **Asset-kind expansion.** SVG, vector, font, etc. - the enum
   reserves slots, but each kind is its own implementation story
   (SVG parser, per-resolution raster cache, etc.). Land one at a time
   as needed.

## Verification

End-to-end test plan (manual, matching the existing project style -
no formal test suite):

1. Build clean on Windows and macOS.
2. Update the example's CUSTOMDRAW demo to call
   `assets->create_from_file` at init, draw the bitmap as a background
   inside WIDGET_PAINT, and `assets->destroy` at shutdown. Verify:
   - Knob drag still updates the meter overlay (existing behaviour
     unchanged).
   - The background bitmap renders without flicker (proves the per-ctx
     GPU cache works).
   - Resizing the window re-uploads cleanly (proves the cache survives
     context swap or is invalidated correctly).
3. Verify the painter's curated surface compiles when a client tries
   to call a removed function (`begin_frame`, etc.) - expected:
   compile error. This is the whole point of the curated struct.
4. xpl host: same checks. Specifically confirm that the painter's
   `push_clip(widget bounds)` still works (it's pushed by the
   framework before dispatch, popped after).
5. macOS xpl host: same checks. `CGImageRef` device-independence means
   the per-ctx cache is trivial here - mostly a smoke test that the
   API wires up.
6. Multi-window: open the existing About dialog while the main window
   has a CUSTOMDRAW with a loaded asset. The asset stays valid across
   the second window (session-scoped, not frame-scoped).

## Scope estimate

~400-500 LOC across the listed files. Bulk is in
`hosts/shared/painter.h` (the forwarding table) and the macOS asset
manager (new). Everything else is plumbing - the xpl `AssetManager`
already does the load-and-cache work.

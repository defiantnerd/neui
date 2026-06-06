# Plan: render-to-surface asset

Follow-up to `neui_asset_api_t`. Adds a fourth asset kind -
`NEUI_ASSET_KIND_SURFACE` - that lets clients render arbitrary content
into a pixel buffer **using the same `neui_painter_api_t` interface
they already know from `WIDGET_PAINT`**, then attach the result as a
regular asset (drawn via `painter_api->draw_asset`, usable inside a
compound layer, etc.).

Primary use cases the user called out: visual diagnostic outputs that
get packed into the asset pipeline (e.g. a real-time spectrum / scope
that updates from a worker thread). General use: thumbnails, cached
ornaments that take >1 frame to compose, baked compound previews.

## Re-use angle

Both asset managers already comment that `NEUI_ASSET_KIND_SURFACE`
would land "one branch the day that kind lands" - the backing-pixels
field layout is the same as `NEUI_ASSET_KIND_BITMAP`. The whole point
of this design is that once a surface has been painted, **it is
indistinguishable from a bitmap** for every downstream consumer:

- The painter's `draw_asset` thunk in each host already resolves a
  handle, walks the per-ctx GPU upload cache, and uploads on miss.
  SURFACE plugs into that path unchanged.
- `AssetEntry::pixels` (BGRA8 premul) is reused as the surface's CPU
  backing buffer.
- `get_pixels_for_export` (drag preview / future file save) gains a
  one-line branch.

What's actually new:

1. A backend can produce an **off-screen render context** that draws
   into a pixel buffer instead of an HWND / NSView.
2. The host can hand the client a `neui_painter_t*` aimed at that
   off-screen context for the duration of a callback.

Everything else is wiring.

## Part A - Public API

### Append to `include/neui/d/assets.h`

```c
// Discriminator: bind to the next reserved slot.
NEUI_ASSET_KIND_SURFACE = 4,

// Client paint callback for paint_surface. Mirrors the WIDGET_PAINT
// payload: a painter + the curated painter API + the surface size in
// logical pixels + an opaque user pointer.
typedef void (NEUI_ABI *neui_surface_paint_fn)(
    neui_painter_t*     painter,
    neui_painter_api_t* api,
    float               width_logical,
    float               height_logical,
    void*               user);

// Vtable entries appended AFTER create_behavior (ABI stability):

neui_asset_t (NEUI_ABI *create_surface)(neui_session_t session,
                                          float width_logical,
                                          float height_logical,
                                          float scale);

// Drive the client callback against the surface's off-screen ctx:
//   1. begin_frame(clear_argb), then push_clip([0,0,w,h]).
//   2. fn(painter, api, w_log, h_log, user).
//   3. pop_clip, end_frame.
//   4. Read pixels back into the asset's CPU buffer.
//   5. Drop any cached per-(window-ctx, generation) bitmap uploads
//      of this asset so the next draw_asset re-uploads.
// Safe to call any time except inside a WIDGET_PAINT callback. Safe
// to call repeatedly to re-render. No-op for non-SURFACE handles.
void (NEUI_ABI *paint_surface)(neui_session_t        session,
                                neui_asset_t          surface,
                                uint32_t              clear_argb,
                                neui_surface_paint_fn fn,
                                void*                 user);
```

`neui_painter_api_t*` is forward-declared in `assets.h` since it lives
in `painter.h`; both headers are already in the same `d/` directory and
clients pulling in `neui.h` get both.

`get_kind` returns `NEUI_ASSET_KIND_SURFACE` for handles allocated by
`create_surface`. `get_size` returns the logical dimensions (mirrors
`BITMAP`). `destroy` releases CPU pixels + the off-screen ctx + any
cached uploads, same as `BITMAP`.

### Why callback, not begin/end pair

Lifecycle is bulletproof - the framework owns `begin_frame` /
`end_frame` / clip / read-back / cache invalidation. A
`begin_paint_surface` / `end_paint_surface` pair would let clients leak
half-drawn surfaces or recurse into themselves. The callback shape
also matches `WIDGET_PAINT` mentally, which is the surface clients
already know.

## Part B - Backend vtable

### Append to `neui_render_backend_t` in `include/neui/d/renderer.h`

```c
// Create an off-screen render context that draws into a CPU pixel
// buffer instead of a window. Used by the asset manager to back
// NEUI_ASSET_KIND_SURFACE.
//   width_px / height_px - physical pixel dimensions of the surface.
//   scale                - HiDPI factor (1.0 / 2.0 / 3.0); the
//                          backend sets the off-screen target's DPI
//                          so logical-pixel draws map to the right
//                          pixel count.
// Returns nullptr if the backend doesn't support off-screen targets
// (null backend) or allocation failed. The returned ctx is destroyed
// via destroy_context (same path as window contexts).
neui_render_ctx_t (NEUI_ABI *create_offscreen_context)(
    uint32_t width_px,
    uint32_t height_px,
    float    scale);

// Read back the surface pixels of an off-screen ctx as BGRA8
// premultiplied, top-down (width_px rows of width_px * 4 bytes).
// out_bgra must be width_px * height_px * 4 bytes. Returns false on
// window contexts, null contexts, or read failure. Call after
// end_frame.
bool (NEUI_ABI *read_pixels_bgra)(neui_render_ctx_t ctx,
                                    uint8_t*         out_bgra);
```

`begin_frame` / `end_frame` / `resize` / `update_dpi` work on
off-screen contexts unchanged. `destroy_context` already cleans up; it
just needs to handle the off-screen path internally.

### D2D (`backends/d2d/d2d_backend.cpp`)

- Initialise an `IWICImagingFactory` alongside the existing factories.
- `create_offscreen_context`: create an `IWICBitmap`
  (`GUID_WICPixelFormat32bppPBGRA`, `WICBitmapCacheOnDemand`), then
  `ID2D1Factory::CreateWicBitmapRenderTarget` with DPI `= scale * 96`.
  Store flag + IWICBitmap + render target on the `D2DContext`.
- `read_pixels_bgra`: `IWICBitmap::Lock(WICBitmapLockRead)` + memcpy
  per row (already top-down BGRA premul). Handle tightly-packed vs
  WIC's natural pitch.
- `destroy_context`: release WIC bitmap if present.

The existing path API + transform / clip / alpha / font stacks all
operate on `ID2D1RenderTarget*`, which is what `WicBitmapRenderTarget`
returns. Off-screen and HWND targets are interchangeable for every
draw call - zero duplication.

### CG (`backends/cg/cg_backend.mm`)

- `create_offscreen_context`: allocate a `width_px * height_px * 4`
  buffer; `CGBitmapContextCreate` with `kCGImageAlphaPremultipliedFirst
  | kCGBitmapByteOrder32Little` (BGRA premul on little-endian) over
  that buffer. The macOS code uses `isFlipped=YES` views so the CTM
  matches Y-down; for an off-screen ctx we apply the same flip via
  `CGContextTranslateCTM(0, h); CGContextScaleCTM(1, -1)` once at
  create-time.
- Store the CGContextRef + owned buffer pointer on `CGContextState`.
  The existing `set_current_frame` path is only used for window
  contexts; the off-screen ctx is bound at create-time and stays bound
  for its lifetime.
- `read_pixels_bgra`: memcpy from the owned buffer. (Or skip the copy
  and have read_pixels_bgra return a pointer + length - but that
  complicates lifetime; copy is cheap.)
- `destroy_context`: `CGContextRelease` + `free(buffer)`.

### Null (`backends/null/null_backend.cpp`)

- `create_offscreen_context` returns `nullptr`; the asset manager
  treats that as "surface unsupported" and `create_surface` returns
  `asset_none`. Matches how null handles every other capability.
- `read_pixels_bgra` returns `false`.

## Part C - Asset manager extension

Three asset managers, identical shape:

### Add to `AssetEntry` / `W32AssetEntry` / `MacOSAssetEntry`

```cpp
// Populated for NEUI_ASSET_KIND_SURFACE entries; null otherwise.
// Owns the off-screen render ctx for the surface's lifetime; pixels[]
// holds the most recently rendered frame.
neui_render_ctx_t surface_ctx = nullptr;
```

### Add `allocate_surface`

```cpp
uint32_t allocate_surface(uint32_t width_px, uint32_t height_px,
                           float scale,
                           neui_render_backend_t* backend);
```

Calls `backend->create_offscreen_context`. On failure returns 0 (so
the public `create_surface` returns `asset_none`). Reserves a
`width_px * height_px * 4` pixel buffer (zero-filled until the first
paint).

### Add `paint_surface` coordinator

```cpp
void paint_surface(uint32_t slot,
                   uint32_t clear_argb,
                   neui_surface_paint_fn fn,
                   void* user,
                   neui_render_backend_t* backend,
                   /* host-specific painter setup */);
```

- `backend->begin_frame(entry->surface_ctx, clear_argb)`.
- `backend->push_clip(ctx, 0, 0, w_log, h_log)`.
- Construct a `neui_painter_t` on the stack with
  `{ backend, ctx, host_token, draw_asset_thunk }` - the host's
  existing thunk works unchanged (it walks the same asset manager).
- Invoke `fn(&painter, &k_painter_api, w_log, h_log, user)`.
- `backend->pop_clip`, `backend->end_frame`.
- `backend->read_pixels_bgra(ctx, entry->pixels.data())`.
- For each cached `(other_ctx, generation)` GPU upload of this entry,
  `backend->destroy_bitmap` and erase. Next `draw_asset` rebuilds
  them from the new pixels.

### `release_slot` extension

If `entry->surface_ctx` is set, `backend->destroy_context(ctx)` before
dropping the entry. Mirrors how `compound` / `behavior` reset their
unique_ptr.

### `release_context` semantics

Releasing the on-screen window context of a frame must NOT touch any
surface's own off-screen ctx - it must only clean up that surface's
cached GPU upload of the dying window ctx. The existing iteration
over `entry->bitmaps` already does the right thing; surface_ctx lives
on a different field.

### `get_pixels_for_export` extension

Add the second case the existing comment promises:

```cpp
case NEUI_ASSET_KIND_SURFACE:
  // Identical body to BITMAP.
```

## Part D - Wiring `neui_asset_api_t`

Each host's `neui_asset_api_t` table grows two entries. The bodies are
small thunks into the asset manager + the painter construction
described above. The painter's host_token + draw_asset_thunk are the
exact pair each host already uses for `WIDGET_PAINT` dispatch, so
nested `draw_asset` inside the surface paint callback works
automatically (you can draw bitmaps into a surface; rendering one
surface from inside another's paint is fine as long as the inner
surface was already painted).

## Part E - Example

`examples/surface_example.cpp` (added to `examples/CMakeLists.txt`):

- App init: `asset->create_surface(sess, 256, 256, 2.0f)` for a 256x256
  logical surface at 2x backing.
- `asset->paint_surface(sess, surface, 0xFF202020, draw_diag, &state)`
  where `draw_diag` paints a small bar-chart / waveform / whatever
  using the painter API.
- A `NEUI_W_CUSTOMDRAW` widget whose `WIDGET_PAINT` calls
  `painter_api->draw_asset(surface, 0, 0, 256, 256)`.
- A button to re-render the surface (e.g. randomise the bars), proving
  the cache-drop path works.

## Part F - Docs

Update `CLAUDE.md`:

- Bump `NEUI_ASSET_KIND_*` enumeration with `SURFACE = 4`.
- Add `create_surface` + `paint_surface` to the Painter + asset API
  section.
- Add this plan to the Plans bullet list.

## Out of scope

- File save (PNG / BMP encode) - `get_pixels_for_export` already
  enables it for any future caller; no encoder shipping in this
  feature.
- Repaint-on-dirty - clients drive `paint_surface` themselves; the
  framework doesn't track invalidations.
- Threaded paint - the off-screen ctx is touched only on the UI
  thread, matching every other backend call. A future capability
  could add a worker-thread-safe variant.
- Sharing a surface between sessions - handle is session-scoped, same
  as `BITMAP`.

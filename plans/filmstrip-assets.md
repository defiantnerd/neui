# Plan: filmstrip / stitchmap assets

## Status (branch `stitchmaps`)

- **Step 1 - store + draw plumbing: SHIPPED.** `FilmstripInfo` + `filmstrip`
  on `AssetEntry`; `set_frame_layout` / `frame_info` / `frame_count` /
  `frame_src_rect` store helpers; `filmstrip_src_rect` + `upload_entry_bitmap`
  refactor + `painter_draw_entry_frame_cached` in `hosts/shared/painter.h`.
  Tier-1 `tests/test_asset_filmstrip.cpp`.
- **Step 2 - public API: SHIPPED (Linux-verified; win/mac/iOS wired, not yet
  compiled there).** `neui_painter_api_t::draw_asset_frame`; `neui_asset_api_t`
  `set_frame_layout` / `create_filmstrip_from_file` / `get_frame_count` +
  `neui_filmstrip_orientation_t`. The draw thunk gained a trailing `frame`
  param + `k_draw_asset_whole` sentinel (one path serves whole-bitmap and
  per-cell draws); wired across xpl / win32 / macOS / iOS.
- **Step 4 - declarative `frame` prop: SHIPPED.** `frame` prop on compound
  asset layers (`compound.h` + `widget_paint_compound.h` routes every asset
  layer through `painter_draw_asset_frame_tinted`; behaviour-preserving for
  non-filmstrips); loader static `frame` + serialize round-trip + the generic
  `bind:{frame:...}`. Example `examples/filmstrip_knob_example.cpp`
  (`neui_filmstrip_knob_example`) - procedural SURFACE strip, value->frame
  bind, rotational drag.
- **Step 3 - recognition helpers: PENDING.** Sidecar JSON / filename / aspect
  heuristic, AND the component-doc `frame_layout` JSON sugar (tag a path-loaded
  asset from within the component) - deferred here since both belong with the
  path-based recognition layer.


Follow-up to `neui_asset_api_t` (and a natural companion to the
component system). Adds **frame-indexed bitmap assets** - the
"filmstrip" / "stitchmap" / "sprite strip" convention ubiquitous in
audio-plugin UIs, where one PNG packs N evenly-spaced frames of a
control (knob, fader, meter, animated ornament) and the value picks
which frame to draw.

Primary use case the user called out: a knob whose visual is an
artist-authored filmstrip (e.g. `knob_100frames.png`, 100 vertical
frames) where `neui.param.value` 0..1 maps to frame `0..99`. General
use: animated toggles, VU/meter ladders, spinners, any value -> frame
state machine.

## What a filmstrip actually is

A filmstrip is the **degenerate uniform-grid case of a texture
atlas**: a single bitmap divided into a `cols x rows` grid of
equal-size cells, the frames numbered in row-major order. The common
audio-plugin shape is a single column (`cols = 1`, `rows = N`) -
frames stacked top-to-bottom. Some assets pad a fixed `gutter` between
frames. There is **no in-band format** that marks a PNG/JPG as a
filmstrip; recognition is a layered convention (see Part C).

## Re-use angle

The rendering machinery already exists. The backend `draw_bitmap`
takes a **source sub-rect** today:

```c
draw_bitmap(ctx, bmp, src_x, src_y, src_w, src_h,   // <- frame window
                      dst_x, dst_y, dst_w, dst_h, tint);
```

and `neui_detail::painter_draw_entry_cached` (`hosts/shared/painter.h`)
currently hardcodes `0,0,0,0` ("full bitmap"). So drawing one frame is
a matter of computing the cell rect and passing it through the **same**
per-`(asset, ctx)` upload cache - the whole strip uploads to the GPU
once and every frame is a sub-rect sample of that one upload. No
per-frame textures, no extra memory, animating the frame is free.

A filmstrip is therefore **NOT a new asset kind**. It is a
`NEUI_ASSET_KIND_BITMAP` carrying optional frame-layout metadata. That
keeps it a plain bitmap for every existing consumer:

- `painter_api->draw_asset` (full-image draw) keeps working - it just
  draws frame... well, the whole strip, which we keep as the
  no-frame-specified behaviour for back-compat, but see "default frame"
  below.
- `get_pixels_for_export` (drag preview) is unaffected.
- Compound `asset` layers keep working; we *add* a `frame` prop.
- The per-ctx GPU cache, device-loss path, `@2x`/`@3x` resolution all
  apply unchanged.

What's actually new:

1. Frame-layout metadata on `AssetEntry`.
2. A frame-aware draw entry point (painter + thunk).
3. Recognition helpers (explicit API primary; filename + sidecar JSON
   opt-in convenience).
4. A `frame` property on compound `asset` layers so value -> frame is
   declarative and slots into `components/*.json`.

## Part A - Asset metadata + store

### `hosts/shared/asset_store.h`

Add a small optional struct, null on ordinary bitmaps:

```cpp
struct FilmstripInfo
{
  uint32_t frame_count = 0;
  uint32_t cols        = 1;   // vertical strip = (1, N)
  uint32_t rows        = 0;
  uint32_t gutter_px   = 0;   // physical px between cells (0 = tight pack)
  // Derived once at tag time (physical px), so the draw path does no
  // division per frame:
  uint32_t frame_w_px  = 0;
  uint32_t frame_h_px  = 0;
};
```

On `AssetEntry`:

```cpp
// Populated when a BITMAP/SURFACE asset has been tagged as a frame
// strip; null = ordinary single-image bitmap. Kind stays BITMAP.
std::unique_ptr<FilmstripInfo> filmstrip;
```

Store helpers:

- `bool set_frame_layout(uint32_t slot, uint32_t cols, uint32_t rows, uint32_t gutter_px)`
  - validates `cols >= 1 && rows >= 1`, that the grid fits
    (`cols*frame_w + (cols-1)*gutter <= width_px`, same for rows),
    computes `frame_w_px/frame_h_px`, sets `frame_count = cols*rows`.
    Returns false (leaves untagged) on a bad fit so a mis-tag can't
    produce out-of-bounds src-rects.
- `const FilmstripInfo* frame_info(uint32_t slot) const`
- `bool frame_src_rect(uint32_t slot, uint32_t frame, float& sx, float& sy, float& sw, float& sh) const`
  - row-major cell -> physical-px src rect; clamps `frame` into
    `[0, frame_count)` (out-of-range clamps rather than draws nothing,
    matching how a value past the end should pin to the last frame).

`allocate_from_file` / `allocate_bitmap` are untouched - tagging is a
second step (or done by the recognition layer in Part C). `SURFACE`
assets can be tagged too (a rendered atlas), since they share the
`pixels` + `bitmaps` layout.

## Part B - Draw path + public API

### Backend - nothing to do

`draw_bitmap` already samples a src-rect. Done.

### `hosts/shared/painter.h`

Add a frame-aware sibling to `painter_draw_entry_cached` (or extend it
with a default `frame = full` param). It resolves the src-rect from
`entry->filmstrip` (full bitmap when null) and forwards to
`draw_bitmap`:

```cpp
template <typename EntryT>
inline void painter_draw_entry_frame_cached(
    neui_render_backend_t* backend, neui_render_ctx_t ctx, EntryT* entry,
    uint32_t frame, float x, float y, float w, float h, uint32_t tint)
{
  // ... identical upload/device-loss block as painter_draw_entry_cached ...
  float sx = 0, sy = 0, sw = 0, sh = 0;   // 0,0,0,0 = full bitmap
  if (entry->filmstrip) frame_src_rect_for(entry, frame, sx, sy, sw, sh);
  if (backend->draw_bitmap)
    backend->draw_bitmap(ctx, bmp, sx, sy, sw, sh, x, y, w, h, tint);
}
```

The existing `painter_draw_entry_cached` becomes a one-line forwarder
with `frame = 0` semantics? No - keep it as "full bitmap" for
back-compat; the frame variant is the new path. Avoid changing the
meaning of an untagged draw.

### Thunk signature

`draw_asset_thunk_t` grows a `frame` param OR gets a sibling
`draw_asset_frame_thunk_t`. **Prefer a sibling** so the existing thunk
(and every host's `draw_asset` wiring) stays byte-stable and the new
path is purely additive. Carry it on `neui_painter` alongside
`draw_asset_thunk`.

### `include/neui/d/painter.h` (client draw surface)

Append after `draw_asset` (ABI: end of struct):

```c
// Draw frame `frame` of a filmstrip asset into the dest rect. If the
// asset carries no frame layout, draws the whole bitmap (== draw_asset).
// frame clamps into [0, frame_count).
void (NEUI_ABI *draw_asset_frame)(neui_painter_t* p, neui_asset_t asset,
                                  uint32_t frame,
                                  float x, float y, float w, float h);
void (NEUI_ABI *draw_asset_frame_tinted)(/* + uint32_t tint */);
```

### `include/neui/d/assets.h` (`NEUI_API_ASSETS`, vtable-appended)

```c
// Tag an existing BITMAP/SURFACE asset with a frame grid (row-major).
// cols/rows >= 1; vertical strip = cols 1. gutter_px = inter-cell pad.
// Returns false (asset stays a plain bitmap) if the grid doesn't fit.
neui_bool (NEUI_ABI *set_frame_layout)(neui_session_t, neui_asset_t,
                                       uint32_t cols, uint32_t rows,
                                       uint32_t gutter_px);

// Convenience: load + tag in one call. orientation picks the 1xN / Nx1
// degenerate grid; for a true 2D grid use create_from_file + set_frame_layout.
neui_asset_t (NEUI_ABI *create_filmstrip_from_file)(
    neui_session_t, const char* path, uint32_t frame_count,
    neui_filmstrip_orientation_t orientation);

uint32_t (NEUI_ABI *get_frame_count)(neui_session_t, neui_asset_t); // 0 = not a strip
```

`neui_filmstrip_orientation_t { NEUI_FILMSTRIP_VERTICAL = 0, NEUI_FILMSTRIP_HORIZONTAL = 1 }`.

Per-host glue: the three `draw_asset` thunks gain a frame-aware twin
(win32 `widgets.cpp`, macOS `widgets.mm`, xpl `host.cpp`), each just
forwarding the resolved entry to `painter_draw_entry_frame_cached`. The
three asset-api vtables gain the three new methods backed by the new
store helpers. Pure addition - no existing thunk changes.

## Part C - Recognition (layered, never silent)

There is no reliable in-band marker, so default to **explicit
declaration** and offer convenience layers the client opts into:

1. **Explicit (primary)** - `set_frame_layout` / `create_filmstrip_from_file`.
   Always correct, always available.
2. **Sidecar JSON (robust convention)** - a `<image>.json` next to the
   file: `{ frames: 100, orientation: "vertical", gutter: 0 }` (or a
   full `{ cols, rows, gutter }`). Parse with the in-lib
   `neui::mujson` (already compiled in - near-zero cost). Resolved
   inside a new `create_filmstrip_from_file(path, /*frame_count=*/0, ...)`
   path: `frame_count == 0` means "look for a sidecar."
3. **Filename suffix (audio-plugin convenience)** - parse trailing
   tokens: `_100frames`, `-128`, `_strip128`, `_fNN`. A small helper
   `parse_filmstrip_filename(name) -> optional<count>`. Opt-in via the
   same `frame_count == 0` discovery path, *after* the sidecar check.
4. **Aspect-ratio heuristic (hint only, never authoritative)** - if
   `height % width == 0 && height/width >= 4`, *suggest* a vertical
   strip of `height/width`. Only surfaced through an explicit
   `guess_frame_count(asset)` query a tool/designer could call - NEVER
   auto-applied (a tall photo must not get sliced).

Recognition layers 2-4 live in the **xpl/shared asset code** (they need
file IO + mujson), reachable from all hosts. The store core
(`set_frame_layout`) stays pure metadata.

## Part D - Declarative value -> frame (the payoff)

A filmstrip's reason to exist is `value -> frame`. That maps exactly
onto the existing compound `asset`-layer binding.

### `include/neui/d/compound.h`

Add a `frame` prop to the asset layer kind:

```
asset layer props:
  "asset"     asset   bitmap/filmstrip handle
  "frame"     int     frame index to draw (default 0; ignored if the
                      asset has no frame layout)
  "rotation"  float
  "tint"      int
```

`paint_compound_*` (`hosts/shared/widget_paint_compound.h`) routes an
asset layer with `frame` set through `draw_asset_frame` instead of
`draw_asset`. Then:

```c
// knob value 0..1 -> frame 0..(count-1)
compound->bind(sess, comp, layer, "frame", "neui.param.value",
               /*scale=*/ frame_count - 1, /*offset=*/ 0);  // rounds to int
```

`bind` already reads the attr as float, computes `scale*x + offset`,
and rounds for int targets - so this works with zero new binding code.

### Component JSON

The component loader (`hosts/shared/component_loader.h`) maps each JSON
field 1:1 onto a compound/behavior prop, so a `frame` layer prop + a
`bind` to `value` falls out for free. A filmstrip knob becomes pure
data in `components/knob_filmstrip.json` - no imperative code:

```jsonc
{ kind: "asset", asset: "knob_100frames.png", frame_layout: { frames: 100 },
  bind: { frame: { attr: "neui.param.value", scale: 99 } } }
```

(`frame_layout` in the component doc tells the loader to
`set_frame_layout` the loaded asset; `build_component` gains that one
call.)

## Implementation order

1. **Store + draw plumbing** (Part A + B minus the public API): the
   `FilmstripInfo` struct, store helpers, `painter_draw_entry_frame_cached`,
   the sibling thunk. Smallest change; unlocks everything. Verifiable
   by a Tier-1 test against fake loader/backend.
2. **Public API** (`set_frame_layout` / `create_filmstrip_from_file` /
   `get_frame_count` / `draw_asset_frame`) wired on all three hosts.
3. **Recognition helpers** (sidecar JSON via mujson, filename parse,
   heuristic query) - opt-in, in shared/xpl asset code.
4. **Declarative** - compound `frame` prop + bind routing + component
   JSON `frame_layout`.

## Verification

- **Tier-1** `tests/test_asset_filmstrip.cpp` (header-only, fake
  loader + backend stub, builds on null platform): `set_frame_layout`
  computes the right `frame_w/h`, `frame_src_rect` returns correct
  row-major cells incl. gutter, out-of-fit grid is rejected,
  out-of-range frame clamps. Parse helpers:
  `parse_filmstrip_filename("knob_100frames")` -> 100; sidecar JSON
  round-trip; heuristic only fires on tall integer-multiple images.
- **Linux smoke** - extend or add a Cairo smoke test that tags a
  generated strip and `read_pixels_bgra` confirms frame N samples the
  expected cell.
- **Example** `examples/filmstrip_knob_example.cpp` ->
  `neui_filmstrip_knob_example`: a knob driven by a bundled (or
  procedurally-generated) vertical filmstrip via the component path,
  plus a CUSTOMDRAW that scrubs frames on drag - hand-verified on the
  user's Linux box.

## ABI notes

- All public additions are **vtable-appended** (end of
  `neui_asset_api_t`, `neui_painter_api_t`) - slot offsets stay stable.
- New `draw_asset` thunk is a **sibling**, not a signature change, so
  every existing host draw path is byte-stable.
- `AssetEntry` grows a `unique_ptr` (one pointer) - like the `comp_*`
  campaign, a full rebuild is required (stale objects compiled against
  the old struct size would corrupt entries).

## Deferred / non-goals

- **True texture-atlas (non-uniform frames)** - arbitrary per-frame
  rects + a named-frame map (TexturePacker JSON). The uniform grid here
  is the 90% case; a general atlas is a separate `NEUI_ASSET_KIND`
  conversation.
- **PNG `tEXt`/`iTXt` embedded frame count** - stb_image doesn't expose
  text chunks; would need a tiny chunk scanner. Sidecar JSON covers the
  same need without parsing PNG internals.
- **Per-frame timing / playback** (animated GIF-style auto-advance) -
  out of scope; frame selection stays value-driven. A future
  behavior-asset "frame ticker" handler could drive it.
- **Built-in KNOB filmstrip skin** - the native KNOB stays painted;
  filmstrip knobs go through CUSTOMDRAW + compound (the component path),
  which is the intended skinning seam.

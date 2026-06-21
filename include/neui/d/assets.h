#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"

// Asset API - session-scoped media handles (bitmaps, surfaces, compounds,
// behaviors, and client-registered fonts; SVG / vector reserved). Acquired via
//   neui_asset_api_t* a = (neui_asset_api_t*)
//     neui_api->get_interface(session, NEUI_API_ASSETS);
// and used outside the paint loop to load / preload media. During paint,
// clients render the loaded asset via neui_painter_api_t::draw_asset with
// the handle returned here.
//
// The handle layout matches neui_widget_t: upper 16 bits = owning session
// id, lower 16 bits = slot. Cross-session handles are rejected by the
// host. Handles are not generation-checked - reusing a slot after destroy
// can collide silently (matches the existing widget id contract; deferred).

// Forward-declarations for the surface paint callback below. The full
// types live in <neui/d/painter.h>; we don't pull that header here to
// keep the include graph tight (clients that use neui_asset_api_t
// without ever calling paint_surface don't need painter.h to compile).
struct neui_painter;
struct neui_painter_api;

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_ASSETS "com.defiantnerd.neui.extension.assets/0"

  typedef struct neui_asset {
    uint32_t id;
  } neui_asset_t;

  static const neui_asset_t asset_none = { UINT32_MAX };

  // Discriminator returned by neui_asset_api::get_kind. Lets future code
  // branch on kind without breaking ABI - the enum reserves slots for
  // formats that aren't implemented yet. NEUI_ASSET_KIND_NONE is also
  // returned for invalid handles.
  typedef enum neui_asset_kind {
    NEUI_ASSET_KIND_NONE     = 0,
    NEUI_ASSET_KIND_BITMAP   = 1,
    // Compound drawable: a mutable, declarative stack of typed layers
    // (text / asset) read at paint time. Built via NEUI_API_COMPOUND.
    NEUI_ASSET_KIND_COMPOUND = 2,
    // Behavior: a mutable list of input handlers (drag / wheel / key /
    // click / context-reset) that, when attached to a CUSTOMDRAW widget,
    // manipulate attrs in the widget's AttrBag in response to mouse /
    // keyboard / wheel events. Built via NEUI_API_BEHAVIOR.
    NEUI_ASSET_KIND_BEHAVIOR = 3,
    // Client-owned off-screen render target. Created via
    // neui_asset_api::create_surface and populated via paint_surface;
    // backed by a CPU pixel buffer that's uploaded to per-window GPU
    // caches on draw just like NEUI_ASSET_KIND_BITMAP. Lets clients
    // bake visual diagnostic outputs / thumbnails / cached ornaments
    // once and then draw the result via painter_api->draw_asset.
    NEUI_ASSET_KIND_SURFACE  = 4,
    // Reserved for future kinds. Do NOT renumber; bind new values to the
    // next unused integer so old client builds stay forward-compatible.
    // NEUI_ASSET_KIND_SVG    = 5,
    // NEUI_ASSET_KIND_VECTOR = 6,
    // Client-registered font. Created via neui_asset_api::create_font /
    // create_from_file; the handle owns the registration lifetime. The font
    // is referenced for drawing by its family-name string (NEUI_ATTR_FONT_*
    // / push_font), exactly like a system font - the handle itself is never
    // passed to the draw path. See "Font loading" in CLAUDE.md.
    NEUI_ASSET_KIND_FONT     = 7,
    // Component: a reusable, declarative bundle of a COMPOUND (visual) + a
    // BEHAVIOR (input) + default attrs + a param manifest + a default size,
    // loaded from a JSON document via create_component_from_file / _string and
    // instantiated via widgets->create_from_component (or attached to an
    // existing CUSTOMDRAW via widgets->set_asset). A *component* bundles a
    // *compound* plus a *behavior* - distinct kinds. See <neui/d/component.h>.
    NEUI_ASSET_KIND_COMPONENT = 8,
  } neui_asset_kind_t;

  // --- Component loading types (NEUI_ASSET_KIND_COMPONENT) ----------------
  // Defined here (rather than in <neui/d/component.h>) so neui_asset_api_t can
  // reference them without a circular include; component.h is the doc umbrella
  // and includes this header.

  // Optional asset-resolution overrides for component loading. May be NULL ->
  // pure path mode (asset names resolved as files relative to base_dir).
  //   base_dir      - root for relative asset paths. create_component_from_file
  //                   sets it to the .json's directory automatically; for
  //                   _from_string the caller sets it (NULL / "" = cwd).
  //   resolve_asset - consulted FIRST for each asset name (return asset_none to
  //                   fall through to path mode). Lets a client inject pre-
  //                   loaded / in-memory handles by name. hint_path is the raw
  //                   string from the JSON "assets" map. Returned handles are
  //                   borrowed (client-owned); path-resolved handles are owned
  //                   by the component and released with it.
  //   user          - passed back to resolve_asset.
  typedef struct neui_component_env {
    const char*  base_dir;
    neui_asset_t (NEUI_ABI *resolve_asset)(void* user, const char* name,
                                           const char* hint_path);
    void*        user;
  } neui_component_env_t;

  // One entry of a component's parameter manifest (from the JSON "params"
  // list). key / label point into component-owned storage and stay valid
  // until the component asset is destroyed. min / max / def are the declared
  // range and default (def is also stamped into each instance's AttrBag).
  typedef struct neui_component_param {
    const char* key;
    const char* label;
    float       min;
    float       max;
    float       def;
  } neui_component_param_t;

  // Orientation for the create_filmstrip_from_file convenience form: a
  // single-row (HORIZONTAL) or single-column (VERTICAL) strip of
  // frame_count equal cells. For a true 2D grid, load the bitmap with
  // create_from_file and tag it with set_frame_layout(cols, rows, gutter).
  typedef enum neui_filmstrip_orientation {
    NEUI_FILMSTRIP_VERTICAL   = 0,   // cols 1, rows frame_count (top-to-bottom)
    NEUI_FILMSTRIP_HORIZONTAL = 1,   // cols frame_count, rows 1 (left-to-right)
  } neui_filmstrip_orientation_t;

  // Client paint callback for neui_asset_api::paint_surface. Mirrors the
  // NEUI_EVENT_WIDGET_PAINT payload shape so a client can reuse the same
  // drawing code on a widget and on a surface. The painter / api pair is
  // valid only for the duration of the call.
  typedef void (NEUI_ABI *neui_surface_paint_fn)(
      struct neui_painter*     painter,
      struct neui_painter_api* api,
      float                    width_logical,
      float                    height_logical,
      void*                    user);

  typedef struct neui_asset_api {
    uint32_t neui_version;

    // Create a bitmap asset from raw BGRA8 (premultiplied) pixels.
    //   width_px / height_px - physical pixel dimensions.
    //   bgra_premul          - width_px * height_px * 4 bytes; copied by
    //                          the host (caller may free immediately).
    //   scale                - HiDPI factor of the source pixels
    //                          (1.0 = @1x, 2.0 = @2x, 3.0 = @3x). The
    //                          asset's logical size is pixel_size / scale,
    //                          so an @2x bitmap declared with scale=2 draws
    //                          at half its pixel dimensions.
    // Returns asset_none on failure (bad args, allocation, etc.).
    neui_asset_t (NEUI_ABI *create_bitmap)(neui_session_t session,
                                            uint32_t width_px,
                                            uint32_t height_px,
                                            const uint8_t* bgra_premul,
                                            float scale);

    // Create a bitmap asset from a file path or an embedded resource name.
    // PNG / JPG / BMP via the platform loader. On Win32 the name is also
    // tried as an embedded RT_USERDATA "PNG" resource (matching the host's
    // existing image-load behaviour).
    // Resolution: a name "foo.png" loaded with the current display scale
    // > 1.0 will prefer "foo@2x.png" / "foo@3x.png" if present.
    neui_asset_t (NEUI_ABI *create_from_file)(neui_session_t session,
                                                const char* path_utf8);

    // Release the asset. CPU pixels are freed immediately; per-context
    // GPU uploads are released on the next paint or at session destroy.
    // Drawing a destroyed handle is a no-op (not a crash).
    void (NEUI_ABI *destroy)(neui_session_t session, neui_asset_t asset);

    // Intrinsic asset size in logical (96-DPI) pixels.
    //   bitmap: width_px / scale, height_px / scale.
    //   future SVG: viewBox dimensions if present, else false.
    // out_logical_w / out_logical_h may be NULL to ignore.
    // Returns false on invalid handle or asset with no intrinsic size.
    bool (NEUI_ABI *get_size)(neui_session_t session, neui_asset_t asset,
                               float* out_logical_w, float* out_logical_h);

    // Returns the asset's kind discriminator. NEUI_ASSET_KIND_NONE if the
    // handle is invalid.
    neui_asset_kind_t (NEUI_ABI *get_kind)(neui_session_t session,
                                            neui_asset_t asset);

    // Create a compound drawable asset - an empty, mutable layer stack.
    // Populate via NEUI_API_COMPOUND. Returns asset_none on allocation
    // failure. Hosts that don't support compound (e.g. native macOS;
    // CUSTOMDRAW isn't implemented there) return asset_none.
    neui_asset_t (NEUI_ABI *create_compound)(neui_session_t session);

    // Create a behavior asset - an empty, mutable list of input handlers.
    // Populate via NEUI_API_BEHAVIOR. Returns asset_none on allocation
    // failure. Attaches to CUSTOMDRAW via widgets->set_asset (kind-routed).
    neui_asset_t (NEUI_ABI *create_behavior)(neui_session_t session);

    // Create an off-screen render surface (NEUI_ASSET_KIND_SURFACE).
    //   width_logical / height_logical - logical (96-DPI) dimensions.
    //   scale - HiDPI factor of the backing pixels (1.0 / 2.0 / 3.0).
    //     The physical pixel buffer is round(width_logical * scale) by
    //     round(height_logical * scale); a scale of 2.0 on a 100x100
    //     logical surface backs 200x200 pixels so a 1:1 draw on a 2x
    //     display stays crisp.
    // Returns asset_none on allocation failure, or on backends that
    // don't support off-screen targets (null backend). The surface is
    // valid (and draw_asset draws transparent black) until the first
    // paint_surface call populates it.
    neui_asset_t (NEUI_ABI *create_surface)(neui_session_t session,
                                              float width_logical,
                                              float height_logical,
                                              float scale);

    // Drive a client paint callback against an off-screen surface:
    //   1. begin_frame on the surface's off-screen ctx (clear to clear_argb).
    //   2. push a clip to [0,0,width_logical,height_logical].
    //   3. invoke fn(painter, api, width_logical, height_logical, user).
    //   4. pop_clip, end_frame.
    //   5. read pixels back into the asset's CPU buffer.
    //   6. drop any cached per-window GPU uploads of this asset so the
    //      next draw_asset call re-uploads the new pixels.
    // Safe to call any time EXCEPT inside a WIDGET_PAINT callback (the
    // backend's path / transform / alpha state is mid-frame). Safe to
    // call repeatedly to re-render. No-op on non-SURFACE handles, on
    // null fn, or on backends without off-screen support.
    void (NEUI_ABI *paint_surface)(neui_session_t        session,
                                    neui_asset_t          surface,
                                    uint32_t              clear_argb,
                                    neui_surface_paint_fn fn,
                                    void*                 user);

    // --- Font loading (NEUI_ASSET_KIND_FONT) -----------------------------
    //
    // Register a client-supplied font so its family becomes resolvable for
    // text rendering WITHOUT installing it system-wide. The font is then
    // referenced by its family-name string (set NEUI_ATTR_FONT_FAMILY on a
    // widget, or call push_font in a CUSTOMDRAW paint) exactly like a system
    // font - the returned handle only owns the registration lifetime and is
    // never passed to the draw path. Push, not pull: an unknown family still
    // falls back to the host default; registration just widens the set of
    // names that resolve. Family-name collisions are last-wins (ship a
    // uniquely-renamed family to avoid clashes).
    //
    // A multi-weight family is several files (Regular / Bold / ...); register
    // each file with one call - the backend coalesces faces that share a
    // family name, and push_font(family, weight) selects the weight. Italic
    // is not selectable today (the font stack is family + weight only).

    // Register an in-memory font (TTF / OTF / TTC bytes). The framework
    // copies the bytes and owns the copy for the asset's lifetime (the
    // FreeType / DirectWrite in-memory loaders require the buffer stay live).
    // Returns asset_none if the backend cannot register fonts (null backend)
    // or the data is not a usable font.
    neui_asset_t (NEUI_ABI *create_font)(neui_session_t session,
                                         const uint8_t* data,
                                         uint32_t       len);

    // Path convenience form (.ttf / .otf / .ttc). Resolution is immediate;
    // returns asset_none on failure. More robust than create_font on
    // backends that prefer URL / path registration.
    neui_asset_t (NEUI_ABI *create_font_from_file)(neui_session_t session,
                                                   const char*    path_utf8);

    // Write the registered family name into out_buf (UTF-8, NUL-terminated,
    // truncated to cap). Returns the full length (excluding the NUL), or 0
    // if the asset is not a FONT. Lets the client discover the name to pass
    // to NEUI_ATTR_FONT_FAMILY when it differs from the filename.
    uint32_t (NEUI_ABI *get_font_family)(neui_session_t session,
                                         neui_asset_t   font,
                                         char*          out_buf,
                                         uint32_t       cap);

    // --- Component loading (NEUI_ASSET_KIND_COMPONENT) -------------------
    // (Vtable-appended; check the api version / pointer before calling.)
    //
    // Parse a JSON component document (in-host neui::mujson) and materialize
    // it into a COMPONENT asset that owns a COMPOUND (visual) + a BEHAVIOR
    // (input) + a default-attr template + a param manifest + a default size.
    // env may be NULL (path mode). Returns asset_none on malformed JSON, an
    // unusable document, or a backend without compound/behavior support (e.g.
    // the null backend) - graceful, like create_surface. get_kind returns
    // NEUI_ASSET_KIND_COMPONENT; get_size returns the default size; destroy
    // releases the owned compound + behavior (and any path-loaded layer
    // assets) too. Instantiate via widgets->create_from_component.
    neui_asset_t (NEUI_ABI *create_component_from_string)(neui_session_t session,
                                                          const char* json_utf8,
                                                          uint32_t len,
                                                          const neui_component_env_t* env);
    // File convenience: reads the file and sets env.base_dir to its directory
    // (so relative asset paths resolve next to the .json). Returns asset_none
    // on read failure or malformed JSON.
    neui_asset_t (NEUI_ABI *create_component_from_file)(neui_session_t session,
                                                        const char* path_utf8,
                                                        const neui_component_env_t* env);
    // Parameter manifest passthrough (the JSON "params" list). count returns
    // the number of declared params; at writes the i-th into *out (its key /
    // label point into component-owned storage, valid until the component is
    // destroyed) and returns true, or false for an out-of-range index /
    // non-component asset.
    uint32_t (NEUI_ABI *component_param_count)(neui_session_t session,
                                               neui_asset_t component);
    bool     (NEUI_ABI *component_param_at)(neui_session_t session,
                                            neui_asset_t component,
                                            uint32_t index,
                                            neui_component_param_t* out);

    // Serialize a COMPONENT asset back to a JSON document (designer round-
    // trip). Writes the structural definition only - size, params, assets,
    // layers, behavior - using minimal-diff emission (only properties that
    // differ from their defaults). Per-instance attr values are NOT part of a
    // component, so none are written; an asset layer serializes by the name it
    // was loaded under. Writes up to cap bytes including the NUL into out_buf
    // and returns the full length excluding the NUL (call with out_buf=NULL /
    // cap=0 to size first). Returns 0 for a non-component / invalid asset.
    // indent = spaces per nesting level (0 = compact single line).
    uint32_t (NEUI_ABI *serialize_component)(neui_session_t session,
                                             neui_asset_t component,
                                             char* out_buf, uint32_t cap,
                                             int indent);

    // --- Filmstrip / stitchmap assets ------------------------------------
    // (Vtable-appended; check the api version / pointer before calling.)
    //
    // A filmstrip ("stitchmap" / "sprite strip") is a BITMAP / SURFACE asset
    // whose pixels pack frame_count equal-size frames in a cols x rows
    // row-major grid (the audio-plugin convention: a single column of N
    // stacked knob/fader frames, value -> frame). Tagging an asset doesn't
    // change its kind; it stays a bitmap for every other consumer. Draw a
    // single frame via painter_api->draw_asset_frame.

    // Tag an existing BITMAP / SURFACE asset with a frame grid. cols / rows
    // >= 1 (vertical strip = cols 1; horizontal = rows 1). gutter_px is the
    // physical-pixel padding between cells (0 = tight pack). Cell size is the
    // floor of the fitted division, so the grid always stays in bounds.
    // Returns false (leaving the asset an untagged plain bitmap) for a
    // non-bitmap kind, cols/rows < 1, a zero-size bitmap, or a grid that
    // can't fit at least 1 px per cell. Re-tagging overwrites a prior layout.
    bool (NEUI_ABI *set_frame_layout)(neui_session_t session, neui_asset_t asset,
                                       uint32_t cols, uint32_t rows,
                                       uint32_t gutter_px);

    // Load a bitmap from a file (like create_from_file, incl. @2x / @3x
    // resolution) and tag it as a frame strip, in one call.
    //   frame_count > 0 : explicit - a frame_count-frame strip in `orientation`.
    //   frame_count == 0: DISCOVER the layout from a "<path>.json" /
    //                     "<base>.json" sidecar ({ "frames": N, "orientation":
    //                     ..., "gutter": ... } or { "cols", "rows", "gutter" }),
    //                     else a "<N>frames" / "-<N>" / "_f<N>" / "strip<N>"
    //                     filename token (count only - `orientation` picks the
    //                     axis).
    // Returns asset_none if the load fails, the strip doesn't divide evenly
    // enough to fit, or (discovery) no sidecar / filename token is found.
    neui_asset_t (NEUI_ABI *create_filmstrip_from_file)(
        neui_session_t session, const char* path_utf8,
        uint32_t frame_count, neui_filmstrip_orientation_t orientation);

    // Number of frames in a filmstrip asset, or 0 if the handle is invalid
    // or the asset carries no frame layout (i.e. an ordinary bitmap).
    uint32_t (NEUI_ABI *get_frame_count)(neui_session_t session,
                                         neui_asset_t asset);
  } neui_asset_api_t;

#ifdef __cplusplus
}
#endif

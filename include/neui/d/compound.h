#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "assets.h"
#include "gradient.h"

// Compound drawable API - reached via
//   neui_compound_api_t* co = (neui_compound_api_t*)
//     neui_api->get_interface(session, NEUI_API_COMPOUND);
// and operates on neui_asset_t handles previously returned from
// neui_asset_api_t::create_compound.
//
// A compound is a mutable layer stack:
//   - each layer has a kind (text, asset, or rect),
//   - a signed-int z-order (relative to child widgets: z<0 paints below
//     children, z>=0 paints above),
//   - a 9-point anchor pair (parent + self) plus an integer pixel offset,
//   - a size in logical pixels (per axis: NEUI_COMPOUND_FILL = -1 to span
//     the widget on that axis),
//   - typed properties (text-layer: text/size/color/align_x/align_y;
//     asset-layer: asset/rotation; both: alpha),
//   - optional bindings that read values from the widget's AttrBag at
//     paint time, with an optional scale+offset transform for numerics.
//
// Bindings: a layer property bound to attr key K with (scale, offset)
// computes `scale * attr_as_float(K) + offset` at paint time and uses
// that as the effective property value. For string properties, the
// "binding" is the template syntax `{key}` inside set_string instead -
// see set_string below.
//
// Text templates: the `text` property on a text layer is interpreted as
// a template. `{key}` is replaced by the attr `key` rendered as text;
// unknown keys yield the empty string; literal `{` / `}` are written
// as `{{` / `}}`. Future-compatible with format specs of the form
// `{key:.2f}` (deferred).

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_COMPOUND "com.defiantnerd.neui.extension.compound/0"

// Sentinel for "match the widget's dimension on this axis."
#define NEUI_COMPOUND_FILL (-1)

// Layer state filter bits. A layer with NEUI_PROP_SHOW_WHEN = 0 (default)
// is painted in every state. Otherwise the mask is an AND filter across
// three axes (enabled, hovered, pressed): each axis can require the
// positive bit, the negated bit, or neither (don't care).
//
// The visibility rule is:
//   layer visible iff (show_when & ~current_state) == 0
// i.e. every bit set in show_when must be present in current_state.
//
// current_state is composed each paint and always carries exactly one
// bit per axis - the positive bit if the widget is in that state, the
// NOT_* bit if it isn't. Reacts to internal hover / press / enabled
// detection only; independent of any client event emission.
//
//   ENABLED  / NOT_ENABLED  - mirrors widgets->get_enabled
//   HOVERED  / NOT_HOVERED  - mouse cursor inside the widget rect
//   PRESSED  / NOT_PRESSED  - mouse button held since press on this
//                             widget (capture-style: survives the cursor
//                             leaving the rect, clears on release)
//
// Examples:
//   show_when = HOVERED                       -> only when hovered
//   show_when = NOT_ENABLED                   -> only when disabled
//   show_when = HOVERED | NOT_PRESSED         -> only when hovered AND not pressed
//   show_when = ENABLED | HOVERED | PRESSED   -> only when all three hold
//
// Setting both bits on the same axis (e.g. ENABLED | NOT_ENABLED) is a
// contradiction and the layer is never visible.
#define NEUI_LAYER_STATE_ENABLED      (1u << 0)
#define NEUI_LAYER_STATE_HOVERED      (1u << 1)
#define NEUI_LAYER_STATE_PRESSED      (1u << 2)
#define NEUI_LAYER_STATE_NOT_ENABLED  (1u << 3)
#define NEUI_LAYER_STATE_NOT_HOVERED  (1u << 4)
#define NEUI_LAYER_STATE_NOT_PRESSED  (1u << 5)

// Prop name for the state filter. Set via set_int. Default value 0
// means "visible in every state".
//
//   compound->set_int(sess, asset, layer, NEUI_PROP_SHOW_WHEN,
//                     NEUI_LAYER_STATE_HOVERED | NEUI_LAYER_STATE_NOT_PRESSED);
#define NEUI_PROP_SHOW_WHEN "show_when"

  // Layer handle. Internally (asset_id << 16) | slot - same layout as
  // neui_asset_t / neui_widget_t. Slot-reused on remove_layer.
  typedef struct neui_compound_layer {
    uint32_t id;
  } neui_compound_layer_t;

  static const neui_compound_layer_t compound_layer_none = { UINT32_MAX };

  typedef enum neui_compound_layer_kind {
    NEUI_COMPOUND_LAYER_NONE  = 0,
    NEUI_COMPOUND_LAYER_TEXT  = 1,
    NEUI_COMPOUND_LAYER_ASSET = 2,
    NEUI_COMPOUND_LAYER_RECT  = 3,
    NEUI_COMPOUND_LAYER_PATH  = 4,
    NEUI_COMPOUND_LAYER_QR    = 5,
  } neui_compound_layer_kind_t;

  // QR error-correction levels for NEUI_COMPOUND_LAYER_QR's "ecc" prop.
  // Numerically match qrcodegen::QrCode::Ecc. Higher = more redundancy
  // (scannable when partly obscured) at the cost of a denser symbol.
  typedef enum neui_qr_ecc {
    NEUI_QR_ECC_LOW      = 0,  // ~7%  recovery
    NEUI_QR_ECC_MEDIUM   = 1,  // ~15% recovery (default)
    NEUI_QR_ECC_QUARTILE = 2,  // ~25% recovery
    NEUI_QR_ECC_HIGH     = 3,  // ~30% recovery
  } neui_qr_ecc_t;

  // Path-layer command kinds. The layer carries a sequence of these
  // assigned via set_path; the painter replays them against the
  // backend's path API (begin_path / move_to / line_to / arc / close_path
  // / fill_path / stroke_path). Coordinates are in logical pixels, with
  // (0, 0) at the layer rect's top-left - the painter translates to
  // the resolved layer rect before replaying, so authoring a 24x24
  // icon does not require knowing the widget's size.
  //
  // The five-float `args` slot interprets per kind:
  //   MOVE_TO: args[0] = x, args[1] = y
  //   LINE_TO: args[0] = x, args[1] = y
  //   ARC:     args[0] = cx, args[1] = cy, args[2] = r,
  //            args[3] = a_start (radians), args[4] = a_end (radians)
  //   CLOSE:   args ignored
  typedef enum neui_path_cmd_kind {
    NEUI_PATH_CMD_MOVE_TO = 0,
    NEUI_PATH_CMD_LINE_TO = 1,
    NEUI_PATH_CMD_ARC     = 2,
    NEUI_PATH_CMD_CLOSE   = 3,
  } neui_path_cmd_kind_t;

  typedef struct neui_path_cmd {
    uint32_t kind;
    float    args[5];
  } neui_path_cmd_t;

  // 9-point anchor system. Layer geometry: a layer's `self_anchor` point
  // is aligned with its parent's (the widget's) `parent_anchor` point,
  // then the (offset_x, offset_y) shift is applied. Positive x = right,
  // positive y = down (matches renderer convention).
  typedef enum neui_anchor {
    NEUI_ANCHOR_TOP_LEFT     = 0,
    NEUI_ANCHOR_TOP          = 1,
    NEUI_ANCHOR_TOP_RIGHT    = 2,
    NEUI_ANCHOR_LEFT         = 3,
    NEUI_ANCHOR_CENTER       = 4,
    NEUI_ANCHOR_RIGHT        = 5,
    NEUI_ANCHOR_BOTTOM_LEFT  = 6,
    NEUI_ANCHOR_BOTTOM       = 7,
    NEUI_ANCHOR_BOTTOM_RIGHT = 8,
  } neui_anchor_t;

  typedef struct neui_compound_api {
    uint32_t neui_version;

    // -------- Layer management ---------------------------------------------

    // Append a new layer to the compound and return its handle. z is the
    // signed paint-order key (z<0 paints below the widget's children,
    // z>=0 paints above; ties broken by insertion order). Returns
    // compound_layer_none on bad args or non-compound asset.
    neui_compound_layer_t (NEUI_ABI *add_layer)(neui_session_t session,
                                                  neui_asset_t asset,
                                                  neui_compound_layer_kind_t kind,
                                                  int z);

    // Remove a layer. Slot may be reused by a later add_layer; existing
    // handles to the removed slot become inert.
    void (NEUI_ABI *remove_layer)(neui_session_t session,
                                   neui_asset_t asset,
                                   neui_compound_layer_t layer);

    // Remove all layers.
    void (NEUI_ABI *clear)(neui_session_t session, neui_asset_t asset);

    // Update a layer's z (paint order).
    void (NEUI_ABI *set_z)(neui_session_t session, neui_asset_t asset,
                            neui_compound_layer_t layer, int z);

    // -------- Geometry -----------------------------------------------------

    // Set the layer's anchor pair. parent_anchor is a point on the
    // widget rect; self_anchor is a point on the layer rect; the two
    // are aligned. Anchors themselves are not bindable.
    void (NEUI_ABI *set_anchor)(neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer,
                                  neui_anchor_t parent_anchor,
                                  neui_anchor_t self_anchor);

    // -------- Static property setters --------------------------------------
    //
    // Each layer kind recognises a fixed set of property names; unknown
    // names are stored but inert (mirrors AttrBag semantics).
    //
    //   text layer props:
    //     "text"      string  (template - {key} placeholders)
    //     "size"      float   font size in logical px
    //     "color"     int     ARGB - optional; when neither set explicitly
    //                         nor bound, the layer reads the active
    //                         theme's text_primary colour so it tracks
    //                         light / dark mode automatically.
    //     "align_x"   int     0 = start (left), 1 = center, 2 = end (right)
    //     "align_y"   int     0 = top, 1 = center, 2 = bottom
    //   asset layer props:
    //     "asset"     asset   bitmap / filmstrip handle (v1)
    //     "frame"     int     filmstrip cell to draw (row-major), default 0.
    //                         Ignored when the asset has no frame layout (an
    //                         ordinary bitmap draws whole). Commonly bound to
    //                         a value attr - bind("frame", "neui.param.value",
    //                         frame_count - 1, 0) - so a knob/fader value
    //                         scrubs the strip. Negative results clamp to 0.
    //     "rotation"  float   radians, clockwise
    //     "tint"      int     ARGB multiplicative tint. Default 0xFFFFFFFF
    //                         is the passthrough sentinel (no tint, hot
    //                         path unchanged). Any other value runs the
    //                         backend's native multiplicative-tint
    //                         primitive (D2D effect on Windows,
    //                         blend-mode multiply + alpha mask on macOS);
    //                         the source bitmap is cached once per
    //                         (asset, ctx) regardless of how many tints
    //                         reference it, so animating the tint is
    //                         free. Set to a theme colour to track
    //                         light / dark on monochrome icons; bind to
    //                         a state-tracking attr to swap on hover /
    //                         press without per-state asset uploads.
    //   qr layer props:
    //     "text"        string  template (same {key} substitution as the
    //                           text layer) for the string to encode.
    //                           Default "{value}". OVERRIDDEN when the
    //                           widget's AttrBag carries a non-empty string
    //                           attr "neui.attr.qrcode" (NEUI_ATTR_QRCODE) -
    //                           that value is then encoded verbatim.
    //     "fill_color"  int     ARGB of the dark modules. 0 (default) =
    //                           the active theme's text_primary (tracks
    //                           light / dark mode automatically).
    //     "background"  int     ARGB behind the modules + quiet zone.
    //                           0 (default) = transparent (the widget /
    //                           layers below show through).
    //     "ecc"         int     error-correction level, neui_qr_ecc_t
    //                           (0=LOW..3=HIGH). Default 1 (MEDIUM).
    //     "quiet_zone"  int     light-margin width in modules around the
    //                           symbol. Default 4 (the QR spec minimum).
    //                           Clamped to [0, 16].
    //     The symbol is rasterised once into an internally-held bitmap
    //     (regenerated only when the resolved text / size / colours / ecc
    //     change) and blitted at native resolution, centred + letterboxed
    //     to stay square and crisp within the layer rect.
    //   rect layer props:
    //     "fill_color"    int   ARGB; 0 = no fill (alpha 0)
    //     "stroke_color"  int   ARGB; 0 = no stroke (alpha 0)
    //     "stroke_width"  float px; 0 (default) = no stroke
    //     "corner_radius" float px; 0 (default) = sharp corners. Clamped to
    //                              min(width, height) / 2 at paint time.
    //     A gradient fill (overriding fill_color) is set via set_gradient
    //     (below); the stroke stays solid. Honours corner_radius.
    //   path layer props:
    //     "fill_color"    int   ARGB; 0 = no fill. Same semantics as rect.
    //     "stroke_color"  int   ARGB; 0 = no stroke. Same semantics as rect.
    //     "stroke_width"  float px; 0 (default) = no stroke. Same as rect.
    //     The path geometry itself is assigned via set_path (below); it
    //     replaces any previous geometry on the layer. A gradient fill is
    //     available via set_gradient, same as rect.
    //   any layer:
    //     "offset_x"  int     px, relative to anchor
    //     "offset_y"  int     px, relative to anchor
    //     "width"     int     px (NEUI_COMPOUND_FILL = match widget width)
    //     "height"    int     px (NEUI_COMPOUND_FILL = match widget height)
    //     "alpha"     float   0..1 opacity; 0 short-circuits the layer
    //     "show_when" int     bitmask of NEUI_LAYER_STATE_* flags; 0 (default)
    //                         = visible always; non-zero = visible only when
    //                         (current_state & show_when) != 0.

    void (NEUI_ABI *set_int)   (neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer,
                                  const char* prop, int value);
    void (NEUI_ABI *set_float) (neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer,
                                  const char* prop, float value);
    void (NEUI_ABI *set_string)(neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer,
                                  const char* prop, const char* value);
    void (NEUI_ABI *set_asset) (neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer,
                                  const char* prop, neui_asset_t value);

    // -------- Dynamic bindings ---------------------------------------------

    // Bind a numeric property (int- or float-typed) to the widget's
    // attribute `attr_key`. At paint time the attribute is read as float
    // (int attrs are promoted; string / missing attrs yield 0.0), then
    //   effective = scale * x + offset
    // is computed and applied to the property. Int-typed properties
    // (offset_x, width, color, align_*) are rounded to nearest.
    //
    // Replaces any previous static value or binding on the same prop.
    void (NEUI_ABI *bind)(neui_session_t session, neui_asset_t asset,
                           neui_compound_layer_t layer,
                           const char* prop, const char* attr_key,
                           float scale, float offset);

    // Bind an asset-handle property (e.g. an asset layer's "asset" prop)
    // to a widget attribute. The attribute must hold an asset handle
    // (stored as int attr containing the handle's .id - the framework
    // round-trips this on its own).
    void (NEUI_ABI *bind_asset)(neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer,
                                  const char* prop, const char* attr_key);

    // Remove any binding on `prop`. The current static value (if any)
    // is retained.
    void (NEUI_ABI *unbind)(neui_session_t session, neui_asset_t asset,
                              neui_compound_layer_t layer,
                              const char* prop);

    // Assign the layer's path geometry from a flat command array. Only
    // meaningful on NEUI_COMPOUND_LAYER_PATH layers - no-op on other
    // kinds. Replaces any previously-assigned path. Passing NULL or
    // count == 0 clears the path.
    //
    // The painter wraps replay in push_transform + translate(rect.x,
    // rect.y), so command coordinates are layer-local (0, 0 = layer
    // rect's top-left).
    void (NEUI_ABI *set_path)(neui_session_t session, neui_asset_t asset,
                                neui_compound_layer_t layer,
                                const neui_path_cmd_t* cmds, uint32_t count);

    // Set (or clear) a gradient fill on a RECT or PATH layer. When `grad`
    // is non-NULL and carries >= 2 stops, the layer's FILL is painted with
    // that gradient instead of the solid "fill_color" (the stroke is
    // unaffected, and corner_radius / path geometry still apply). Passing
    // NULL, or a gradient with fewer than two stops, clears it so the layer
    // reverts to the solid fill_color. No-op on other layer kinds.
    //
    // Coordinate convention (differs from the render/painter gradient API,
    // which is absolute): inside a compound layer the gradient geometry is
    // NORMALISED to the resolved layer rect. start/end/focal x,y are [0,1]
    // fractions of the rect (0,0 = top-left, 1,1 = bottom-right) and
    // `radius` (radial) is a fraction of the rect's larger dimension, so
    // the gradient scales with the layer regardless of widget size. The
    // stop array is copied; the caller need not keep it alive.
    void (NEUI_ABI *set_gradient)(neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer,
                                  const neui_gradient_t* grad);
  } neui_compound_api_t;

#ifdef __cplusplus
}
#endif

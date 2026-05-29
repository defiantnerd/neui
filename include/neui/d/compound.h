#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "assets.h"

// Compound drawable API - reached via
//   neui_compound_api_t* co = (neui_compound_api_t*)
//     neui_api->get_interface(session, NEUI_API_COMPOUND);
// and operates on neui_asset_t handles previously returned from
// neui_asset_api_t::create_compound.
//
// A compound is a mutable layer stack:
//   - each layer has a kind (text or asset for v1),
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
// is painted in every state. A non-zero mask gates the layer behind the
// widget's current state: paint only if (current_state & show_when) != 0.
//
// The current_state composed each paint always contains exactly one of
// ENABLED / DISABLED (mirrors widgets->get_enabled), and optionally
// HOVERED (mouse cursor inside the widget rect) and / or PRESSED (mouse
// button was pressed on the widget and not yet released - capture-style,
// stays set when the cursor leaves while held).
//
// Reacts to internal hover / press / enabled detection only; independent
// of any client event emission.
#define NEUI_LAYER_STATE_ENABLED  (1u << 0)
#define NEUI_LAYER_STATE_DISABLED (1u << 1)
#define NEUI_LAYER_STATE_HOVERED  (1u << 2)
#define NEUI_LAYER_STATE_PRESSED  (1u << 3)

// Prop name for the state filter. Set via set_int. Default value 0
// means "visible in every state".
//
//   compound->set_int(sess, asset, layer, NEUI_PROP_SHOW_WHEN,
//                     NEUI_LAYER_STATE_HOVERED | NEUI_LAYER_STATE_PRESSED);
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
  } neui_compound_layer_kind_t;

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
    //     "asset"     asset   bitmap handle (v1)
    //     "rotation"  float   radians, clockwise
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
  } neui_compound_api_t;

#ifdef __cplusplus
}
#endif

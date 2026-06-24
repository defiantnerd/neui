#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "assets.h"

// Filter API - reached via
//   neui_filter_api_t* fi = (neui_filter_api_t*)
//     neui_api->get_interface(session, NEUI_API_FILTER);
// and operates on neui_asset_t handles previously returned from
// neui_asset_api_t::create_filter.
//
// A filter is the SVG <filter> model: a mutable, ordered graph of atomic
// fe* primitives. Each primitive reads one or two named inputs, computes a
// pixel result, and optionally registers it under a result name for a later
// primitive to reference. The built-in inputs "SourceGraphic" and
// "SourceAlpha" seed the graph; the final primitive's output is the filter
// result.
//
// A filter is APPLIED to a render SURFACE (NEUI_ASSET_KIND_SURFACE) via
// neui_asset_api_t::apply_filter, which evaluates the graph in place over the
// surface's pixels. This matches the "bake once" model: paint the static art
// into a surface, build a named filter once, apply it after each paint. A
// filter is NOT attached to a widget (unlike a compound / behavior).
//
// Distances (offset dx/dy, blur std-dev, subregion) are in LOGICAL px and
// scale with the surface's backing scale. Colour math is sRGB (matching the
// common SVG `color-interpolation-filters="sRGB"`); linearRGB is deferred.
//
// fe* -> primitive-kind mapping and recognised props are documented on
// neui_filter_prim_kind_t and the property setters below.

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_FILTER "com.defiantnerd.neui.extension.filter/0"

// Built-in input source names (any other string names a prior primitive's
// result). SourceAlpha is the source's alpha as premultiplied black.
#define NEUI_FILTER_SRC_GRAPHIC "SourceGraphic"
#define NEUI_FILTER_SRC_ALPHA   "SourceAlpha"

  // Primitive handle. Internally (asset_slot << 16) | prim_slot - same layout
  // as neui_compound_layer_t / neui_behavior_handler_t. Slot-reused on
  // remove_primitive.
  typedef struct neui_filter_prim {
    uint32_t id;
  } neui_filter_prim_t;

  static const neui_filter_prim_t filter_prim_none = { UINT32_MAX };

  // Atomic SVG filter primitives. NONE first (sentinel).
  typedef enum neui_filter_prim_kind {
    NEUI_FE_NONE          = 0,
    NEUI_FE_FLOOD         = 1,  // feFlood
    NEUI_FE_COLOR_MATRIX  = 2,  // feColorMatrix
    NEUI_FE_OFFSET        = 3,  // feOffset
    NEUI_FE_GAUSSIAN_BLUR = 4,  // feGaussianBlur
    NEUI_FE_COMPOSITE     = 5,  // feComposite
    NEUI_FE_BLEND         = 6,  // feBlend
    NEUI_FE_MERGE         = 7,  // feMerge
  } neui_filter_prim_kind_t;

  typedef struct neui_filter_api {
    uint32_t neui_version;

    // -------- Primitive management -----------------------------------------

    // Append a primitive to the filter graph. Returns filter_prim_none on bad
    // args or a non-filter asset.
    neui_filter_prim_t (NEUI_ABI *add_primitive)(neui_session_t session,
                                                 neui_asset_t asset,
                                                 neui_filter_prim_kind_t kind);
    void (NEUI_ABI *remove_primitive)(neui_session_t session, neui_asset_t asset,
                                      neui_filter_prim_t prim);
    void (NEUI_ABI *clear)(neui_session_t session, neui_asset_t asset);

    // -------- Graph wiring --------------------------------------------------

    // Set primitive input `slot` (0 = `in`, 1 = `in2`) to a source name:
    // "SourceGraphic", "SourceAlpha", or a prior primitive's result name.
    // An empty / unset `in` defaults to SourceGraphic for the first primitive
    // and the previous primitive's result otherwise; an unknown name falls
    // back to that same default.
    void (NEUI_ABI *set_input)(neui_session_t session, neui_asset_t asset,
                               neui_filter_prim_t prim, int slot,
                               const char* source);

    // Register this primitive's output under `name` so a later primitive can
    // reference it via set_input. NULL / "" clears the name.
    void (NEUI_ABI *set_result)(neui_session_t session, neui_asset_t asset,
                                neui_filter_prim_t prim, const char* name);

    // Optional primitive subregion (logical px), a hard clip on the
    // primitive's output (and the fill area for feFlood). Unset = the whole
    // surface. Pass w <= 0 or h <= 0 to clear.
    void (NEUI_ABI *set_region)(neui_session_t session, neui_asset_t asset,
                                neui_filter_prim_t prim,
                                float x, float y, float w, float h);

    // -------- Property setters ---------------------------------------------
    //
    // Recognised props per kind (unknown names are stored but inert):
    //
    //   feFlood:
    //     "flood_color"    int    ARGB fill colour (default opaque black).
    //     "flood_opacity"  float  0..1 multiplied into the colour's alpha
    //                             (default 1).
    //   feColorMatrix:
    //     "type"           string "matrix" (default) / "saturate" /
    //                             "hueRotate" / "luminanceToAlpha".
    //     "values"         floats (set_floats): 20 for "matrix" (row-major
    //                             4x5, channels normalised 0..1), 1 for
    //                             "saturate" (0..1) and "hueRotate" (degrees),
    //                             0 for "luminanceToAlpha".
    //   feOffset:
    //     "dx" / "dy"      float  shift in logical px (positive = right/down).
    //   feGaussianBlur:
    //     "std_dev"        float  sets both axes; or
    //     "std_dev_x" / "std_dev_y" float  per-axis std-deviation (px).
    //   feComposite:
    //     "operator"       string "over" (default) / "in" / "out" / "atop" /
    //                             "xor" / "arithmetic". Reads `in` (src) and
    //                             `in2` (dst).
    //     "k1".."k4"       float  arithmetic coefficients
    //                             (out = k1*i1*i2 + k2*i1 + k3*i2 + k4).
    //   feBlend:
    //     "mode"           string "normal" (default) / "multiply" / "screen" /
    //                             "darken" / "lighten". Reads `in` + `in2`.
    //   feMerge:
    //     (no props) - stack the merge_add_input sources bottom-to-top.

    void (NEUI_ABI *set_int)   (neui_session_t session, neui_asset_t asset,
                                neui_filter_prim_t prim,
                                const char* prop, int value);
    void (NEUI_ABI *set_float) (neui_session_t session, neui_asset_t asset,
                                neui_filter_prim_t prim,
                                const char* prop, float value);
    void (NEUI_ABI *set_string)(neui_session_t session, neui_asset_t asset,
                                neui_filter_prim_t prim,
                                const char* prop, const char* value);
    // Array-valued prop (the feColorMatrix "values" list). Copies `count`
    // floats; count 0 / data NULL clears.
    void (NEUI_ABI *set_floats)(neui_session_t session, neui_asset_t asset,
                                neui_filter_prim_t prim,
                                const char* prop,
                                const float* values, uint32_t count);

    // Append an input source to a feMerge primitive (ordered bottom-to-top:
    // the first added is the lowest layer). No-op on non-merge primitives.
    void (NEUI_ABI *merge_add_input)(neui_session_t session, neui_asset_t asset,
                                     neui_filter_prim_t prim,
                                     const char* source);
  } neui_filter_api_t;

#ifdef __cplusplus
}
#endif

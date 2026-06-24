#pragma once
#include <stdint.h>
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Colour-gradient description shared by the render backend
// (neui_render_backend_t::fill_rect_gradient / fill_path_gradient) and the
// client painter (neui_painter_api_t with the same names). A gradient is an
// immediate-mode brush: it is described by value at the fill call site, used
// once, and not retained - there is no handle to create / destroy.
//
// All coordinates are logical pixels at 96 DPI, in the SAME coordinate space
// as the rect / path being filled, so the gradient follows the active
// transform stack. Colours are 0xAARRGGBB; each stop's alpha byte is further
// multiplied by the current alpha-stack top, exactly like the solid fills.

// A single colour stop. `offset` is the normalised position along the
// gradient axis in [0, 1] (0 = start / centre, 1 = end / edge); the backend
// clamps it into range. `argb` is the colour at that position.
typedef struct neui_gradient_stop {
  float    offset;
  uint32_t argb;
} neui_gradient_stop_t;

// Gradient geometry kind.
typedef enum neui_gradient_kind {
  NEUI_GRADIENT_LINEAR = 0,  // colour interpolates along an axis
  NEUI_GRADIENT_RADIAL = 1,  // colour interpolates centre -> circle edge
} neui_gradient_kind_t;

// How the pattern behaves OUTSIDE the [0, 1] stop range along its axis.
// CLAMP is the universal default; the Cairo and D2D backends honour all
// three, while the CoreGraphics backend supports CLAMP only and treats
// REPEAT / MIRROR as CLAMP.
typedef enum neui_gradient_extend {
  NEUI_GRADIENT_EXTEND_CLAMP  = 0,  // hold the end stops (default)
  NEUI_GRADIENT_EXTEND_REPEAT = 1,  // tile the pattern
  NEUI_GRADIENT_EXTEND_MIRROR = 2,  // reflect every other tile
} neui_gradient_extend_t;

// A gradient brush passed by pointer to the gradient fill entry points.
//
// LINEAR: the axis runs from (start_x, start_y) to (end_x, end_y). The 0.0
//   stop sits at the start point, the 1.0 stop at the end; colour is
//   constant along lines perpendicular to the axis. `radius` is unused.
//
// RADIAL: concentric rings centred at (start_x, start_y). The 0.0 stop sits
//   at the centre, the 1.0 stop on the circle of the given `radius`.
//   (end_x, end_y) is the focal point the inner stop radiates from - set it
//   equal to the centre (the common case) for a plain concentric radial;
//   offsetting it yields an off-centre "spotlight" highlight.
//
// `stops` must point at `stop_count` (>= 2) entries in ascending offset
// order. Fewer than two stops, a null `stops`, or a null gradient draws
// nothing.
typedef struct neui_gradient {
  neui_gradient_kind_t        kind;
  const neui_gradient_stop_t* stops;
  uint32_t                    stop_count;
  neui_gradient_extend_t      extend;
  float                       start_x, start_y;
  float                       end_x,   end_y;
  float                       radius;   // RADIAL only
} neui_gradient_t;

#ifdef __cplusplus
}
#endif

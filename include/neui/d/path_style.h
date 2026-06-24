#pragma once
#include <stdint.h>
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Path fill-rule + stroke-style description, shared by the render backend
// (neui_render_backend_t) and the client painter (neui_painter_api_t with the
// same entry points). Like the gradient brush, a stroke style is an immediate-
// mode value passed by pointer at the stroke call site - there is no handle to
// create / destroy.
//
// All lengths are logical pixels at 96 DPI.

// How a filled path resolves self-overlap / sub-path nesting. NONZERO is the
// default (matches the bare fill_path); EVENODD is the SVG even-odd rule.
typedef enum neui_fill_rule {
  NEUI_FILL_RULE_NONZERO = 0,
  NEUI_FILL_RULE_EVENODD = 1,
} neui_fill_rule_t;

// Stroke end-cap shape (SVG stroke-linecap).
typedef enum neui_line_cap {
  NEUI_LINE_CAP_BUTT   = 0,  // flush square end (default)
  NEUI_LINE_CAP_ROUND  = 1,  // semicircle past the end
  NEUI_LINE_CAP_SQUARE = 2,  // square half-width past the end
} neui_line_cap_t;

// Stroke corner shape (SVG stroke-linejoin).
typedef enum neui_line_join {
  NEUI_LINE_JOIN_MITER = 0,  // sharp corner, clamped by miter_limit (default)
  NEUI_LINE_JOIN_ROUND = 1,  // rounded corner
  NEUI_LINE_JOIN_BEVEL = 2,  // flattened corner
} neui_line_join_t;

// Stroke style passed by pointer to stroke_path_styled. A NULL style (or
// passing this all-zero) yields the plain stroke_path defaults: butt cap,
// miter join, solid line. `dash_array` lists on/off run lengths in logical
// px (NULL / dash_count 0 = solid); `dash_offset` shifts the dash pattern
// start. `miter_limit <= 0` uses the SVG default of 4.
typedef struct neui_stroke_style {
  neui_line_cap_t  cap;
  neui_line_join_t join;
  float            miter_limit;
  const float*     dash_array;
  uint32_t         dash_count;
  float            dash_offset;
} neui_stroke_style_t;

#ifdef __cplusplus
}
#endif

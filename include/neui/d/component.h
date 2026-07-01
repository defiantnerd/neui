#pragma once

#include "assets.h"
#include "widgets.h"

// Component format - the declarative, JSON-authored definition of a composited
// control (knob, fader, ...). A COMPONENT asset (NEUI_ASSET_KIND_COMPONENT)
// bundles a COMPOUND (visual layer stack) + a BEHAVIOR (input handlers) +
// default attrs + a parameter manifest + a default size. Load it once and
// instantiate it many times; the compound + behavior are shared across every
// instance (shape shared, AttrBag per-widget), exactly like building them by
// hand - but in one line instead of ~45.
//
// "component" vs "compound": a *component* bundles a *compound* plus a
// *behavior* plus defaults. They are different asset kinds; don't confuse them.
//
// This header is the documentation umbrella. The actual types live where the
// APIs that use them are declared, to avoid a circular include:
//   - neui_component_env_t, neui_component_param_t, NEUI_ASSET_KIND_COMPONENT,
//     create_component_from_file / _from_string, component_param_count / _at
//       -> <neui/d/assets.h>  (neui_asset_api_t)
//   - create_from_component
//       -> <neui/d/widgets.h> (neui_widget_api_t)
//
// Typical use:
//   neui_asset_api_t*  assets  = get_interface(sess, NEUI_API_ASSETS);
//   neui_widget_api_t* widgets = get_interface(sess, NEUI_API_WIDGETS);
//   neui_attr_api_t*   attrs   = get_interface(sess, NEUI_API_ATTRS);
//
//   neui_asset_t knob = assets->create_component_from_file(sess, "components/knob.json", NULL);
//
//   // one-call instantiate (size from the component default if w/h <= 0):
//   neui_widget_t w = widgets->create_from_component(sess, parent, knob, x, y, 0, 0);
//   attrs->set_string(sess, w, "name", "Cutoff");
//   attrs->set_float (sess, w, NEUI_PARAM_VALUE, 0.5f);
//
//   // or attach to an existing CUSTOMDRAW (set_asset is COMPONENT-aware):
//   neui_widget_t cw = widgets->create(sess, parent, NEUI_W_CUSTOMDRAW, x, y, 110, 110, NULL);
//   widgets->set_asset(sess, cw, knob);
//
//   assets->destroy(sess, knob);   // releases the owned compound + behavior
//
// JSON SCHEMA (every field maps 1:1 onto an existing compound-layer / behavior-
// handler property). Parsed by the in-host neui::mujson, which is JSON-like:
// bare keys, a single trailing comma, and `//` line + `/* */` block comments
// are tolerated.
//
//   {
//     "component": "knob",            // logical name (informational)
//     "size": [110, 110],             // default instance w, h (logical px)
//
//     // default attrs stamped on every instance + the param manifest:
//     "params": [
//       { "key": "neui.param.value", "default": 0.5, "min": 0, "max": 1, "label": "Value" }
//     ],
//
//     // logical asset names -> resolved via neui_component_env_t (path or callback):
//     "assets": { "bg": "knob_bg.png", "indicator": "knob_move.png" },
//
//     // compound layer stack (kind = asset | text | rect | path):
//     "layers": [
//       { "kind": "asset", "z": 0, "anchor": ["center","center"], "size": "fill", "asset": "bg" },
//       { "kind": "asset", "z": 1, "anchor": ["center","center"], "size": [70,70], "offset": [0,-15],
//         "asset": "indicator",
//         "bind": { "rotation": { "attr": "neui.param.value", "scale": 4.71238898, "offset": 0 } } },
//       { "kind": "text",  "z": 2, "anchor": ["bottom","bottom"], "size": ["fill", 22],
//         "text": "{name}: {value}", "font_size": 14, "align": ["center","center"], "weight": 400 },
//       { "kind": "rect",  "z": -1, "anchor": ["top_left","top_left"], "size": "fill",
//         "fill_color": "#33336699", "stroke_color": "#FF6699CC", "stroke_width": 1.5, "corner_radius": 14 },
//       { "kind": "path",  "z": 3, "anchor": ["bottom_right","bottom_right"], "size": [20,20],
//         "offset": [-6,-6], "fill_color": "#FFFFFFFF",
//         "path": [ {"m":[4,3]}, {"l":[15,10]}, {"l":[4,17]}, {"l":[9,10]}, {"z":true} ] }
//     ],
//
//     // behavior handler list (kind -> NEUI_BEHAVIOR_KIND_*):
//     "behavior": [
//       { "kind": "drag_rotational", "target": "neui.param.value", "min": 0, "max": 1, "deadzone": 4 },
//       { "kind": "wheel",    "step": 0.02 },
//       { "kind": "key_step", "step": 0.01, "coarse": 0.10 },
//       { "kind": "context_reset", "target_default": "neui.param.default" }
//     ]
//   }
//
// Field notes:
//   - anchors: the 9 tokens top_left|top|top_right|left|center|right|
//     bottom_left|bottom|bottom_right.
//   - "fill" on a size axis -> NEUI_COMPOUND_FILL (-1). "size" may be the
//     string "fill" (both axes) or a 2-array of ("fill" | number) per axis.
//   - colors: "#AARRGGBB" (string) or a bare int -> ARGB.
//   - "show_when": array of state tokens, "!" negates: ["hovered","!pressed"]
//     -> NEUI_LAYER_STATE_* bitmask. Tokens: enabled / hovered / pressed /
//     selected (selected reads the NEUI_ATTR_SELECTED attribute).
//   - "align": [x, y] with x in {left,center,right} and y in {top,center,bottom}.
//   - "bind": map of numeric prop -> { attr, scale, offset } (compound->bind);
//     a "bind" on the "asset" prop is treated as bind_asset (attr only).
//   - "path": array of one-key objects {"m":[x,y]} | {"l":[x,y]} |
//     {"a":[cx,cy,r,a0,a1]} | {"z":true} -> MOVE_TO / LINE_TO / ARC / CLOSE.
//   - behavior props are the documented handler props in <neui/d/behavior.h>.

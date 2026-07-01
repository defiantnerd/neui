# Component widgets: how attrbag, layers, behaviors and bindings fit together

A practical "how to" for building composited controls (knobs, faders, meters,
custom buttons) on a `NEUI_W_CUSTOMDRAW` widget. It explains the four moving
parts and how they talk to each other, then lists the common attributes and the
layer / behavior kinds with their per-kind properties.

For the formal C declarations see `<neui/d/compound.h>`, `<neui/d/behavior.h>`,
`<neui/d/assets.h>` and `<neui/d/component.h>`. For the "why" behind the design
see `docs/design-notes.md`.

---

## The mental model

A painted control is built from four things:

```
            +-------------------------------------------------+
            |               CUSTOMDRAW widget                 |
            |                                                 |
            |   AttrBag (per-widget data)                     |
            |     neui.param.value = 0.5                      |
            |     name             = "Cutoff"   <-- the state |
            |        ^                    |                    |
            |   write|                    |read               |
            |        |                    v                    |
            |   +---------+          +-----------+             |
            |   |BEHAVIOR |          | COMPOUND  |             |
            |   | (input) |          | (visual)  |             |
            |   | drag,   |          | layers +  |             |
            |   | wheel,  |          | bindings  |             |
            |   | keys    |          |           |             |
            |   +---------+          +-----------+             |
            +-------------------------------------------------+
```

1. **The AttrBag is the single source of truth.** Every CUSTOMDRAW widget owns
   a string-keyed attribute bag (see the Attribute API in `CLAUDE.md`). The
   control's *state* (a knob's value, a label's text, a toggle's on/off) lives
   here as named float / int / string attrs. Nothing else stores it.

2. **A BEHAVIOR asset is the input side.** It is a list of handlers (drag,
   wheel, key-step, click, context-reset, drag-source). When the user
   interacts, the matching handler *writes* a value into a named attr in the
   AttrBag (its `target`, default `neui.param.value`).

3. **A COMPOUND asset is the visual side.** It is a stack of layers (text,
   asset/bitmap, rect, path, qr). Each layer *reads* from the AttrBag through
   bindings and templates, and paints accordingly.

4. **Bindings + templates are the wires.** A binding makes a numeric layer
   property track an attr (`effective = scale * attr + offset`). A text
   template (`"{name}: {value}"`) substitutes attr values into a string. This
   is how a write on the input side becomes a redraw on the visual side.

The key idea: **behavior and compound never talk to each other directly.** They
only share the AttrBag. The behavior writes an attr; the framework invalidates
the widget; the compound reads the attr on the next paint. This loose coupling
is why one compound and one behavior asset can be shared across many widget
instances - the shape is shared, the per-widget AttrBag is not.

### The data-flow loop

```
user drags  ->  BEHAVIOR handler writes neui.param.value = 0.62
            ->  framework fires ATTR_CHANGED + invalidates the widget
            ->  next paint: COMPOUND layer bound to neui.param.value
                recomputes rotation = 4.712 * 0.62 - 2.356  ->  redraws
```

A programmatic `attrs->set_float(sess, w, NEUI_PARAM_VALUE, 0.62f)` enters the
same loop at step 2 (it invalidates and repaints), so code and user input drive
the visual identically. The only difference: a user-driven write fires
`NEUI_EVENT_ATTR_CHANGED`; a programmatic `attrs->set_*` stays silent.

---

## Three ways to build one

### 1. Imperative

Create the assets, add layers / handlers, set props and bindings, attach both
to a CUSTOMDRAW widget. The two slots are independent and compose:

```cpp
neui_asset_t   comp = assets->create_compound(sess);
neui_asset_t   beh  = assets->create_behavior(sess);

// visual: a value-tracking rotating indicator + a label
auto disc = compound->add_layer(sess, comp, NEUI_COMPOUND_LAYER_RECT, -1);
compound->set_anchor(sess, comp, disc, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
compound->set_int  (sess, comp, disc, "fill_color", 0xFF2A2E38);

auto ind = compound->add_layer(sess, comp, NEUI_COMPOUND_LAYER_ASSET, 0);
compound->set_asset(sess, comp, ind, "asset", indicator_bitmap);
compound->bind     (sess, comp, ind, "rotation", NEUI_PARAM_VALUE, 4.712389f, -2.356194f);

auto lbl = compound->add_layer(sess, comp, NEUI_COMPOUND_LAYER_TEXT, 1);
compound->set_string(sess, comp, lbl, "text", "{name}");

// input: rotary drag writes neui.param.value
auto h = behavior->add_handler(sess, beh, NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL);
behavior->set_string(sess, beh, h, "target", NEUI_PARAM_VALUE);
behavior->set_float (sess, beh, h, "min", 0.0f);
behavior->set_float (sess, beh, h, "max", 1.0f);

neui_widget_t w = widgets->create(sess, parent, NEUI_W_CUSTOMDRAW, x, y, 110, 110, NULL);
widgets->set_asset(sess, w, comp);   // kind-routed -> compound slot
widgets->set_asset(sess, w, beh);    // kind-routed -> behavior slot
attrs->set_string  (sess, w, "name", "Cutoff");
attrs->set_float   (sess, w, NEUI_PARAM_VALUE, 0.5f);
```

### 2. Declarative (component JSON)

The same definition in one JSON file - this is the recommended path for any
reusable control. A **component** asset bundles the compound + behavior +
default attrs + a param manifest + a default size. Load once, instantiate many:

```cpp
neui_asset_t  knob = assets->create_component_from_file(sess, "components/knob.json", NULL);
neui_widget_t w    = widgets->create_from_component(sess, parent, knob, x, y, 0, 0);
attrs->set_string(sess, w, "name", "Cutoff");
attrs->set_float (sess, w, NEUI_PARAM_VALUE, 0.5f);
```

See `examples/components/knob.json` for a complete, commented example and the
JSON schema in `<neui/d/component.h>`. Every JSON field maps 1:1 onto a
compound-layer or behavior-handler property documented below.

> **component vs compound:** a *component* bundles a *compound* (visual) plus a
> *behavior* (input) plus defaults. They are different asset kinds - do not
> confuse them.

### Which to use

- **Component JSON** for any control you instantiate more than once, want to
  hot-reload, or hand to a designer. The compound + behavior are shared across
  instances automatically.
- **Imperative** for one-off widgets or when the shape is computed at runtime.

Both end at the same place: a CUSTOMDRAW widget with a compound and/or behavior
attached, reading and writing one shared AttrBag.

---

## Attaching, sharing and invalidation

- `widgets->set_asset(sess, w, handle)` is **kind-routed**: a COMPOUND handle
  lands in the visual slot, a BEHAVIOR handle in the input slot, a COMPONENT
  handle fills both and stamps defaults. The two slots are independent - a
  widget can have a compound only, a behavior only, or both.
- When a compound is attached, the widget's `WIDGET_PAINT` event is
  **suppressed** - the layer stack does the drawing instead.
- **One asset, many widgets.** The same compound / behavior can back any number
  of widgets. Shape (layers, handlers, bindings) is shared; the AttrBag is
  per-widget, so each instance shows its own value.
- **Invalidation is automatic.** `attrs->set_*` on a widget carrying a compound
  invalidates it; mutating a compound (add/remove layer, set prop) invalidates
  every CUSTOMDRAW whose compound matches. State-filtered layers (`show_when`)
  also invalidate on hover / press / enabled transitions.
- **Lifetime.** A component's `destroy` releases its owned compound + behavior +
  path-loaded layer assets. Clear or destroy a widget before destroying an
  asset it still references (assets are not refcounted by widgets).

---

## Bindings and templates (the wires in detail)

**Numeric binding** - `bind(layer, prop, attr_key, scale, offset)`. At paint
time the attr is read as float (int attrs promoted; string / missing yield 0),
then `effective = scale * x + offset` is applied. Int-typed targets
(`offset_x`, `width`, `color`, `align_*`) round to nearest. A binding replaces
any static value on that prop. Example: map a 0..1 value onto a -135deg..+135deg
sweep with `scale = 3*pi/2 = 4.712389`, `offset = -3*pi/4 = -2.356194`.

**Asset binding** - `bind_asset(layer, prop, attr_key)`. The attr holds an asset
handle's `.id` as an int; the framework round-trips it. Useful for swapping a
bitmap from an attr.

**Text template** - the `text` prop on a text (or qr) layer is a template.
`{key}` is replaced by attr `key` rendered as text; unknown keys yield empty
string; `{{` / `}}` are literal braces. `{key:.2f}` format specs are reserved
(deferred). Templates are pre-parsed at `set_string`.

**Unbind** - `unbind(layer, prop)` drops the binding and keeps the last static
value.

---

## Common attributes

These are the well-known attr keys most relevant to component widgets. The
param keys are the conventional `target` / read keys for behaviors and bindings;
the others tune painted-control behavior. Full table of all attrs is in
`CLAUDE.md`. Macros are in `<neui/d/attrs.h>`.

| Macro | Key string | Type | Role |
|---|---|---|---|
| `NEUI_PARAM_VALUE` | `neui.param.value` | float | Default `target` for behaviors and the value most bindings read. The control's primary value, conventionally 0..1. |
| `NEUI_PARAM_DEFAULT` | `neui.param.default` | float | Default reset value read by `CONTEXT_RESET` (right-click "Reset to default"). |
| `NEUI_ATTR_STEPS` | `neui.attr.steps` | int | Default `snap_attr`. `>=2` snaps the value to N discrete positions; `<2` continuous. |
| `NEUI_ATTR_BACKGROUND` | `neui.attr.background` | int ARGB | Background fill for painted widgets. Honoured unconditionally. |
| `NEUI_ATTR_QRCODE` | `neui.attr.qrcode` | string | When non-empty, overrides a QR layer's `text` template and is encoded verbatim. |
| `NEUI_ATTR_FONT_FAMILY` | `neui.attr.font_family` | string | Family name for text-bearing widgets. Empty = host default. |
| `NEUI_ATTR_FONT_SIZE` | `neui.attr.font_size` | float | Font size in logical px. |
| `NEUI_ATTR_FONT_WEIGHT` | `neui.attr.font_weight` | int | CSS weight 100..900 (400 Normal, 700 Bold). |

Plus any **custom keys you invent** - e.g. `"name"` in the knob example is just
an app-defined string attr the text layer reads via `{name}`. Use your own
namespace; `neui.*` is reserved.

> A new `NEUI_ATTR_*` / `NEUI_PARAM_*` macro needs a matching row in
> `k_well_known_attrs` (`hosts/shared/attrs.h`).

---

## Layer types

A compound layer is positioned by a 9-point anchor pair (a point on the parent /
widget rect aligned to a point on the layer rect), plus `offset_x` / `offset_y`,
plus `width` / `height` (`NEUI_COMPOUND_FILL = -1` spans that axis), plus a
signed `z` (z<0 below the widget's children, z>=0 above; ties by insertion
order). Macros / enums in `<neui/d/compound.h>`.

### Properties common to every layer

| Prop | Type | Notes |
|---|---|---|
| `offset_x` / `offset_y` | int | px relative to the resolved anchor. |
| `width` / `height` | int | px; `NEUI_COMPOUND_FILL` (-1) = match widget on that axis. |
| `alpha` | float | 0..1 opacity; 0 short-circuits the layer. |
| `show_when` | int | `NEUI_LAYER_STATE_*` bitmask. 0 = always visible. AND filter over enabled / hovered / pressed / selected, each with a positive bit and a `_NOT_*` bit. `selected` reads `NEUI_ATTR_SELECTED` (client/behavior-driven). Rule: visible iff `(show_when & ~current_state) == 0`. |

### Per-kind properties

| Kind (`neui_compound_layer_kind_t`) | Special props |
|---|---|
| **TEXT** (`NEUI_COMPOUND_LAYER_TEXT`) | `text` (string template), `size` (float px), `color` (int ARGB, optional - defaults to theme `text_primary`, tracks light/dark), `align_x` (0 left / 1 center / 2 right), `align_y` (0 top / 1 center / 2 bottom). |
| **ASSET** (`NEUI_COMPOUND_LAYER_ASSET`) | `asset` (bitmap / surface / filmstrip handle), `frame` (int filmstrip cell, default 0; commonly bound to a value), `rotation` (float radians, clockwise), `tint` (int ARGB multiplicative; default `0xFFFFFFFF` = passthrough, any other runs the tint primitive; `0` short-circuits). |
| **RECT** (`NEUI_COMPOUND_LAYER_RECT`) | `fill_color` (int ARGB, 0 = no fill), `stroke_color` (int ARGB, 0 = no stroke), `stroke_width` (float px, 0 = none), `corner_radius` (float px, 0 = sharp; clamped to `min(w,h)/2`). A circle is a rect with `corner_radius = w/2`. |
| **PATH** (`NEUI_COMPOUND_LAYER_PATH`) | `fill_color`, `stroke_color`, `stroke_width` (same as rect). Geometry assigned via `set_path(cmds, count)` - `MOVE_TO` / `LINE_TO` / `ARC` / `CLOSE`, coords layer-local (0,0 = layer top-left). No bezier in v1. |
| **QR** (`NEUI_COMPOUND_LAYER_QR`) | `text` (string template, default `"{value}"`; overridden by the `neui.attr.qrcode` attr), `fill_color` (int ARGB dark modules, 0 = theme `text_primary`), `background` (int ARGB, 0 = transparent), `ecc` (`neui_qr_ecc_t` 0=LOW..3=HIGH, default MEDIUM), `quiet_zone` (int modules, default 4, clamped [0,16]). |

---

## Behavior handler types

A behavior asset is a list of handlers run in insertion order. Each handler
matches certain events and writes its `target` attr. Hit region per handler uses
the same 9-point anchor system (default: whole widget). Macros / enums in
`<neui/d/behavior.h>`.

### Properties common to most handlers

| Prop | Type | Default | Notes |
|---|---|---|---|
| `target` | string | `neui.param.value` | Attr key the handler writes. |
| `min` / `max` | float | 0.0 / 1.0 | Value bounds. |
| `step` | float | 0.01 | Nudge per wheel notch / arrow key. |
| `coarse` | float | 0.10 | Nudge per PageUp / PageDown. |
| `snap_attr` | string | `neui.attr.steps` | Attr holding the discrete-step count; `<2` = continuous. |
| `fine_modifier` | string | `shift` | `shift` / `ctrl` / `alt` / `none` - scales motion by `fine_scale`. |
| `fine_scale` | float | 0.2 | Multiplier under the fine modifier. |
| `cursor` | string | - | Advisory hint, stored but not applied in v1. |

### Per-kind handlers and their special props

| Kind (`neui_behavior_kind_t`) | Behavior + special props |
|---|---|
| **DRAG_VERTICAL** (`1`) | `dy / sweep` added to target; up = increase. `sweep` (px for a full 0..1 sweep, default 200). |
| **DRAG_HORIZONTAL** (`2`) | `dx / sweep` added; right = increase. `sweep`. |
| **DRAG_ROTATIONAL** (`3`) | Cursor angle from widget centre (1.5*pi sweep). `deadzone` (centre dead-zone radius px, default 4). |
| **DRAG_BIAXIAL** (`4`) | `dx` writes `target`, `dy` writes `target_y`. `sweep` (x-axis), `sweep_y` (y-axis). |
| **WHEEL** (`5`) | `delta * step` added to target. |
| **KEY_STEP** (`6`) | Arrow keys (`step`), Page (`coarse`), Home/End (`min`/`max`). Requires widget focus. |
| **CLICK_TOGGLE** (`7`) | Left-click flips target between `min` and `max`. |
| **CLICK_CYCLE** (`8`) | Left-click steps to next snap position (modulo the `steps` count). `wrap` (int: 0 = clamp at max, 1 = wrap to min). |
| **CLICK_SELECT** (`11`) | Left-click TOGGLES selection: flips `target` between `min` (deselected) / `max` (selected) AND mirrors the on/off state into `selected_attr` as int (1/0). `selected_attr` (default `neui.attr.selected` - the key the compound `show_when` SELECTED axis reads; `""` = skip the mirror). Float drives bindings, bool drives layer visibility. No group exclusivity. |
| **CONTEXT_RESET** (`9`) | Right-click "Reset to default" popup. `target_default` (attr key for the reset value, default `neui.param.default`). |
| **DRAG_SOURCE** (`10`) | Arms on mouse-down in the hit region; past `threshold_px` (default 4) calls `begin_drag`. `allowed_actions` (int `NEUI_DND_ACTION_*` bitmask, default COPY\|MOVE), `drag_data_key` (attr holding the data-item id), `drag_preview_key` (attr holding the preview-image asset id), `drag_hot_x` / `drag_hot_y` (int hot-spot px, -1 = centre), `result_attr` (attr the runtime writes with the negotiated action; fires ATTR_CHANGED). |

### Hit-region props (any handler)

| Prop | Type | Default | Notes |
|---|---|---|---|
| `anchor_parent` | int | `NEUI_ANCHOR_TOP_LEFT` | Anchor point on the widget rect. |
| `anchor_self` | int | `NEUI_ANCHOR_TOP_LEFT` | Anchor point on the handler rect. |
| `offset_x` / `offset_y` | int | 0 | px relative to anchor. |
| `width` / `height` | int | full widget | px; `NEUI_COMPOUND_FILL` (-1) = full widget. |

---

## Putting it together: the loop once more

1. Author the visual as a COMPOUND (layers + bindings + templates) and the
   input as a BEHAVIOR (handlers writing `neui.param.value`), or bundle both in
   a COMPONENT JSON.
2. Attach to a CUSTOMDRAW widget with `set_asset`; stamp per-instance attrs
   (value, name, ...).
3. The user interacts -> a behavior handler writes an attr -> the framework
   invalidates -> the next paint reads the attr through bindings / templates ->
   the visual updates. Programmatic `attrs->set_*` drives the same loop.

The AttrBag is the contract between input and visual. Get the attr keys right
and the two halves stay decoupled, shareable, and reloadable.

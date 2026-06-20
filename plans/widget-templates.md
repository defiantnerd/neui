# Declarative "Component" format for composited widgets (knobs etc.)

## Context

In plugin UIs a single control (a knob) is composed of repeated layers - background,
rotating indicator, value/name labels - plus interactive behavior (rotary drag, wheel,
key-step, right-click reset). neuilib already has the runtime: a **COMPOUND** asset is the
layer stack (text/asset/rect/path + 9-point anchors + z + bindings against a per-widget
AttrBag); a **BEHAVIOR** asset is the input side. One compound + one behavior attached to N
widgets serves N knobs that differ only by their attrs - so repetition is not duplicated at
runtime. The cost is **authoring**: one knob archetype is ~45 lines of imperative
`add_layer`/`set_anchor`/`bind`/`add_handler` calls (`examples/main.cpp:880-1128`).

A declarative JSON document fixes that and doubles as a designer -> developer handover
artifact. This asset is named a **component**: a reusable, declarative definition that
bundles a COMPOUND (visual) + a BEHAVIOR (input) + default params, loaded once and
instantiated many times. (Note the deliberate-but-close naming: a *component* bundles a
*compound* plus a *behavior*; they are different things.)

**Why this plan was rewritten.** An earlier draft proposed a *client-side helper library*
whose sole justification was keeping a client-vendored JSON parser out of the host
libraries. That premise is now void: the parser (`neui::mujson`) is compiled into the core
`neui` lib. With the parser already inside the host, "component" should be a **first-class
host capability** - parsed and materialized entirely inside the host, exposed through the
existing `assets` + `widgets` interface vtables - not a parallel client-side handle type
with its own slot table and a bespoke override-string parser. This removes the helper
library, the separate `neui_part_t` handle, and the `key=val` overrides mini-parser.

### Verified mujson API (`src/mujson.h`)

- Class `neui::mujson`, static methods. `static object_t parse(const std::string&)` /
  `parse(const char*)` -> top-level object; empty object on error, detail via
  `getLastError()`. `serialize(object_t, int indent=0)` available.
- `value_t = std::variant<std::monostate, bool, int, double, std::string, object_t, array_t>`;
  `struct node { value_t value; };` `object_t = std::vector<std::pair<std::string,node>>`
  (linear key lookup); `array_t = std::vector<node>`.
- Resolution: **quoted => string; bare => bool/null/int/double else string** (so `0` is the
  int arm, `0.5` the double arm - `as_num` must accept both).
- Lenient: bare keys, one trailing comma, **`//` line + `/* */` block comments** (added),
  `\uXXXX`(+surrogates) -> UTF-8, depth 128.

### Verified neui conventions this builds on

- Asset handle `neui_asset_t { uint32_t id }` packed `(session<<16)|slot`; `asset_none`
  (`include/neui/d/assets.h:33-37`). Kinds NONE/BITMAP/COMPOUND/BEHAVIOR/SURFACE/(SVG,VECTOR
  reserved)/FONT=7 (`assets.h:43-71`) - **next free value is 8**.
- `assets->create_compound`/`create_behavior`/`create_from_file`/`get_kind`/`get_size`/
  `destroy` (`assets.h:84-215`). No refcount: client keeps assets alive; `clear()` at session
  teardown releases all slots (`hosts/shared/asset_store.h`).
- `set_asset` is already kind-routed (`hosts/crossplatform/widgets.cpp:459-500`): BEHAVIOR ->
  `behavior_asset`, COMPOUND/other -> `compound_asset`; cross-session rejected; unknown kind
  falls through to compound slot.
- Compound/behavior build APIs: `neui_compound_api_t` (`compound.h:147-289`),
  `neui_behavior_api_t` (`behavior.h:88-183`).
- Vtable-append for evolution (`CLAUDE.md`): new methods go at the END of an existing API
  struct; all hosts rebuild together pre-1.0. `get_interface` is a strcmp table
  (`host.cpp:159-177`). **No new interface is needed** - component methods append to the
  existing `assets` and `widgets` vtables.

## Recommended approach: host-side, no client helper

Add a new asset kind `NEUI_ASSET_KIND_COMPONENT = 8`. A component asset internally owns the
COMPOUND + BEHAVIOR it builds, plus a default-attr template, a param manifest, and a default
size. It is created, owned, and released by the host's asset store like any other asset.

**Public surface (all vtable-appended; no new interface):**

`neui_asset_api_t` (`include/neui/d/assets.h`):
```c
neui_asset_t (NEUI_ABI *create_component_from_string)(neui_session_t, const char* json_utf8,
                                                      uint32_t len, const neui_component_env_t*);
neui_asset_t (NEUI_ABI *create_component_from_file)(neui_session_t, const char* path_utf8,
                                                    const neui_component_env_t*);
uint32_t     (NEUI_ABI *component_param_count)(neui_session_t, neui_asset_t component);
bool         (NEUI_ABI *component_param_at)(neui_session_t, neui_asset_t component,
                                            uint32_t index, neui_component_param_t* out);
```
`env` may be NULL (path mode; `_from_file` sets base_dir to the file's directory). `get_kind`
returns COMPONENT; `get_size` returns the default size; `destroy` releases the owned
compound + behavior too.

`neui_widget_api_t` (`include/neui/d/widgets.h`):
```c
neui_widget_t (NEUI_ABI *create_from_component)(neui_session_t, neui_widget_t parent,
                                                neui_asset_t component,
                                                int x, int y, int width, int height);
```
Creates a CUSTOMDRAW (size from the component default if w/h <= 0), attaches the component's
compound + behavior to both slots, and stamps the component's default attrs. Per-instance
values are then set via the existing `attrs->set_*`. Returns `widget_none` on a
bad/non-component asset.

`set_asset` (extend the existing kind-routing in all three hosts): a COMPONENT asset routes
to **both** `compound_asset` and `behavior_asset` slots and stamps default attrs - so a
component can also be attached to an already-created CUSTOMDRAW. (COMPOUND/BEHAVIOR/IMAGE
routing is unchanged.)

### Calling sequence (replaces the old helper sequence)

```c
neui_asset_t knob = assets->create_component_from_file(sess, "components/knob.json", NULL); // parse ONCE

// path A - one call:
neui_widget_t w = widgets->create_from_component(sess, parent, knob, x, y, w, h);
attrs->set_string(sess, w, "name", "Cutoff");          // per-instance, via the EXISTING attr API
attrs->set_float (sess, w, NEUI_PARAM_VALUE, 0.5f);

// path B - attach to an existing custom-draw widget:
neui_widget_t cw = widgets->create(sess, parent, NEUI_W_CUSTOMDRAW, x, y, w, h, NULL);
widgets->set_asset(sess, cw, knob);                    // COMPONENT -> both slots + defaults

assets->destroy(sess, knob);                           // releases owned compound + behavior
```

The compound + behavior handles are shared across every instance (shape shared, AttrBag
per-widget) - exactly the existing reuse model, now with one-line authoring.

## File layout

- `include/neui/d/component.h` - NEW. `neui_component_env_t` (base_dir + `resolve_asset`
  callback + user) and `neui_component_param_t` (`{ const char* key; const char* label;
  float min, max, def; }`, strings owned by the component, valid until it is destroyed), the
  `NEUI_ASSET_KIND_COMPONENT` doc, and the JSON schema reference. `assets.h` `#include`s it.
  No component *vtable* (a component has no mutation API - it is loaded whole).
- `include/neui/d/assets.h` - add `NEUI_ASSET_KIND_COMPONENT = 8` to the kind enum; append
  the 4 methods above to `neui_asset_api_t`; include `component.h`.
- `include/neui/d/widgets.h` - append `create_from_component` to `neui_widget_api_t`.
- `hosts/shared/component_loader.h` - NEW, header-only (ODR-safe `inline`, like
  `compound.h`), **compiled into each host** (not the client). `#include "mujson.h"`. Holds
  the parser-isolation helpers, the `kPropType` table, anchor/state/kind token tables, and
  `build_component(...)` - the mujson walker that drives the passed-in
  `asset`/`compound`/`behavior` api pointers to create the compound + behavior and collect
  defaults/manifest/size, returning a plain struct. It does NOT touch the asset store, so it
  is unit-testable with fake api vtables.
- `hosts/shared/asset_store.h` - `AssetEntry` gains component fields
  (`neui_asset_t comp_compound, comp_behavior;` default-attr list; param list; default w/h);
  `allocate_component(...)`; `release_slot` for COMPONENT releases the two owned sub-asset
  slots; `get_kind`/`get_size` handle COMPONENT; `get_pixels_for_export` rejects it.
- Per-host glue (`hosts/crossplatform/{widgets.cpp,host.cpp}`, `hosts/win32/widgets.cpp`,
  `hosts/macos/widgets.mm`): implement the 3 asset thunks + `create_from_component`
  (call `build_component` with the host's own `asset_api`/`compound_api`/`behavior_api`
  pointers, then `allocate_component` to wrap, then attach + stamp), and extend `set_asset`
  for the COMPONENT kind. CUSTOMDRAW already supports compound+behavior on all three hosts,
  so both slots exist.
- `examples/component_knob_example.cpp` + `examples/components/knob.json` + slices -> CMake
  target `neui_component_knob_example`.

## JSON schema (every field maps to an existing prop)

Top-level key renamed `component`. Anchors are the 9 tokens
`top_left|top|top_right|left|center|right|bottom_left|bottom|bottom_right` -> `neui_anchor_t`.
`"fill"` on a size axis -> `NEUI_COMPOUND_FILL (-1)`. Colors are `"#AARRGGBB"` (string arm) or
a bare int -> ARGB. `show_when` tokens `["hovered","!pressed"]` -> `NEUI_LAYER_STATE_*` bits.
mujson parses `//` and `/* */` comments, so the shipped sample may be annotated.

```jsonc
{
  "component": "knob",
  "size": [110, 110],

  "params": [
    { "key": "neui.param.value", "default": 0.5, "min": 0, "max": 1, "label": "Value" }
  ],

  "assets": { "bg": "knob_bg.png", "indicator": "knob_move.png" },

  "layers": [
    { "kind": "asset", "z": 0, "anchor": ["center","center"], "size": "fill", "asset": "bg" },
    { "kind": "asset", "z": 1, "anchor": ["center","center"], "size": [70,70], "offset": [0,-15],
      "asset": "indicator",
      "bind": { "rotation": { "attr": "neui.param.value", "scale": 4.71238898, "offset": 0 } } },
    { "kind": "text", "z": 2, "anchor": ["bottom","bottom"], "size": ["fill", 22],
      "text": "{name}: {value}", "font_size": 14, "align": ["center","center"], "weight": 400 },
    { "kind": "rect", "z": -1, "anchor": ["top_left","top_left"], "size": "fill",
      "fill_color": "#33336699", "stroke_color": "#FF6699CC", "stroke_width": 1.5, "corner_radius": 14 },
    { "kind": "path", "z": 3, "anchor": ["bottom_right","bottom_right"], "size": [20,20],
      "offset": [-6,-6], "fill_color": "#FFFFFFFF",
      "path": [ {"m":[4,3]}, {"l":[15,10]}, {"l":[4,17]}, {"l":[9,10]}, {"z":true} ] }
  ],

  "behavior": [
    { "kind": "drag_rotational", "target": "neui.param.value", "min": 0, "max": 1, "deadzone": 4 },
    { "kind": "wheel",    "step": 0.02 },
    { "kind": "key_step", "step": 0.01, "coarse": 0.10 },
    { "kind": "context_reset", "target_default": "neui.param.default" }
  ]
}
```

### Field -> prop mapping (for the implementer)

- **Layer common**: `offset:[x,y]` -> `offset_x/offset_y`; `size` (string `"fill"` or
  `[fill|num, fill|num]`) -> `width/height` (-1 = fill); `alpha`; `show_when` tokens ->
  bitmask (`!x` => `NOT_*` bit); `bind` map -> `compound->bind(prop, attr, scale, offset)`;
  `bind_asset` `{ "asset": { "attr": "k" } }` -> `compound->bind_asset`.
- **text**: `text` -> `set_string("text")` ({key} templating kept); `font_size` ->
  `set_float("size")`; `color` (string `#..` or int) -> `set_int("color")`; `align:[x,y]` ->
  `align_x/align_y`; `family`/`weight`.
- **asset**: `asset` name -> resolve -> `set_asset("asset")`; `rotation` float; `tint` int.
- **rect/path**: `fill_color`/`stroke_color`/`stroke_width`/`corner_radius` (rect).
- **path**: array of `{"m":[x,y]}|{"l":[x,y]}|{"a":[cx,cy,r,a0,a1]}|{"z":true}` ->
  `neui_path_cmd_t[]` -> `set_path` (1:1 MOVE_TO/LINE_TO/ARC/CLOSE).
- **behavior**: `kind` -> `NEUI_BEHAVIOR_KIND_*`; other keys are documented handler props.
  A single `kPropType` table (name -> {int|float|string}) drives both layer and behavior
  walking; numeric props go through `as_num` (int+double arms); color props check string
  then int arm. Adding a future prop is one table row.
- **params/defaults**: each `params[]` entry stamps `default` onto the component's default
  AttrBag (under `key`) and contributes a `neui_component_param_t` to the manifest.

## Asset resolution + designer handover

`neui_component_env_t` (passed to the load call): **path mode (default, env NULL)** resolves
asset names to files relative to `base_dir` via `asset_api->create_from_file` (`@2x`/`@3x`
aware) - a component is a folder (`knob.json` + slices). **Injected mode**: a non-NULL
`resolve_asset(user, name, hint_path)` is consulted first (return `asset_none` to fall
through to path mode) so a client can hand in pre-loaded / in-memory handles by name.
Path-resolved assets are owned by the component (released with it); callback-returned assets
are borrowed (owned/borrowed flag per layer asset). The visual half (layers/assets) is
tool-emittable; the behavioral half (`bind`/`behavior`/`params`) is human annotation - kept
in distinct top-level keys so a tool can regenerate visuals without clobbering behavior.

## Build steps (execution order)

1. `include/neui/d/component.h` (structs + kind doc) and the `assets.h`/`widgets.h` vtable
   appends + `NEUI_ASSET_KIND_COMPONENT = 8`.
2. `hosts/shared/asset_store.h` - COMPONENT entry fields + `allocate_component` + release.
3. `hosts/shared/component_loader.h` - `#include "mujson.h"`; parser-isolation helpers,
   `kPropType` + token tables, `build_component(session, json, len, env, asset_api,
   compound_api, behavior_api)` returning `{ compound, behavior, defaults, params, w, h }`.
   Reuses `neui_anchor_t`, `NEUI_COMPOUND_LAYER_*`, `NEUI_BEHAVIOR_KIND_*`, `neui_path_cmd_t`.
4. Per-host glue in all three hosts: the 3 asset thunks + `create_from_component` + the
   COMPONENT branch in `set_asset`.
5. `examples/component_knob_example.cpp` + `examples/components/knob.json` + slices; CMake
   target `neui_component_knob_example`.
6. `tests/test_component_loader.cpp` - Tier-1; compiles `src/mujson.cpp` (like
   `test_mujson.cpp`); drives `build_component` with **fake** asset/compound/behavior api
   vtables that record calls, and asserts the emitted layer kinds/anchors/bindings, behavior
   handler kinds/props, defaults, and manifest. Builds everywhere incl. null. Add to
   `tests/CMakeLists.txt`.

## Verification

- **Unit (Tier-1)**: `test_component_loader.cpp` under `ctest`, all platforms. Cover the
  mujson-sensitive spots: int-vs-double (`"min":0` and `"scale":0.5`), `#..` string color vs
  bare-int color, a `"fill"` axis, a negated `show_when`, a `//` and a `/* */` comment, and a
  path command list. Build with `vcvars64.bat` then
  `cmake --build out/build/x64-Debug --target neui_tests`; run `neui_tests.exe` (the shell
  needs the VS dev environment or even `<algorithm>` is not found).
- **Visual / end-to-end**: `cmake --build out/build/x64-Debug --target neui_component_knob_example`.
  It loads `components/knob.json`, makes 3-4 knobs (mix of `create_from_component` and
  `create` + `set_asset`) with different `name`/`value`, and confirms drag/wheel/key/reset
  parity with the hand-coded knobs in `examples/main.cpp`. Screenshot via PrintWindow +
  PW_RENDERFULLCONTENT (D2D swap-chain CopyFromScreen is black).
- **Boilerplate win**: per-knob client code ~3 lines (`create_component_*` once +
  `create_from_component` per knob), down from ~45.
- **Lifetime**: destroying the component releases its compound + behavior; instantiated
  widgets paint as cleared (no crash) if destroyed out of order - matches the IMAGE convention.

## Decisions (resolved)

1. **Architecture**: host-side. Component is a first-class session asset
   (`NEUI_ASSET_KIND_COMPONENT`) built + owned by the host; the parser stays encapsulated in
   `libneui`. No client-side helper library, no `neui_part_t`, no override-string parser.
2. **Name**: **Component** (bundles a COMPOUND + a BEHAVIOR + defaults). Close to "compound"
   by design; the header documents the distinction.
3. **Instantiate**: **both** - `widgets->create_from_component` (primary one-call) AND a
   COMPONENT-aware `set_asset` (attach to an existing CUSTOMDRAW). Per-instance values via the
   existing `attrs->set_*`.
4. **Parser**: `neui::mujson` (core lib), driven internally; comment support added. Numeric
   reads via `as_num` (int+double); colors accept `#AARRGGBB` or int.
5. **Surface**: vtable-append only (assets + widgets); no new `get_interface` entry. Loader
   is host-compiled shared code, unit-tested in isolation via fake api vtables.

## Status

Executable. The former open dependency (parser) is closed; the architecture is now host-side
per the decision that, with the parser in the host, a client helper library is unnecessary.

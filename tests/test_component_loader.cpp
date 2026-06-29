// Tier-1 unit tests for the host-agnostic component loader
// (hosts/shared/component_loader.h). Drives build_component with FAKE
// asset / compound / behavior api vtables that record every call, then
// asserts the emitted layers, bindings, behavior handlers, defaults and
// param manifest. Links no host; mujson.cpp is compiled into the suite.

#include "neui_test.h"
#include "component_loader.h"

#include <map>
#include <string>
#include <vector>

using namespace neui_detail;

// --- recorders -------------------------------------------------------------

namespace
{
  struct LayerRec
  {
    neui_compound_layer_kind_t kind = NEUI_COMPOUND_LAYER_NONE;
    int                        z    = 0;
    int                        parent = -1;   // -1 == top-level; else parent layer id
    neui_anchor_t              pa = NEUI_ANCHOR_TOP_LEFT, sa = NEUI_ANCHOR_TOP_LEFT;
    std::map<std::string, int>          ints;
    std::map<std::string, float>        flts;
    std::map<std::string, std::string>  strs;
    std::map<std::string, uint32_t>     assets;          // prop -> asset id
    struct Bind { std::string attr; float scale, offset; };
    std::map<std::string, Bind>         binds;           // numeric binds
    std::map<std::string, std::string>  bind_assets;     // prop -> attr key
    std::vector<neui_path_cmd_t>        path;
  };
  struct HandlerRec
  {
    neui_behavior_kind_t                kind = NEUI_BEHAVIOR_KIND_NONE;
    std::map<std::string, int>          ints;
    std::map<std::string, float>        flts;
    std::map<std::string, std::string>  strs;
  };

  struct FrameLayoutRec { uint32_t cols, rows, gutter; };
  std::vector<LayerRec>              g_layers;
  std::vector<HandlerRec>            g_handlers;
  std::vector<std::string>           g_loaded_files;
  std::map<uint32_t, FrameLayoutRec> g_frame_layouts;   // asset id -> grid
  bool                     g_compound_created = false;
  bool                     g_behavior_created = false;
  bool                     g_frame_layout_ok  = true;   // fake set_frame_layout result

  void reset_recorders()
  {
    g_layers.clear();
    g_handlers.clear();
    g_loaded_files.clear();
    g_frame_layouts.clear();
    g_compound_created = false;
    g_behavior_created = false;
    g_frame_layout_ok  = true;
  }

  // --- fake asset api ------------------------------------------------------
  neui_asset_t NEUI_ABI fake_create_compound(neui_session_t)
  { g_compound_created = true; neui_asset_t a; a.id = 0x10; return a; }
  neui_asset_t NEUI_ABI fake_create_behavior(neui_session_t)
  { g_behavior_created = true; neui_asset_t a; a.id = 0x20; return a; }
  neui_asset_t NEUI_ABI fake_create_from_file(neui_session_t, const char* path)
  {
    g_loaded_files.emplace_back(path ? path : "");
    neui_asset_t a; a.id = 1000u + static_cast<uint32_t>(g_loaded_files.size());
    return a;
  }
  bool NEUI_ABI fake_set_frame_layout(neui_session_t, neui_asset_t a,
                                      uint32_t cols, uint32_t rows, uint32_t gutter)
  {
    if (!g_frame_layout_ok) return false;   // simulate a layout that won't fit
    g_frame_layouts[a.id] = { cols, rows, gutter };
    return true;
  }

  // --- fake compound api ---------------------------------------------------
  neui_compound_layer_t NEUI_ABI fake_add_layer(neui_session_t, neui_asset_t,
                                                neui_compound_layer_kind_t kind, int z)
  {
    LayerRec r; r.kind = kind; r.z = z; r.parent = -1;
    g_layers.push_back(r);
    neui_compound_layer_t l; l.id = static_cast<uint32_t>(g_layers.size() - 1);
    return l;
  }
  neui_compound_layer_t NEUI_ABI fake_add_child_layer(neui_session_t, neui_asset_t,
                                                      neui_compound_layer_t parent,
                                                      neui_compound_layer_kind_t kind, int z)
  {
    LayerRec r; r.kind = kind; r.z = z; r.parent = static_cast<int>(parent.id);
    g_layers.push_back(r);
    neui_compound_layer_t l; l.id = static_cast<uint32_t>(g_layers.size() - 1);
    return l;
  }
  void NEUI_ABI fake_set_anchor(neui_session_t, neui_asset_t, neui_compound_layer_t l,
                                neui_anchor_t pa, neui_anchor_t sa)
  { g_layers[l.id].pa = pa; g_layers[l.id].sa = sa; }
  void NEUI_ABI fake_c_set_int(neui_session_t, neui_asset_t, neui_compound_layer_t l,
                               const char* prop, int v)
  { g_layers[l.id].ints[prop] = v; }
  void NEUI_ABI fake_c_set_float(neui_session_t, neui_asset_t, neui_compound_layer_t l,
                                 const char* prop, float v)
  { g_layers[l.id].flts[prop] = v; }
  void NEUI_ABI fake_c_set_string(neui_session_t, neui_asset_t, neui_compound_layer_t l,
                                  const char* prop, const char* v)
  { g_layers[l.id].strs[prop] = v ? v : ""; }
  void NEUI_ABI fake_c_set_asset(neui_session_t, neui_asset_t, neui_compound_layer_t l,
                                 const char* prop, neui_asset_t v)
  { g_layers[l.id].assets[prop] = v.id; }
  void NEUI_ABI fake_bind(neui_session_t, neui_asset_t, neui_compound_layer_t l,
                          const char* prop, const char* attr, float scale, float offset)
  { g_layers[l.id].binds[prop] = { attr ? attr : "", scale, offset }; }
  void NEUI_ABI fake_bind_asset(neui_session_t, neui_asset_t, neui_compound_layer_t l,
                                const char* prop, const char* attr)
  { g_layers[l.id].bind_assets[prop] = attr ? attr : ""; }
  void NEUI_ABI fake_set_path(neui_session_t, neui_asset_t, neui_compound_layer_t l,
                              const neui_path_cmd_t* cmds, uint32_t count)
  { g_layers[l.id].path.assign(cmds, cmds + count); }

  // --- fake behavior api ---------------------------------------------------
  neui_behavior_handler_t NEUI_ABI fake_add_handler(neui_session_t, neui_asset_t,
                                                    neui_behavior_kind_t kind)
  {
    HandlerRec r; r.kind = kind;
    g_handlers.push_back(r);
    neui_behavior_handler_t h; h.id = static_cast<uint32_t>(g_handlers.size() - 1);
    return h;
  }
  void NEUI_ABI fake_b_set_int(neui_session_t, neui_asset_t, neui_behavior_handler_t h,
                               const char* prop, int v)
  { g_handlers[h.id].ints[prop] = v; }
  void NEUI_ABI fake_b_set_float(neui_session_t, neui_asset_t, neui_behavior_handler_t h,
                                 const char* prop, float v)
  { g_handlers[h.id].flts[prop] = v; }
  void NEUI_ABI fake_b_set_string(neui_session_t, neui_asset_t, neui_behavior_handler_t h,
                                  const char* prop, const char* v)
  { g_handlers[h.id].strs[prop] = v ? v : ""; }

  ComponentApis make_fake_apis(neui_asset_api_t& a, neui_compound_api_t& c,
                               neui_behavior_api_t& b)
  {
    a = neui_asset_api_t{};
    a.create_compound   = fake_create_compound;
    a.create_behavior   = fake_create_behavior;
    a.create_from_file  = fake_create_from_file;
    a.set_frame_layout  = fake_set_frame_layout;

    c = neui_compound_api_t{};
    c.add_layer       = fake_add_layer;
    c.add_child_layer = fake_add_child_layer;
    c.set_anchor  = fake_set_anchor;
    c.set_int     = fake_c_set_int;
    c.set_float   = fake_c_set_float;
    c.set_string  = fake_c_set_string;
    c.set_asset   = fake_c_set_asset;
    c.bind        = fake_bind;
    c.bind_asset  = fake_bind_asset;
    c.set_path    = fake_set_path;

    b = neui_behavior_api_t{};
    b.add_handler = fake_add_handler;
    b.set_int     = fake_b_set_int;
    b.set_float   = fake_b_set_float;
    b.set_string  = fake_b_set_string;

    ComponentApis apis;
    apis.asset = &a; apis.compound = &c; apis.behavior = &b;
    return apis;
  }

  BuiltComponent run_loader(const std::string& json,
                            const neui_component_env_t* env = nullptr)
  {
    reset_recorders();
    neui_asset_api_t a; neui_compound_api_t c; neui_behavior_api_t b;
    ComponentApis apis = make_fake_apis(a, c, b);
    neui_session_t sess; sess.session = 1;
    return build_component(sess, json.c_str(),
                           static_cast<uint32_t>(json.size()), env, apis);
  }

  const LayerRec* layer_of_kind(neui_compound_layer_kind_t k, int nth = 0)
  {
    int seen = 0;
    for (const auto& l : g_layers)
      if (l.kind == k && seen++ == nth) return &l;
    return nullptr;
  }
} // namespace

// A representative knob document exercising every mujson-sensitive path:
// int-vs-double, "#AARRGGBB" string color AND a bare-int color, a "fill"
// axis, a negated show_when token, // and /* */ comments, and a path list.
static const char* kKnobJson = R"json(
{
  // a knob, fully annotated
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
      "text": "{name}: {value}", "font_size": 14, "color": "#FF6699CC",
      "align": ["center","bottom"], "weight": 400,
      "show_when": ["hovered","!pressed"] },
    /* a framing rect with a hex color, plus a sibling with a bare-int color */
    { "kind": "rect", "z": -1, "anchor": ["top_left","top_left"], "size": "fill",
      "fill_color": "#33336699", "stroke_width": 1.5, "corner_radius": 14 },
    { "kind": "rect", "z": -2, "anchor": ["top_left","top_left"], "size": "fill",
      "fill_color": 305419896 },
    { "kind": "path", "z": 3, "anchor": ["bottom_right","bottom_right"], "size": [20,20],
      "fill_color": "#FFFFFFFF",
      "path": [ {"m":[4,3]}, {"l":[15,10]}, {"l":[4,17]}, {"l":[9,10]}, {"z":true} ] }
  ],
  "behavior": [
    { "kind": "drag_rotational", "target": "neui.param.value", "min": 0, "max": 1, "deadzone": 4 },
    { "kind": "wheel", "step": 0.02 },
    { "kind": "key_step", "step": 0.01, "coarse": 0.10 },
    { "kind": "context_reset", "target_default": "neui.param.default" }
  ]
}
)json";

TEST_CASE("component_loader: builds compound + behavior + defaults")
{
  BuiltComponent c = run_loader(kKnobJson);

  CHECK(c.ok);
  CHECK(c.name == "knob");
  CHECK(g_compound_created && g_behavior_created);
  CHECK(c.compound.id == 0x10);
  CHECK(c.behavior.id == 0x20);
  CHECK(c.width == 110.0f && c.height == 110.0f);

  // params + defaults
  CHECK(c.params.size() == 1);
  CHECK(c.params[0].key == "neui.param.value");
  CHECK(c.params[0].min == 0.0f && c.params[0].max == 1.0f && c.params[0].def == 0.5f);
  CHECK(c.defaults.size() == 1);
  CHECK(c.defaults[0].key == "neui.param.value");
  CHECK(c.defaults[0].type == ComponentDefaultAttr::FLOAT && c.defaults[0].fval == 0.5f);

  // layer count + kinds
  CHECK(g_layers.size() == 6);
  CHECK(layer_of_kind(NEUI_COMPOUND_LAYER_ASSET, 0) != nullptr);
  CHECK(layer_of_kind(NEUI_COMPOUND_LAYER_TEXT, 0)  != nullptr);
  CHECK(layer_of_kind(NEUI_COMPOUND_LAYER_PATH, 0)  != nullptr);
}

TEST_CASE("component_loader: anchors, fill, offset, bind")
{
  run_loader(kKnobJson);

  // bg asset layer: anchored center/center, both axes fill, asset resolved
  const LayerRec* bg = layer_of_kind(NEUI_COMPOUND_LAYER_ASSET, 0);
  REQUIRE(bg);
  CHECK(bg->pa == NEUI_ANCHOR_CENTER && bg->sa == NEUI_ANCHOR_CENTER);
  CHECK(bg->ints.at("width")  == NEUI_COMPOUND_FILL);
  CHECK(bg->ints.at("height") == NEUI_COMPOUND_FILL);
  CHECK(bg->assets.count("asset") == 1);

  // indicator layer: 70x70, offset (0,-15), rotation bound to value @ ~3pi/2
  const LayerRec* ind = layer_of_kind(NEUI_COMPOUND_LAYER_ASSET, 1);
  REQUIRE(ind);
  CHECK(ind->ints.at("width") == 70 && ind->ints.at("height") == 70);
  CHECK(ind->ints.at("offset_x") == 0 && ind->ints.at("offset_y") == -15);
  REQUIRE(ind->binds.count("rotation") == 1);
  CHECK(ind->binds.at("rotation").attr == "neui.param.value");
  CHECK(ind->binds.at("rotation").scale > 4.7f && ind->binds.at("rotation").scale < 4.72f);
}

TEST_CASE("component_loader: asset layer frame prop (static + bound) round-trips")
{
  const char* json = R"json({
    "size": [80, 80],
    "layers": [
      { "kind": "asset", "asset": "strip", "frame": 7 },
      { "kind": "asset", "asset": "strip",
        "bind": { "frame": { "attr": "neui.param.value", "scale": 63, "offset": 0 } } }
    ]
  })json";
  BuiltComponent built = run_loader(json);
  (void)built;

  const LayerRec* a0 = layer_of_kind(NEUI_COMPOUND_LAYER_ASSET, 0);
  const LayerRec* a1 = layer_of_kind(NEUI_COMPOUND_LAYER_ASSET, 1);
  REQUIRE(a0); REQUIRE(a1);
  CHECK(a0->ints.at("frame") == 7);             // static frame parsed
  REQUIRE(a1->binds.count("frame") == 1);       // bound frame parsed
  CHECK(a1->binds.at("frame").attr == "neui.param.value");
  CHECK(a1->binds.at("frame").scale == 63.0f);
  CHECK(a0->ints.count("frame") == 1 && a1->ints.count("frame") == 0);
}

TEST_CASE("component_loader: frame_layout tags the resolved asset")
{
  const char* json = R"json({
    "size": [80, 80],
    "assets": { "strip": "knob_100.png" },
    "layers": [
      { "kind": "asset", "asset": "strip",
        "frame_layout": { "frames": 100, "orientation": "vertical" },
        "bind": { "frame": { "attr": "neui.param.value", "scale": 99 } } }
    ]
  })json";
  run_loader(json);

  // The asset layer resolved "strip" to a create_from_file handle; the loader
  // tagged that handle with the frame grid via set_frame_layout.
  const LayerRec* a0 = layer_of_kind(NEUI_COMPOUND_LAYER_ASSET, 0);
  REQUIRE(a0);
  REQUIRE(a0->assets.count("asset") == 1);
  uint32_t aid = a0->assets.at("asset");
  REQUIRE(g_frame_layouts.count(aid) == 1);
  CHECK_EQ((int)g_frame_layouts.at(aid).cols, 1);
  CHECK_EQ((int)g_frame_layouts.at(aid).rows, 100);
  // frame bind round-trips through the generic numeric-bind path.
  REQUIRE(a0->binds.count("frame") == 1);
  CHECK(a0->binds.at("frame").scale == 99.0f);
}

TEST_CASE("component_loader: a frame_layout the asset rejects is not recorded")
{
  // When set_frame_layout fails (a grid that doesn't fit the bitmap), the asset
  // stays an untagged plain bitmap, so the loader must NOT record the layout -
  // otherwise serialize would re-emit a frame_layout the runtime dropped.
  reset_recorders();
  neui_asset_api_t a; neui_compound_api_t c; neui_behavior_api_t b;
  ComponentApis apis = make_fake_apis(a, c, b);
  g_frame_layout_ok = false;

  std::string json = R"json({
    "size": [80, 80],
    "assets": { "strip": "knob.png" },
    "layers": [
      { "kind": "asset", "asset": "strip", "frame_layout": { "frames": 700 } }
    ]
  })json";
  neui_session_t sess; sess.session = 1;
  BuiltComponent built = build_component(sess, json.c_str(), (uint32_t)json.size(),
                                         nullptr, apis);
  CHECK(built.ok);
  CHECK(g_frame_layouts.empty());             // tag attempted, asset refused it
  CHECK(built.asset_frame_layouts.empty());   // so nothing is recorded for re-emit
  g_frame_layout_ok = true;                   // restore for later test cases
}

TEST_CASE("component_loader: text props, colors, show_when")
{
  run_loader(kKnobJson);

  const LayerRec* txt = layer_of_kind(NEUI_COMPOUND_LAYER_TEXT, 0);
  REQUIRE(txt);
  CHECK(txt->strs.at("text") == "{name}: {value}");
  CHECK(txt->flts.at("size") == 14.0f);     // font_size -> "size"
  CHECK(txt->ints.at("weight") == 400);
  CHECK(txt->ints.at("align_x") == 1);       // center
  CHECK(txt->ints.at("align_y") == 2);       // bottom
  CHECK(txt->ints.at("color") == static_cast<int>(static_cast<uint32_t>(0xFF6699CCu)));
  // show_when ["hovered","!pressed"]
  CHECK(txt->ints.at("show_when") ==
        static_cast<int>(NEUI_LAYER_STATE_HOVERED | NEUI_LAYER_STATE_NOT_PRESSED));

  // hex color on the framing rect, and a bare-int color on its sibling
  const LayerRec* rect0 = layer_of_kind(NEUI_COMPOUND_LAYER_RECT, 0);
  const LayerRec* rect1 = layer_of_kind(NEUI_COMPOUND_LAYER_RECT, 1);
  REQUIRE(rect0); REQUIRE(rect1);
  CHECK(rect0->ints.at("fill_color") == static_cast<int>(static_cast<uint32_t>(0x33336699u)));
  CHECK(rect0->flts.at("stroke_width") == 1.5f);
  CHECK(rect0->flts.at("corner_radius") == 14.0f);
  CHECK(rect1->ints.at("fill_color") == 305419896); // bare int arm
}

TEST_CASE("component_loader: path geometry")
{
  run_loader(kKnobJson);

  const LayerRec* p = layer_of_kind(NEUI_COMPOUND_LAYER_PATH, 0);
  REQUIRE(p);
  CHECK(p->ints.at("fill_color") == static_cast<int>(static_cast<uint32_t>(0xFFFFFFFFu)));
  REQUIRE(p->path.size() == 5);
  CHECK(p->path[0].kind == NEUI_PATH_CMD_MOVE_TO);
  CHECK(p->path[0].args[0] == 4.0f && p->path[0].args[1] == 3.0f);
  CHECK(p->path[1].kind == NEUI_PATH_CMD_LINE_TO);
  CHECK(p->path[4].kind == NEUI_PATH_CMD_CLOSE);
}

TEST_CASE("component_loader: path fill-rule + stroke style")
{
  const char* json = R"({
    "component": "t", "size": [10, 10],
    "layers": [
      { "kind": "path", "size": [10, 10],
        "fill_color": "#FF000000", "fill_rule": "evenodd",
        "stroke_color": "#FF112233", "stroke_width": 2,
        "stroke_linecap": "round", "stroke_linejoin": "bevel",
        "stroke_miterlimit": 8, "stroke_dasharray": [4, 2], "stroke_dashoffset": 1,
        "path": [ {"m":[0,0]}, {"l":[5,5]} ] }
    ]
  })";
  run_loader(json);
  const LayerRec* p = layer_of_kind(NEUI_COMPOUND_LAYER_PATH, 0);
  REQUIRE(p);
  CHECK(p->ints.at("fill_rule")  == 1);   // evenodd
  CHECK(p->ints.at("stroke_cap") == 1);   // round
  CHECK(p->ints.at("stroke_join") == 2);  // bevel
  CHECK(p->flts.at("stroke_miter") == 8.0f);
  CHECK(p->flts.at("stroke_dash_offset") == 1.0f);
  CHECK(p->strs.count("stroke_dasharray") == 1);  // array -> string transport
}

TEST_CASE("component_loader: behavior handlers + typed props")
{
  run_loader(kKnobJson);

  REQUIRE(g_handlers.size() == 4);
  CHECK(g_handlers[0].kind == NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL);
  CHECK(g_handlers[0].strs.at("target") == "neui.param.value");
  CHECK(g_handlers[0].flts.at("min") == 0.0f && g_handlers[0].flts.at("max") == 1.0f);
  CHECK(g_handlers[0].flts.at("deadzone") == 4.0f);   // float arm, bare int 4
  CHECK(g_handlers[1].kind == NEUI_BEHAVIOR_KIND_WHEEL);
  CHECK(g_handlers[1].flts.at("step") == 0.02f);
  CHECK(g_handlers[2].kind == NEUI_BEHAVIOR_KIND_KEY_STEP);
  CHECK(g_handlers[2].flts.at("coarse") == 0.10f);
  CHECK(g_handlers[3].kind == NEUI_BEHAVIOR_KIND_CONTEXT_RESET);
  CHECK(g_handlers[3].strs.at("target_default") == "neui.param.default");
}

TEST_CASE("component_loader: asset resolution via env callback")
{
  // resolve_asset returns a handle for "bg" (borrowed, NOT path-loaded) and
  // falls through to path mode for "indicator".
  struct Ctx { int calls = 0; } ctx;
  neui_component_env_t env{};
  env.user = &ctx;
  env.base_dir = "res";
  env.resolve_asset = [](void* u, const char* name, const char*) -> neui_asset_t {
    static_cast<Ctx*>(u)->calls++;
    if (std::string(name) == "bg") { neui_asset_t a; a.id = 7777; return a; }
    return asset_none; // fall through to path mode
  };

  BuiltComponent c = run_loader(kKnobJson, &env);
  CHECK(ctx.calls == 2);                       // both asset layers consulted
  // "bg" came from the callback (borrowed); only "indicator" was path-loaded.
  CHECK(g_loaded_files.size() == 1);
  CHECK(g_loaded_files[0] == std::string("res/knob_move.png"));
  CHECK(c.owned_assets.size() == 1);           // only the path-loaded one is owned
}

TEST_CASE("component_loader: malformed json fails gracefully")
{
  BuiltComponent c = run_loader("{ this is : not valid ] ]");
  CHECK(!c.ok);
  CHECK(c.compound.id == asset_none.id);
}

TEST_CASE("component_loader: a group layer nests children via add_child_layer")
{
  const char* json = R"json({
    "size": [60, 60],
    "layers": [
      { "kind": "group", "z": 0, "anchor": ["center","center"], "size": [40,40],
        "layers": [
          { "kind": "rect", "z": 0, "fill_color": "#FF101010" },
          { "kind": "text", "z": 1, "text": "{value}" }
        ] }
    ]
  })json";
  run_loader(json);

  REQUIRE(g_layers.size() == 3);
  // layer 0 is the group (top-level); 1 & 2 are its children (parent == 0).
  CHECK(g_layers[0].kind == NEUI_COMPOUND_LAYER_GROUP);
  CHECK(g_layers[0].parent == -1);
  CHECK(g_layers[0].ints.at("width") == 40 && g_layers[0].ints.at("height") == 40);
  CHECK(g_layers[1].kind == NEUI_COMPOUND_LAYER_RECT);
  CHECK(g_layers[1].parent == 0);
  CHECK(g_layers[2].kind == NEUI_COMPOUND_LAYER_TEXT);
  CHECK(g_layers[2].parent == 0);
}

TEST_CASE("build_component: arc layer props + value binding")
{
  const char* json = R"json({
    "size": [80, 80],
    "layers": [
      { "kind": "arc", "z": 1, "anchor": ["center","center"], "size": "fill",
        "stroke_color": "#FFE0E0E0", "stroke_width": 6, "stroke_linecap": "round",
        "begin_angle": -135, "end_angle": 135, "polarity": "center", "direction": "ccw",
        "bind": { "value": { "attr": "neui.param.value", "scale": 1, "offset": 0 } } }
    ]
  })json";
  run_loader(json);

  REQUIRE(g_layers.size() == 1);
  const LayerRec& a = g_layers[0];
  CHECK(a.kind == NEUI_COMPOUND_LAYER_ARC);
  CHECK(a.ints.at("stroke_color") == static_cast<int>(0xFFE0E0E0u));
  CHECK(a.flts.at("stroke_width") == 6.0f);
  CHECK(a.ints.at("stroke_cap") == 1);     // round
  CHECK(a.flts.at("begin_angle") == -135.0f);
  CHECK(a.flts.at("end_angle") == 135.0f);
  CHECK(a.ints.at("polarity") == 1);       // center
  CHECK(a.ints.at("direction") == 1);      // ccw
  REQUIRE(a.binds.count("value") == 1);
  CHECK(a.binds.at("value").attr == "neui.param.value");
}

// --- serialization (designer round-trip) -----------------------------------

namespace
{
  // Hand-build a small knob compound + behavior using the internal helpers,
  // plus the round-trip metadata, ready to feed serialize_component.
  struct HandBuilt
  {
    CompoundAsset ca;
    BehaviorAsset ba;
    std::string   name = "knob";
    std::vector<std::pair<std::string, std::string>> anames;
    std::vector<std::pair<uint32_t, std::string>>    hnames;
    std::vector<std::pair<uint32_t, FilmstripLayout>> flayouts;
  };

  HandBuilt make_handbuilt()
  {
    HandBuilt hb;
    hb.anames = { { "bg", "knob_bg.png" }, { "indicator", "knob_move.png" } };
    hb.hnames = { { 1001u, "bg" }, { 1002u, "indicator" } };
    // The bg asset is a 100-frame vertical strip (recorded at build time).
    hb.flayouts = { { 1001u, FilmstripLayout{ 1u, 100u, 0u } } };

    // bg asset layer (z 0), fill, centered
    uint32_t s0 = compound_add_layer(hb.ca, NEUI_COMPOUND_LAYER_ASSET, 0);
    CompoundLayer* L0 = compound_get_layer(hb.ca, s0);
    L0->parent_anchor = NEUI_ANCHOR_CENTER; L0->self_anchor = NEUI_ANCHOR_CENTER;
    L0->asset = neui_asset_t{ 1001u };

    // indicator asset layer (z 1), 70x70, offset (0,-15), rotation bound
    uint32_t s1 = compound_add_layer(hb.ca, NEUI_COMPOUND_LAYER_ASSET, 1);
    CompoundLayer* L1 = compound_get_layer(hb.ca, s1);
    L1->parent_anchor = NEUI_ANCHOR_CENTER; L1->self_anchor = NEUI_ANCHOR_CENTER;
    L1->width = 70; L1->height = 70; L1->offset_y = -15;
    L1->asset = neui_asset_t{ 1002u };
    apply_bind(*L1, "rotation", "neui.param.value", 4.71238898f, 0.0f);

    // text layer (z 2)
    uint32_t s2 = compound_add_layer(hb.ca, NEUI_COMPOUND_LAYER_TEXT, 2);
    CompoundLayer* L2 = compound_get_layer(hb.ca, s2);
    L2->parent_anchor = NEUI_ANCHOR_BOTTOM; L2->self_anchor = NEUI_ANCHOR_BOTTOM;
    L2->width = NEUI_COMPOUND_FILL; L2->height = 22;
    apply_set_string(*L2, "text", "{name}: {value}");
    L2->text_size = 14.0f;

    // behavior: drag_rotational (all defaults) + wheel (step 0.02)
    uint32_t h0 = behavior_add_handler(hb.ba, NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL);
    (void)h0;
    uint32_t h1 = behavior_add_handler(hb.ba, NEUI_BEHAVIOR_KIND_WHEEL);
    behavior_get_handler(hb.ba, h1)->step = 0.02f;

    return hb;
  }

  ComponentSerializeInput input_for(HandBuilt& hb)
  {
    ComponentSerializeInput in;
    in.name = &hb.name;
    in.width = 110.0f; in.height = 110.0f;
    in.asset_names = &hb.anames;
    in.asset_handle_names = &hb.hnames;
    in.asset_frame_layouts = &hb.flayouts;
    in.compound = &hb.ca;
    in.behavior = &hb.ba;
    return in;
  }
} // namespace

TEST_CASE("serialize_component: emits a parseable document")
{
  HandBuilt hb = make_handbuilt();
  std::string json = serialize_component(input_for(hb), 2);

  // re-parse with the same parser the loader uses
  auto root = neui::mujson::parse(json);
  CHECK(!root.empty());

  const auto* comp = cl_detail::obj_get(root, "component");
  CHECK(comp && std::get<std::string>(comp->value) == "knob");
  // assets block round-tripped by name
  const auto* assets = cl_detail::obj_get(root, "assets");
  REQUIRE(assets && std::holds_alternative<neui::mujson::object_t>(assets->value));
  const auto& ao = std::get<neui::mujson::object_t>(assets->value);
  bool has_bg = false;
  for (const auto& kv : ao)
    if (kv.first == "bg" && std::get<std::string>(kv.second.value) == "knob_bg.png") has_bg = true;
  CHECK(has_bg);
}

TEST_CASE("serialize_component: round-trips through the loader")
{
  HandBuilt hb = make_handbuilt();
  std::string json = serialize_component(input_for(hb), 0);

  // Feed the serialized JSON back through build_component (fake apis record).
  BuiltComponent rebuilt = run_loader(json);
  CHECK(rebuilt.ok);

  // same three layers, same kinds, same z order
  REQUIRE(g_layers.size() == 3);
  CHECK(g_layers[0].kind == NEUI_COMPOUND_LAYER_ASSET);
  CHECK(g_layers[1].kind == NEUI_COMPOUND_LAYER_ASSET);
  CHECK(g_layers[2].kind == NEUI_COMPOUND_LAYER_TEXT);

  // asset names resolved through the regenerated "assets" block (path mode)
  CHECK(g_loaded_files.size() == 2);
  CHECK(g_layers[0].assets.count("asset") == 1);

  // indicator geometry + rotation binding survived
  CHECK(g_layers[1].ints.at("width") == 70 && g_layers[1].ints.at("height") == 70);
  CHECK(g_layers[1].ints.at("offset_y") == -15);
  REQUIRE(g_layers[1].binds.count("rotation") == 1);
  CHECK(g_layers[1].binds.at("rotation").attr == "neui.param.value");
  CHECK(g_layers[1].binds.at("rotation").scale > 4.7f);

  // text layer
  CHECK(g_layers[2].strs.at("text") == "{name}: {value}");
  CHECK(g_layers[2].flts.at("size") == 14.0f);
  CHECK(g_layers[2].pa == NEUI_ANCHOR_BOTTOM);

  // behavior: 2 handlers, kinds preserved
  REQUIRE(g_handlers.size() == 2);
  CHECK(g_handlers[0].kind == NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL);
  CHECK(g_handlers[1].kind == NEUI_BEHAVIOR_KIND_WHEEL);
  CHECK(g_handlers[1].flts.at("step") == 0.02f);

  // minimal-diff: the all-default drag_rotational emitted no min/max/deadzone
  CHECK(g_handlers[0].flts.count("min") == 0);
  CHECK(g_handlers[0].flts.count("max") == 0);
  CHECK(g_handlers[0].flts.count("deadzone") == 0);

  // The bg asset's frame-strip layout survived the round-trip: serialize
  // re-emitted "frame_layout" and the reload re-tagged the asset (1 x 100).
  REQUIRE(g_frame_layouts.size() == 1);
  const auto& fl = g_frame_layouts.begin()->second;
  CHECK_EQ((int)fl.cols, 1);
  CHECK_EQ((int)fl.rows, 100);
}

TEST_CASE("serialize_component: round-trips an arc layer")
{
  // Hand-build a value-driven ring (stroked arc, value bound to the param).
  HandBuilt hb;
  hb.name = "ring";
  uint32_t s = compound_add_layer(hb.ca, NEUI_COMPOUND_LAYER_ARC, 1);
  CompoundLayer* L = compound_get_layer(hb.ca, s);
  L->parent_anchor = NEUI_ANCHOR_CENTER; L->self_anchor = NEUI_ANCHOR_CENTER;
  apply_set_int  (*L, "stroke_color", static_cast<int>(0xFFE0E0E0u));
  apply_set_float(*L, "stroke_width", 6.0f);
  apply_set_int  (*L, "stroke_cap",   1);          // round
  apply_set_float(*L, "begin_angle",  -120.0f);
  apply_set_float(*L, "end_angle",     120.0f);
  apply_set_int  (*L, "polarity",      1);          // center
  apply_set_int  (*L, "direction",     1);          // ccw
  apply_bind     (*L, "value", "neui.param.value", 1.0f, 0.0f);

  ComponentSerializeInput in;
  in.name = &hb.name; in.width = 80.0f; in.height = 80.0f;
  in.compound = &hb.ca; in.behavior = &hb.ba;
  std::string json = serialize_component(in, 0);

  BuiltComponent rebuilt = run_loader(json);
  CHECK(rebuilt.ok);
  REQUIRE(g_layers.size() == 1);
  const LayerRec& a = g_layers[0];
  CHECK(a.kind == NEUI_COMPOUND_LAYER_ARC);
  CHECK(a.ints.at("stroke_color") == static_cast<int>(0xFFE0E0E0u));
  CHECK(a.flts.at("stroke_width") == 6.0f);
  CHECK(a.ints.at("stroke_cap") == 1);
  CHECK(a.flts.at("begin_angle") == -120.0f);
  CHECK(a.flts.at("end_angle") == 120.0f);
  CHECK(a.ints.at("polarity") == 1);
  CHECK(a.ints.at("direction") == 1);
  // `value` is bound (not a static prop), so it round-trips as a binding.
  REQUIRE(a.binds.count("value") == 1);
  CHECK(a.binds.at("value").attr == "neui.param.value");
}

TEST_CASE("serialize_component: re-emits frame_layout for a tagged asset")
{
  HandBuilt hb = make_handbuilt();
  std::string json = serialize_component(input_for(hb), 0);

  auto root = neui::mujson::parse(json);
  REQUIRE(!root.empty());
  const auto* layers = cl_detail::obj_get(root, "layers");
  REQUIRE(layers && std::holds_alternative<neui::mujson::array_t>(layers->value));

  // Exactly one asset layer (the bg) carries a frame_layout { frames: 100 };
  // the indicator asset layer (no recorded layout) carries none.
  int frame_layout_count = 0;
  for (const auto& ln : std::get<neui::mujson::array_t>(layers->value)) {
    const auto* fl = cl_detail::obj_get(std::get<neui::mujson::object_t>(ln.value),
                                        "frame_layout");
    if (!fl) continue;
    ++frame_layout_count;
    const auto& flo = std::get<neui::mujson::object_t>(fl->value);
    const auto* frames = cl_detail::obj_get(flo, "frames");
    REQUIRE(frames != nullptr);
    CHECK(std::get<int>(frames->value) == 100);
  }
  CHECK_EQ(frame_layout_count, 1);
}

TEST_CASE("serialize_component: frame_layout is emitted once when layers share an asset")
{
  // Two asset layers reference the SAME tagged asset (id 1001). The layout
  // lives on the asset, so it must be re-emitted on exactly ONE layer - not
  // duplicated onto every layer that happens to draw that asset (which would
  // bloat the round-trip document).
  HandBuilt hb;
  hb.anames   = { { "strip", "knob_100.png" } };
  hb.hnames   = { { 1001u, "strip" } };
  hb.flayouts = { { 1001u, FilmstripLayout{ 1u, 100u, 0u } } };

  uint32_t s0 = compound_add_layer(hb.ca, NEUI_COMPOUND_LAYER_ASSET, 0);
  compound_get_layer(hb.ca, s0)->asset = neui_asset_t{ 1001u };
  uint32_t s1 = compound_add_layer(hb.ca, NEUI_COMPOUND_LAYER_ASSET, 1);
  compound_get_layer(hb.ca, s1)->asset = neui_asset_t{ 1001u };  // same asset

  std::string json = serialize_component(input_for(hb), 0);
  auto root = neui::mujson::parse(json);
  REQUIRE(!root.empty());
  const auto* layers = cl_detail::obj_get(root, "layers");
  REQUIRE(layers && std::holds_alternative<neui::mujson::array_t>(layers->value));

  int frame_layout_count = 0;
  for (const auto& ln : std::get<neui::mujson::array_t>(layers->value))
    if (cl_detail::obj_get(std::get<neui::mujson::object_t>(ln.value), "frame_layout"))
      ++frame_layout_count;
  CHECK_EQ(frame_layout_count, 1);
}

TEST_CASE("serialize_component: nests a group's children under a layers array")
{
  // Build a real group compound (group + 2 children), serialize, and confirm
  // the children land in a nested "layers" array; then round-trip it back
  // through the loader and confirm 3 layers with the right parentage.
  CompoundAsset ca;
  uint32_t g = compound_add_layer(ca, NEUI_COMPOUND_LAYER_GROUP, 0);
  compound_get_layer(ca, g)->width  = 40;
  compound_get_layer(ca, g)->height = 40;
  uint32_t c0 = compound_add_child_layer(ca, g, NEUI_COMPOUND_LAYER_RECT, 0);
  apply_set_int(*compound_get_layer(ca, c0), "fill_color",
                static_cast<int>(static_cast<uint32_t>(0xFF101010u)));
  uint32_t c1 = compound_add_child_layer(ca, g, NEUI_COMPOUND_LAYER_TEXT, 1);
  apply_set_string(*compound_get_layer(ca, c1), "text", "{value}");

  BehaviorAsset ba;
  std::string nm = "grp";
  std::vector<std::pair<std::string, std::string>> an;
  std::vector<std::pair<uint32_t, std::string>>    hn;
  ComponentSerializeInput in;
  in.name = &nm; in.width = 60.0f; in.height = 60.0f;
  in.asset_names = &an; in.asset_handle_names = &hn;
  in.compound = &ca; in.behavior = &ba;

  std::string json = serialize_component(in, 0);
  auto root = neui::mujson::parse(json);
  REQUIRE(!root.empty());
  const auto* layers = cl_detail::obj_get(root, "layers");
  REQUIRE(layers && std::holds_alternative<neui::mujson::array_t>(layers->value));
  const auto& la = std::get<neui::mujson::array_t>(layers->value);
  REQUIRE(la.size() == 1);   // only the group at the top level
  const auto& go = std::get<neui::mujson::object_t>(la[0].value);
  const auto* kind = cl_detail::obj_get(go, "kind");
  REQUIRE(kind); CHECK(std::get<std::string>(kind->value) == "group");
  const auto* kids = cl_detail::obj_get(go, "layers");
  REQUIRE(kids && std::holds_alternative<neui::mujson::array_t>(kids->value));
  CHECK_EQ((int)std::get<neui::mujson::array_t>(kids->value).size(), 2);

  // Full round-trip through the loader: 3 layers, both children under the group.
  run_loader(json);
  REQUIRE(g_layers.size() == 3);
  CHECK(g_layers[0].kind == NEUI_COMPOUND_LAYER_GROUP);
  CHECK(g_layers[1].parent == 0);
  CHECK(g_layers[2].parent == 0);
}

// Regression: a drag_biaxial handler's per-axis Y target must survive JSON
// load (target_y was previously absent from the string-prop table, so it was
// silently dropped at load while serialize still emitted it - a lossy
// round-trip for any biaxial component).
TEST_CASE("component_loader: drag_biaxial target_y survives load + round-trip")
{
  const char* kBiaxial = R"json({
    "component": "xy_pad",
    "size": [120, 120],
    "compound": [
      { "kind": "rect", "z": 0, "anchor": ["top_left","top_left"], "size": "fill",
        "fill_color": "#FF202020" }
    ],
    "behavior": [
      { "kind": "drag_biaxial", "target": "neui.param.x", "target_y": "neui.param.y",
        "sweep": 100, "sweep_y": 100 }
    ]
  })json";

  BuiltComponent c = run_loader(kBiaxial);
  CHECK(c.ok);
  REQUIRE(g_handlers.size() == 1);
  CHECK(g_handlers[0].kind == NEUI_BEHAVIOR_KIND_DRAG_BIAXIAL);
  // The fix: target_y reaches be_set_string instead of being dropped.
  REQUIRE(g_handlers[0].strs.count("target_y") == 1);
  CHECK(g_handlers[0].strs.at("target_y") == "neui.param.y");
  CHECK(g_handlers[0].strs.at("target") == "neui.param.x");

  // Round-trip: hand-build a biaxial behavior, serialize, reload, confirm
  // target_y is preserved end-to-end.
  BehaviorAsset ba;
  uint32_t h = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_BIAXIAL);
  BehaviorHandler* H = behavior_get_handler(ba, h);
  H->target   = "neui.param.x";
  H->target_y = "neui.param.y";

  CompoundAsset ca;
  std::string nm = "xy_pad";
  std::vector<std::pair<std::string, std::string>> an;
  std::vector<std::pair<uint32_t, std::string>>    hn;
  ComponentSerializeInput in;
  in.name = &nm; in.width = 120.0f; in.height = 120.0f;
  in.asset_names = &an; in.asset_handle_names = &hn;
  in.compound = &ca; in.behavior = &ba;

  std::string json = serialize_component(in, 0);
  BuiltComponent rebuilt = run_loader(json);
  CHECK(rebuilt.ok);
  REQUIRE(g_handlers.size() == 1);
  CHECK(g_handlers[0].kind == NEUI_BEHAVIOR_KIND_DRAG_BIAXIAL);
  REQUIRE(g_handlers[0].strs.count("target_y") == 1);
  CHECK(g_handlers[0].strs.at("target_y") == "neui.param.y");
}

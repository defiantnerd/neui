#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <neui/d/assets.h>
#include <neui/d/compound.h>
#include <neui/d/behavior.h>
#include <neui/d/component.h>

// Internal data models (CompoundAsset / BehaviorAsset) - needed by
// serialize_component to introspect a built component back to JSON.
#include "compound.h"
#include "behavior.h"
#include "filmstrip_recognize.h"  // filmstrip_layout_from_object
#include "mujson_accessors.h"     // obj_get / as_obj / as_arr / as_str / as_num / as_bool

// mujson lives in the core lib (src/). Hosts reach hosts/shared via relative
// includes and don't carry src/ on their include path, so reference it
// relative to this header's own location (resolves the same for the Tier-1
// test, which also has src/ on its path).
#include "../../src/mujson.h"

// Host-side component loader. Parses a JSON component document (neui::mujson)
// and materializes it into a COMPOUND (visual) + a BEHAVIOR (input) by driving
// the public compound / behavior / asset API vtables - the SAME calls a client
// would make by hand. It is host-agnostic (it only touches the passed-in api
// pointers, never a host's internal Session or asset store), so it lives once
// in hosts/shared and is compiled into every host. The host wraps the returned
// BuiltComponent in a NEUI_ASSET_KIND_COMPONENT store entry.
//
// build_component() does NOT touch the asset store, so it is unit-testable in
// isolation with fake api vtables (see tests/test_component_loader.cpp).

namespace neui_detail
{
  // --- result types -------------------------------------------------------

  // A default attr stamped onto every instance's AttrBag at instantiate time.
  struct ComponentDefaultAttr
  {
    enum Type { INT, FLOAT, STRING } type = FLOAT;
    std::string key;
    int         ival = 0;
    float       fval = 0.0f;
    std::string sval;
  };

  // One parameter-manifest entry (mirrors neui_component_param_t but owns its
  // strings). The host copies these into stable storage and points the public
  // neui_component_param_t key / label at them.
  struct ComponentParam
  {
    std::string key;
    std::string label;
    float       min = 0.0f;
    float       max = 1.0f;
    float       def = 0.0f;
  };

  // What build_component returns; the host wraps it into a COMPONENT entry.
  struct BuiltComponent
  {
    neui_asset_t                      compound = asset_none;
    neui_asset_t                      behavior = asset_none;
    std::vector<ComponentDefaultAttr> defaults;
    std::vector<ComponentParam>       params;
    std::vector<neui_asset_t>         owned_assets; // path-loaded layer assets
    float                             width  = 0.0f;
    float                             height = 0.0f;
    bool                              ok     = false;

    // Round-trip metadata (consumed by serialize_component). name is the JSON
    // "component" key; asset_names is the "assets" block (name -> path);
    // asset_handle_names maps each resolved layer-asset handle id back to the
    // name it was referenced by, so asset layers serialize by name again.
    std::string                                      name;
    std::vector<std::pair<std::string, std::string>> asset_names;
    std::vector<std::pair<uint32_t, std::string>>    asset_handle_names;
    // Frame-strip layout applied to a layer asset via a "frame_layout" block,
    // keyed by the resolved asset handle id (deduped). The layout lives on the
    // asset, not the layer, so serialize_component re-emits it from here.
    std::vector<std::pair<uint32_t, FilmstripLayout>> asset_frame_layouts;
  };

  // The api vtables the loader drives. The host passes its own pointers.
  struct ComponentApis
  {
    neui_asset_api_t*    asset    = nullptr;
    neui_compound_api_t* compound = nullptr;
    neui_behavior_api_t* behavior = nullptr;
  };

  namespace cl_detail
  {
    using mj = neui::mujson;

    // The generic mujson value accessors (obj_get / as_obj / as_arr / as_str /
    // as_num / as_bool) live in neui_detail (mujson_accessors.h) and are shared
    // with filmstrip_recognize.h. Re-export them through cl_detail so existing
    // cl_detail::-qualified call sites keep resolving; only the
    // component-specific interpreters are defined here.
    using neui_detail::obj_get;
    using neui_detail::as_obj;
    using neui_detail::as_arr;
    using neui_detail::as_str;
    using neui_detail::as_num;
    using neui_detail::as_bool;

    // Color: "#AARRGGBB" string arm, or a bare int arm.
    inline bool as_color(const mj::node* n, int& out)
    {
      if (auto* s = as_str(n)) {
        if (!s->empty() && (*s)[0] == '#') {
          out = static_cast<int>(static_cast<uint32_t>(
              std::strtoul(s->c_str() + 1, nullptr, 16)));
          return true;
        }
        return false;
      }
      double d;
      if (as_num(n, d)) { out = static_cast<int>(static_cast<int64_t>(d)); return true; }
      return false;
    }

    // --- token tables ------------------------------------------------------

    inline neui_anchor_t parse_anchor(const std::string& s)
    {
      if (s == "top_left")     return NEUI_ANCHOR_TOP_LEFT;
      if (s == "top")          return NEUI_ANCHOR_TOP;
      if (s == "top_right")    return NEUI_ANCHOR_TOP_RIGHT;
      if (s == "left")         return NEUI_ANCHOR_LEFT;
      if (s == "center")       return NEUI_ANCHOR_CENTER;
      if (s == "right")        return NEUI_ANCHOR_RIGHT;
      if (s == "bottom_left")  return NEUI_ANCHOR_BOTTOM_LEFT;
      if (s == "bottom")       return NEUI_ANCHOR_BOTTOM;
      if (s == "bottom_right") return NEUI_ANCHOR_BOTTOM_RIGHT;
      return NEUI_ANCHOR_CENTER;
    }
    inline uint32_t parse_state_token(const std::string& s)
    {
      bool neg = !s.empty() && s[0] == '!';
      std::string t = neg ? s.substr(1) : s;
      if (t == "enabled")  return neg ? NEUI_LAYER_STATE_NOT_ENABLED  : NEUI_LAYER_STATE_ENABLED;
      if (t == "hovered")  return neg ? NEUI_LAYER_STATE_NOT_HOVERED  : NEUI_LAYER_STATE_HOVERED;
      if (t == "pressed")  return neg ? NEUI_LAYER_STATE_NOT_PRESSED  : NEUI_LAYER_STATE_PRESSED;
      if (t == "selected") return neg ? NEUI_LAYER_STATE_NOT_SELECTED : NEUI_LAYER_STATE_SELECTED;
      return 0;
    }
    inline neui_compound_layer_kind_t parse_layer_kind(const std::string& s)
    {
      if (s == "text")  return NEUI_COMPOUND_LAYER_TEXT;
      if (s == "asset") return NEUI_COMPOUND_LAYER_ASSET;
      if (s == "rect")  return NEUI_COMPOUND_LAYER_RECT;
      if (s == "path")  return NEUI_COMPOUND_LAYER_PATH;
      if (s == "group") return NEUI_COMPOUND_LAYER_GROUP;
      if (s == "arc")   return NEUI_COMPOUND_LAYER_ARC;
      return NEUI_COMPOUND_LAYER_NONE;
    }
    inline neui_behavior_kind_t parse_behavior_kind(const std::string& s)
    {
      if (s == "drag_vertical")   return NEUI_BEHAVIOR_KIND_DRAG_VERTICAL;
      if (s == "drag_horizontal") return NEUI_BEHAVIOR_KIND_DRAG_HORIZONTAL;
      if (s == "drag_rotational") return NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL;
      if (s == "drag_biaxial")    return NEUI_BEHAVIOR_KIND_DRAG_BIAXIAL;
      if (s == "wheel")           return NEUI_BEHAVIOR_KIND_WHEEL;
      if (s == "key_step")        return NEUI_BEHAVIOR_KIND_KEY_STEP;
      if (s == "click_toggle")    return NEUI_BEHAVIOR_KIND_CLICK_TOGGLE;
      if (s == "click_cycle")     return NEUI_BEHAVIOR_KIND_CLICK_CYCLE;
      if (s == "click_select")    return NEUI_BEHAVIOR_KIND_CLICK_SELECT;
      if (s == "context_reset")   return NEUI_BEHAVIOR_KIND_CONTEXT_RESET;
      if (s == "drag_source")     return NEUI_BEHAVIOR_KIND_DRAG_SOURCE;
      return NEUI_BEHAVIOR_KIND_NONE;
    }
    inline int parse_align_x(const std::string& s)
    {
      if (s == "center") return 1;
      if (s == "right" || s == "end") return 2;
      return 0;
    }
    inline int parse_align_y(const std::string& s)
    {
      if (s == "center") return 1;
      if (s == "bottom" || s == "end") return 2;
      return 0;
    }

    // One size axis: "fill" -> NEUI_COMPOUND_FILL, a number -> rounded int.
    inline bool size_axis(const mj::node* n, int& out)
    {
      if (auto* s = as_str(n)) {
        if (*s == "fill") { out = NEUI_COMPOUND_FILL; return true; }
        return false;
      }
      double d;
      if (as_num(n, d)) { out = static_cast<int>(std::lround(d)); return true; }
      return false;
    }

    // Behavior prop name -> setter type. "kind" is consumed separately.
    enum PropType { P_INT, P_FLOAT, P_STRING, P_UNKNOWN };
    inline PropType behavior_prop_type(const std::string& p)
    {
      // strings
      if (p == "target" || p == "target_y" || p == "target_default" ||
          p == "snap_attr" || p == "fine_modifier" || p == "cursor" ||
          p == "drag_data_key" || p == "drag_preview_key" || p == "result_attr" ||
          p == "selected_attr")
        return P_STRING;
      // floats
      if (p == "min" || p == "max" || p == "step" || p == "coarse" ||
          p == "fine_scale" || p == "sweep" || p == "sweep_y" ||
          p == "deadzone" || p == "threshold_px")
        return P_FLOAT;
      // ints
      if (p == "wrap" || p == "allowed_actions" || p == "anchor_parent" ||
          p == "anchor_self" || p == "offset_x" || p == "offset_y" ||
          p == "width" || p == "height" || p == "drag_hot_x" || p == "drag_hot_y")
        return P_INT;
      return P_UNKNOWN;
    }

    // --- asset resolution --------------------------------------------------

    struct AssetMap
    {
      std::vector<std::pair<std::string, std::string>> entries; // name -> hint_path
      const std::string* find(const std::string& name) const
      {
        for (const auto& e : entries)
          if (e.first == name) return &e.second;
        return nullptr;
      }
    };

    inline std::string join_path(const std::string& base, const std::string& rel)
    {
      if (rel.empty()) return rel;
      // absolute? (unix "/", windows "X:\" or leading slash/backslash)
      if (rel[0] == '/' || rel[0] == '\\' ||
          (rel.size() > 1 && rel[1] == ':'))
        return rel;
      if (base.empty()) return rel;
      char last = base.back();
      if (last == '/' || last == '\\') return base + rel;
      return base + "/" + rel;
    }

    // --- reverse token tables (for serialize) ------------------------------

    inline const char* anchor_token(neui_anchor_t a)
    {
      switch (a) {
        case NEUI_ANCHOR_TOP_LEFT:     return "top_left";
        case NEUI_ANCHOR_TOP:          return "top";
        case NEUI_ANCHOR_TOP_RIGHT:    return "top_right";
        case NEUI_ANCHOR_LEFT:         return "left";
        case NEUI_ANCHOR_CENTER:       return "center";
        case NEUI_ANCHOR_RIGHT:        return "right";
        case NEUI_ANCHOR_BOTTOM_LEFT:  return "bottom_left";
        case NEUI_ANCHOR_BOTTOM:       return "bottom";
        case NEUI_ANCHOR_BOTTOM_RIGHT: return "bottom_right";
      }
      return "center";
    }
    inline const char* layer_kind_token(neui_compound_layer_kind_t k)
    {
      switch (k) {
        case NEUI_COMPOUND_LAYER_TEXT:  return "text";
        case NEUI_COMPOUND_LAYER_ASSET: return "asset";
        case NEUI_COMPOUND_LAYER_RECT:  return "rect";
        case NEUI_COMPOUND_LAYER_PATH:  return "path";
        case NEUI_COMPOUND_LAYER_GROUP: return "group";
        case NEUI_COMPOUND_LAYER_ARC:   return "arc";
        default:                        return "";
      }
    }
    inline const char* behavior_kind_token(neui_behavior_kind_t k)
    {
      switch (k) {
        case NEUI_BEHAVIOR_KIND_DRAG_VERTICAL:   return "drag_vertical";
        case NEUI_BEHAVIOR_KIND_DRAG_HORIZONTAL: return "drag_horizontal";
        case NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL: return "drag_rotational";
        case NEUI_BEHAVIOR_KIND_DRAG_BIAXIAL:    return "drag_biaxial";
        case NEUI_BEHAVIOR_KIND_WHEEL:           return "wheel";
        case NEUI_BEHAVIOR_KIND_KEY_STEP:        return "key_step";
        case NEUI_BEHAVIOR_KIND_CLICK_TOGGLE:    return "click_toggle";
        case NEUI_BEHAVIOR_KIND_CLICK_CYCLE:     return "click_cycle";
        case NEUI_BEHAVIOR_KIND_CLICK_SELECT:    return "click_select";
        case NEUI_BEHAVIOR_KIND_CONTEXT_RESET:   return "context_reset";
        case NEUI_BEHAVIOR_KIND_DRAG_SOURCE:     return "drag_source";
        default:                                 return "";
      }
    }
    inline const char* align_x_token(int v) { return v == 2 ? "right"  : v == 1 ? "center" : "left"; }
    inline const char* align_y_token(int v) { return v == 2 ? "bottom" : v == 1 ? "center" : "top"; }
    inline const char* fine_mod_token(FineModifier m)
    {
      switch (m) {
        case FineModifier::Ctrl: return "ctrl";
        case FineModifier::Alt:  return "alt";
        case FineModifier::None: return "none";
        default:                 return "shift";
      }
    }
    inline std::string hexcolor(uint32_t c)
    {
      char b[12];
      std::snprintf(b, sizeof(b), "#%08X", c);
      return std::string(b);
    }

    // --- mujson node builders ----------------------------------------------
    inline mj::node njson_str(const std::string& s) { mj::node n; n.value = s; return n; }
    inline mj::node njson_int(int i)                { mj::node n; n.value = i; return n; }
    inline mj::node njson_num(double d)             { mj::node n; n.value = d; return n; }
    inline mj::node njson_bool(bool b)              { mj::node n; n.value = b; return n; }
    inline mj::node njson_arr(mj::array_t a)        { mj::node n; n.value = std::move(a); return n; }
    inline mj::node njson_obj(mj::object_t o)       { mj::node n; n.value = std::move(o); return n; }
  } // namespace cl_detail

  // --- the build ----------------------------------------------------------

  inline BuiltComponent build_component(neui_session_t session,
                                        const char* json, uint32_t len,
                                        const neui_component_env_t* env,
                                        const ComponentApis& apis)
  {
    using namespace cl_detail;
    BuiltComponent out;
    if (!json || !apis.asset || !apis.compound || !apis.behavior) return out;

    mj::object_t root = mj::parse(std::string(json, json + len));
    if (root.empty() && *mj::getLastError()) return out; // malformed

    if (auto* nm = as_str(obj_get(root, "component"))) out.name = *nm;

    // default size
    if (const mj::array_t* sz = as_arr(obj_get(root, "size"))) {
      double w = 0, h = 0;
      if (sz->size() >= 1) as_num(&(*sz)[0], w);
      if (sz->size() >= 2) as_num(&(*sz)[1], h);
      out.width  = static_cast<float>(w);
      out.height = static_cast<float>(h);
    }

    // asset name -> hint_path map
    AssetMap amap;
    if (const mj::object_t* assets = as_obj(obj_get(root, "assets"))) {
      for (const auto& kv : *assets)
        if (auto* path = as_str(&kv.second)) {
          amap.entries.emplace_back(kv.first, *path);
          out.asset_names.emplace_back(kv.first, *path); // retained for round-trip
        }
    }

    std::string base_dir = (env && env->base_dir) ? env->base_dir : "";

    auto resolve_asset = [&](const std::string& name) -> neui_asset_t {
      const std::string* hint = amap.find(name);
      const std::string  hint_path = hint ? *hint : name;
      if (env && env->resolve_asset) {
        neui_asset_t a = env->resolve_asset(env->user, name.c_str(), hint_path.c_str());
        if (a.id != asset_none.id) {
          out.asset_handle_names.emplace_back(a.id, name); // borrowed, but name known
          return a; // borrowed - not owned by component
        }
      }
      std::string full = join_path(base_dir, hint_path);
      neui_asset_t a = apis.asset->create_from_file(session, full.c_str());
      if (a.id != asset_none.id) {
        out.owned_assets.push_back(a); // component-owned
        out.asset_handle_names.emplace_back(a.id, name);
      }
      return a;
    };

    // params -> manifest + default attrs
    if (const mj::array_t* params = as_arr(obj_get(root, "params"))) {
      for (const auto& pn : *params) {
        const mj::object_t* po = as_obj(&pn);
        if (!po) continue;
        const std::string* key = as_str(obj_get(*po, "key"));
        if (!key) continue;
        ComponentParam cp;
        cp.key = *key;
        if (auto* lbl = as_str(obj_get(*po, "label"))) cp.label = *lbl;
        double v;
        if (as_num(obj_get(*po, "min"), v)) cp.min = static_cast<float>(v);
        if (as_num(obj_get(*po, "max"), v)) cp.max = static_cast<float>(v);
        bool has_def = as_num(obj_get(*po, "default"), v);
        if (has_def) cp.def = static_cast<float>(v);
        out.params.push_back(cp);
        if (has_def) {
          ComponentDefaultAttr d;
          d.type = ComponentDefaultAttr::FLOAT;
          d.key  = cp.key;
          d.fval = cp.def;
          out.defaults.push_back(std::move(d));
        }
      }
    }

    // compound
    neui_asset_t cs = apis.asset->create_compound(session);
    if (cs.id == asset_none.id) return out; // host without compound support
    out.compound = cs;

    // Build one layer (and, for a GROUP, recurse into its nested "layers").
    // parent_layer == compound_layer_none -> top-level (add_layer); otherwise
    // the layer is a child of that GROUP (add_child_layer). Recursive, so the
    // tree can nest arbitrarily.
    std::function<void(const mj::object_t&, neui_compound_layer_t)> build_layer;
    build_layer = [&](const mj::object_t& lo_ref, neui_compound_layer_t parent_layer) {
        const mj::object_t* lo = &lo_ref;
        const std::string* kstr = as_str(obj_get(*lo, "kind"));
        if (!kstr) return;
        neui_compound_layer_kind_t kind = parse_layer_kind(*kstr);
        if (kind == NEUI_COMPOUND_LAYER_NONE) return;

        int z = 0;
        double zd;
        if (as_num(obj_get(*lo, "z"), zd)) z = static_cast<int>(std::lround(zd));

        // A host without group support leaves add_child_layer null; the child
        // is then skipped while the rest of the document still loads.
        neui_compound_layer_t layer =
          (parent_layer.id == compound_layer_none.id)
            ? apis.compound->add_layer(session, cs, kind, z)
            : (apis.compound->add_child_layer
                 ? apis.compound->add_child_layer(session, cs, parent_layer, kind, z)
                 : compound_layer_none);
        if (layer.id == compound_layer_none.id) return;

        // anchor (default center/center)
        neui_anchor_t pa = NEUI_ANCHOR_CENTER, sa = NEUI_ANCHOR_CENTER;
        if (const mj::array_t* an = as_arr(obj_get(*lo, "anchor"))) {
          if (an->size() >= 1) if (auto* s = as_str(&(*an)[0])) pa = parse_anchor(*s);
          if (an->size() >= 2) if (auto* s = as_str(&(*an)[1])) sa = parse_anchor(*s);
        }
        apis.compound->set_anchor(session, cs, layer, pa, sa);

        // offset
        if (const mj::array_t* off = as_arr(obj_get(*lo, "offset"))) {
          double ox = 0, oy = 0;
          if (off->size() >= 1) as_num(&(*off)[0], ox);
          if (off->size() >= 2) as_num(&(*off)[1], oy);
          apis.compound->set_int(session, cs, layer, "offset_x", static_cast<int>(std::lround(ox)));
          apis.compound->set_int(session, cs, layer, "offset_y", static_cast<int>(std::lround(oy)));
        }

        // size: "fill" or [w,h]
        if (const mj::node* szn = obj_get(*lo, "size")) {
          if (as_str(szn) && *as_str(szn) == "fill") {
            apis.compound->set_int(session, cs, layer, "width",  NEUI_COMPOUND_FILL);
            apis.compound->set_int(session, cs, layer, "height", NEUI_COMPOUND_FILL);
          } else if (const mj::array_t* sa2 = as_arr(szn)) {
            int wv, hv;
            if (sa2->size() >= 1 && size_axis(&(*sa2)[0], wv))
              apis.compound->set_int(session, cs, layer, "width", wv);
            if (sa2->size() >= 2 && size_axis(&(*sa2)[1], hv))
              apis.compound->set_int(session, cs, layer, "height", hv);
          }
        }

        // alpha
        double av;
        if (as_num(obj_get(*lo, "alpha"), av))
          apis.compound->set_float(session, cs, layer, "alpha", static_cast<float>(av));

        // show_when
        if (const mj::array_t* sw = as_arr(obj_get(*lo, "show_when"))) {
          uint32_t mask = 0;
          for (const auto& tn : *sw)
            if (auto* t = as_str(&tn)) mask |= parse_state_token(*t);
          apis.compound->set_int(session, cs, layer, NEUI_PROP_SHOW_WHEN, static_cast<int>(mask));
        }

        // kind-specific
        if (kind == NEUI_COMPOUND_LAYER_TEXT) {
          if (auto* t = as_str(obj_get(*lo, "text")))
            apis.compound->set_string(session, cs, layer, "text", t->c_str());
          double fs;
          if (as_num(obj_get(*lo, "font_size"), fs))
            apis.compound->set_float(session, cs, layer, "size", static_cast<float>(fs));
          int col;
          if (as_color(obj_get(*lo, "color"), col))
            apis.compound->set_int(session, cs, layer, "color", col);
          if (const mj::array_t* al = as_arr(obj_get(*lo, "align"))) {
            if (al->size() >= 1) if (auto* s = as_str(&(*al)[0]))
              apis.compound->set_int(session, cs, layer, "align_x", parse_align_x(*s));
            if (al->size() >= 2) if (auto* s = as_str(&(*al)[1]))
              apis.compound->set_int(session, cs, layer, "align_y", parse_align_y(*s));
          }
          if (auto* fam = as_str(obj_get(*lo, "family")))
            apis.compound->set_string(session, cs, layer, "family", fam->c_str());
          double wt;
          if (as_num(obj_get(*lo, "weight"), wt))
            apis.compound->set_int(session, cs, layer, "weight", static_cast<int>(std::lround(wt)));
        } else if (kind == NEUI_COMPOUND_LAYER_ASSET) {
          neui_asset_t layer_asset = asset_none;
          if (auto* name = as_str(obj_get(*lo, "asset"))) {
            layer_asset = resolve_asset(*name);
            if (layer_asset.id != asset_none.id)
              apis.compound->set_asset(session, cs, layer, "asset", layer_asset);
          }
          // "frame_layout": tag the resolved asset as a filmstrip so a later
          // "frame" prop / bind selects a cell. Same object shape as the file
          // sidecar ({frames,orientation,gutter} or {cols,rows,gutter}). The
          // applied layout is recorded (deduped by handle) so serialize_component
          // can re-emit it - the layout lives on the asset, not the layer.
          if (layer_asset.id != asset_none.id && apis.asset->set_frame_layout) {
            if (const mj::object_t* fl = as_obj(obj_get(*lo, "frame_layout"))) {
              FilmstripLayout lay;
              // Only record (for serialize re-emit) when the tag actually took:
              // a layout that doesn't fit the bitmap leaves the asset an
              // untagged plain bitmap, and re-emitting a frame_layout the
              // runtime dropped would make a load -> serialize -> reload claim a
              // strip that was never applied.
              if (filmstrip_layout_from_object(*fl, lay) &&
                  apis.asset->set_frame_layout(session, layer_asset,
                                               lay.cols, lay.rows, lay.gutter)) {
                bool seen = false;
                for (const auto& e : out.asset_frame_layouts)
                  if (e.first == layer_asset.id) { seen = true; break; }
                if (!seen) out.asset_frame_layouts.emplace_back(layer_asset.id, lay);
              }
            }
          }
          double rot;
          if (as_num(obj_get(*lo, "rotation"), rot))
            apis.compound->set_float(session, cs, layer, "rotation", static_cast<float>(rot));
          int tint;
          if (as_color(obj_get(*lo, "tint"), tint))
            apis.compound->set_int(session, cs, layer, "tint", tint);
          double frame_d;
          if (as_num(obj_get(*lo, "frame"), frame_d)) {
            // Clamp before lround: a malformed doc with a huge double would
            // overflow `long` in std::lround (UB). A frame index is a small
            // non-negative cell number; the draw path clamps the upper end to
            // the strip's last cell anyway.
            if (frame_d < 0.0) frame_d = 0.0;
            else if (frame_d > 2147483647.0) frame_d = 2147483647.0;
            apis.compound->set_int(session, cs, layer, "frame",
                                   static_cast<int>(std::lround(frame_d)));
          }
        } else if (kind == NEUI_COMPOUND_LAYER_RECT ||
                   kind == NEUI_COMPOUND_LAYER_PATH) {
          int col;
          if (as_color(obj_get(*lo, "fill_color"), col))
            apis.compound->set_int(session, cs, layer, "fill_color", col);
          if (as_color(obj_get(*lo, "stroke_color"), col))
            apis.compound->set_int(session, cs, layer, "stroke_color", col);
          double swd;
          if (as_num(obj_get(*lo, "stroke_width"), swd))
            apis.compound->set_float(session, cs, layer, "stroke_width", static_cast<float>(swd));
          // SVG fill-rule + stroke style (cap / join / miter / dash).
          if (const std::string* fr = as_str(obj_get(*lo, "fill_rule")))
            apis.compound->set_int(session, cs, layer, "fill_rule", (*fr == "evenodd") ? 1 : 0);
          if (const std::string* lc = as_str(obj_get(*lo, "stroke_linecap")))
            apis.compound->set_int(session, cs, layer, "stroke_cap",
                                   (*lc == "round") ? 1 : (*lc == "square") ? 2 : 0);
          if (const std::string* lj = as_str(obj_get(*lo, "stroke_linejoin")))
            apis.compound->set_int(session, cs, layer, "stroke_join",
                                   (*lj == "round") ? 1 : (*lj == "bevel") ? 2 : 0);
          double mld;
          if (as_num(obj_get(*lo, "stroke_miterlimit"), mld))
            apis.compound->set_float(session, cs, layer, "stroke_miter", static_cast<float>(mld));
          double dod;
          if (as_num(obj_get(*lo, "stroke_dashoffset"), dod))
            apis.compound->set_float(session, cs, layer, "stroke_dash_offset", static_cast<float>(dod));
          if (const mj::array_t* da = as_arr(obj_get(*lo, "stroke_dasharray"))) {
            std::string spec;  // transported as a string (no float-array setter)
            for (const auto& dn : *da) {
              double d;
              if (as_num(&dn, d)) { if (!spec.empty()) spec += ','; spec += std::to_string(d); }
            }
            if (!spec.empty())
              apis.compound->set_string(session, cs, layer, "stroke_dasharray", spec.c_str());
          }
          // Value-driven trim (PATH §A) / fill (RECT §B). `value` is the
          // painted/stroked fraction; `polarity` (same min/center/max enum as
          // the arc) anchors it; RECT adds a fill-axis `orientation`. Defaults
          // (value 1, polarity min, horizontal) are the no-op ones, so a doc
          // that omits them keeps the old static behaviour.
          double valnum;
          if (as_num(obj_get(*lo, "value"), valnum))
            apis.compound->set_float(session, cs, layer, "value", static_cast<float>(valnum));
          if (const std::string* ps = as_str(obj_get(*lo, "polarity")))
            apis.compound->set_int(session, cs, layer, "polarity",
                                   (*ps == "center" || *ps == "centre") ? 1 : (*ps == "max") ? 2 : 0);
          else { double pd; if (as_num(obj_get(*lo, "polarity"), pd))
            apis.compound->set_int(session, cs, layer, "polarity", static_cast<int>(std::lround(pd))); }
          if (kind == NEUI_COMPOUND_LAYER_RECT) {
            if (const std::string* os = as_str(obj_get(*lo, "orientation")))
              apis.compound->set_int(session, cs, layer, "orientation", (*os == "vertical") ? 1 : 0);
            else { double od; if (as_num(obj_get(*lo, "orientation"), od))
              apis.compound->set_int(session, cs, layer, "orientation", (od != 0.0) ? 1 : 0); }
          }
          if (kind == NEUI_COMPOUND_LAYER_RECT) {
            double cr;
            if (as_num(obj_get(*lo, "corner_radius"), cr))
              apis.compound->set_float(session, cs, layer, "corner_radius", static_cast<float>(cr));
          } else { // PATH geometry
            if (const mj::array_t* pcmds = as_arr(obj_get(*lo, "path"))) {
              std::vector<neui_path_cmd_t> cmds;
              for (const auto& cn : *pcmds) {
                const mj::object_t* co = as_obj(&cn);
                if (!co || co->empty()) continue;
                neui_path_cmd_t cmd{};
                const std::string& op = (*co)[0].first;
                const mj::node&    arg = (*co)[0].second;
                auto read_args = [&](int count) {
                  if (const mj::array_t* a = std::get_if<mj::array_t>(&arg.value))
                    for (int i = 0; i < count && i < (int)a->size(); ++i) {
                      double d; if (as_num(&(*a)[i], d)) cmd.args[i] = static_cast<float>(d);
                    }
                };
                if (op == "m")      { cmd.kind = NEUI_PATH_CMD_MOVE_TO;  read_args(2); }
                else if (op == "l") { cmd.kind = NEUI_PATH_CMD_LINE_TO;  read_args(2); }
                else if (op == "a") { cmd.kind = NEUI_PATH_CMD_ARC;      read_args(5); }
                else if (op == "c") { cmd.kind = NEUI_PATH_CMD_CUBIC_TO; read_args(6); }
                else if (op == "q") { cmd.kind = NEUI_PATH_CMD_QUAD_TO;  read_args(4); }
                else if (op == "z") { cmd.kind = NEUI_PATH_CMD_CLOSE; }
                else continue;
                cmds.push_back(cmd);
              }
              if (!cmds.empty())
                apis.compound->set_path(session, cs, layer, cmds.data(),
                                        static_cast<uint32_t>(cmds.size()));
            }
          }
        } else if (kind == NEUI_COMPOUND_LAYER_ARC) {
          int col;
          if (as_color(obj_get(*lo, "fill_color"), col))
            apis.compound->set_int(session, cs, layer, "fill_color", col);
          if (as_color(obj_get(*lo, "stroke_color"), col))
            apis.compound->set_int(session, cs, layer, "stroke_color", col);
          double swd;
          if (as_num(obj_get(*lo, "stroke_width"), swd))
            apis.compound->set_float(session, cs, layer, "stroke_width", static_cast<float>(swd));
          if (const std::string* lc = as_str(obj_get(*lo, "stroke_linecap")))
            apis.compound->set_int(session, cs, layer, "stroke_cap",
                                   (*lc == "round") ? 1 : (*lc == "square") ? 2 : 0);
          double ang;
          if (as_num(obj_get(*lo, "begin_angle"), ang))
            apis.compound->set_float(session, cs, layer, "begin_angle", static_cast<float>(ang));
          if (as_num(obj_get(*lo, "end_angle"), ang))
            apis.compound->set_float(session, cs, layer, "end_angle", static_cast<float>(ang));
          double val;
          if (as_num(obj_get(*lo, "value"), val))
            apis.compound->set_float(session, cs, layer, "value", static_cast<float>(val));
          // polarity: "min" / "center" / "max" (or 0 / 1 / 2).
          if (const std::string* ps = as_str(obj_get(*lo, "polarity")))
            apis.compound->set_int(session, cs, layer, "polarity",
                                   (*ps == "center" || *ps == "centre") ? 1 : (*ps == "max") ? 2 : 0);
          else { double pd; if (as_num(obj_get(*lo, "polarity"), pd))
            apis.compound->set_int(session, cs, layer, "polarity", static_cast<int>(std::lround(pd))); }
          // direction: "cw" / "ccw" (or 0 / 1).
          if (const std::string* ds = as_str(obj_get(*lo, "direction")))
            apis.compound->set_int(session, cs, layer, "direction", (*ds == "ccw") ? 1 : 0);
          else { double dd; if (as_num(obj_get(*lo, "direction"), dd))
            apis.compound->set_int(session, cs, layer, "direction", (dd != 0.0) ? 1 : 0); }
        }

        // bindings (numeric props + asset prop)
        if (const mj::object_t* binds = as_obj(obj_get(*lo, "bind"))) {
          for (const auto& bkv : *binds) {
            const mj::object_t* bo = as_obj(&bkv.second);
            if (!bo) continue;
            const std::string* attr = as_str(obj_get(*bo, "attr"));
            if (!attr) continue;
            if (bkv.first == "asset") {
              apis.compound->bind_asset(session, cs, layer, "asset", attr->c_str());
            } else {
              double scale = 1.0, offset = 0.0;
              as_num(obj_get(*bo, "scale"),  scale);
              as_num(obj_get(*bo, "offset"), offset);
              apis.compound->bind(session, cs, layer, bkv.first.c_str(), attr->c_str(),
                                  static_cast<float>(scale), static_cast<float>(offset));
            }
          }
        }

        // A GROUP layer's nested "layers" array becomes its children, added
        // via add_child_layer so they live in the group's coordinate frame.
        if (kind == NEUI_COMPOUND_LAYER_GROUP) {
          if (const mj::array_t* kids = as_arr(obj_get(*lo, "layers")))
            for (const auto& kn : *kids)
              if (const mj::object_t* ko = as_obj(&kn)) build_layer(*ko, layer);
        }
      };

    if (const mj::array_t* layers = as_arr(obj_get(root, "layers"))) {
      for (const auto& ln : *layers)
        if (const mj::object_t* lo = as_obj(&ln))
          build_layer(*lo, compound_layer_none);
    }

    // behavior
    neui_asset_t ba = apis.asset->create_behavior(session);
    if (ba.id != asset_none.id) {
      out.behavior = ba;
      if (const mj::array_t* handlers = as_arr(obj_get(root, "behavior"))) {
        for (const auto& hn : *handlers) {
          const mj::object_t* ho = as_obj(&hn);
          if (!ho) continue;
          const std::string* kstr = as_str(obj_get(*ho, "kind"));
          if (!kstr) continue;
          neui_behavior_kind_t bk = parse_behavior_kind(*kstr);
          if (bk == NEUI_BEHAVIOR_KIND_NONE) continue;
          neui_behavior_handler_t h = apis.behavior->add_handler(session, ba, bk);
          for (const auto& pkv : *ho) {
            if (pkv.first == "kind") continue;
            PropType pt = behavior_prop_type(pkv.first);
            if (pt == P_STRING) {
              if (auto* s = as_str(&pkv.second))
                apis.behavior->set_string(session, ba, h, pkv.first.c_str(), s->c_str());
            } else if (pt == P_FLOAT) {
              double d;
              if (as_num(&pkv.second, d))
                apis.behavior->set_float(session, ba, h, pkv.first.c_str(), static_cast<float>(d));
            } else if (pt == P_INT) {
              double d;
              if (as_num(&pkv.second, d))
                apis.behavior->set_int(session, ba, h, pkv.first.c_str(),
                                       static_cast<int>(std::lround(d)));
            }
          }
        }
      }
    }

    out.ok = true;
    return out;
  }

  // --- serialization (designer round-trip) --------------------------------

  // Everything serialize_component needs from a built COMPONENT. The host
  // resolves the compound / behavior handles to their internal structs and
  // hands them in alongside the retained round-trip metadata.
  struct ComponentSerializeInput
  {
    const std::string*                                      name = nullptr;
    float                                                   width = 0.0f;
    float                                                   height = 0.0f;
    const std::vector<ComponentParam>*                      params = nullptr;
    const std::vector<std::pair<std::string, std::string>>* asset_names = nullptr;
    const std::vector<std::pair<uint32_t, std::string>>*    asset_handle_names = nullptr;
    const std::vector<std::pair<uint32_t, FilmstripLayout>>* asset_frame_layouts = nullptr;
    const CompoundAsset*                                    compound = nullptr;
    const BehaviorAsset*                                    behavior = nullptr;
  };

  // Serialize a built component back to a JSON document string. Minimal-diff:
  // only properties that differ from their defaults are emitted (plus the
  // structural kind / anchor / size). Per-instance attr values are NOT part of
  // a component, so none are written. indent = spaces per level (0 = compact).
  inline std::string serialize_component(const ComponentSerializeInput& in,
                                         int indent = 2)
  {
    using namespace cl_detail;
    using mj = neui::mujson;
    mj::object_t root;

    if (in.name && !in.name->empty())
      root.emplace_back("component", njson_str(*in.name));

    {
      mj::array_t sz;
      sz.push_back(njson_int(static_cast<int>(std::lround(in.width))));
      sz.push_back(njson_int(static_cast<int>(std::lround(in.height))));
      root.emplace_back("size", njson_arr(std::move(sz)));
    }

    if (in.params && !in.params->empty()) {
      mj::array_t arr;
      for (const auto& p : *in.params) {
        mj::object_t po;
        po.emplace_back("key", njson_str(p.key));
        po.emplace_back("default", njson_num(p.def));
        po.emplace_back("min", njson_num(p.min));
        po.emplace_back("max", njson_num(p.max));
        if (!p.label.empty()) po.emplace_back("label", njson_str(p.label));
        arr.push_back(njson_obj(std::move(po)));
      }
      root.emplace_back("params", njson_arr(std::move(arr)));
    }

    if (in.asset_names && !in.asset_names->empty()) {
      mj::object_t ao;
      for (const auto& e : *in.asset_names) ao.emplace_back(e.first, njson_str(e.second));
      root.emplace_back("assets", njson_obj(std::move(ao)));
    }

    auto name_for_handle = [&](neui_asset_t a) -> const std::string* {
      if (in.asset_handle_names)
        for (const auto& e : *in.asset_handle_names)
          if (e.first == a.id) return &e.second;
      return nullptr;
    };

    if (in.compound) {
      // A frame-strip layout lives on the asset, not the layer, so it is only
      // re-emitted on the FIRST asset layer (in serialization order) that
      // references each tagged asset. Emitting it on every layer sharing the
      // asset would bloat the round-trip document (minimal-diff violation).
      std::vector<uint32_t> emitted_frame_layout_assets;
      // Emit the direct children of parent_slot (0 == top level) as a layers
      // array; a GROUP layer recurses to nest its own children, so a
      // load -> serialize round-trip preserves the layer tree.
      std::function<mj::array_t(uint32_t)> emit_children;
      emit_children = [&](uint32_t parent_slot) -> mj::array_t {
      mj::array_t larr;
      for (uint32_t slot : compound_sorted_children(*in.compound, parent_slot)) {
        const CompoundLayer* L = compound_get_layer(*in.compound, slot);
        if (!L) continue;
        // Compare against a default-constructed layer rather than hard-coded
        // literals, so the model's defaults (compound.h) stay the single source
        // of truth - a default change can't silently desync the serializer.
        const CompoundLayer defL{};
        mj::object_t lo;
        lo.emplace_back("kind", njson_str(layer_kind_token(L->kind)));
        if (L->z != 0) lo.emplace_back("z", njson_int(L->z));
        {
          mj::array_t an;
          an.push_back(njson_str(anchor_token(L->parent_anchor)));
          an.push_back(njson_str(anchor_token(L->self_anchor)));
          lo.emplace_back("anchor", njson_arr(std::move(an)));
        }
        if (L->width == NEUI_COMPOUND_FILL && L->height == NEUI_COMPOUND_FILL) {
          lo.emplace_back("size", njson_str("fill"));
        } else {
          mj::array_t sz;
          sz.push_back(L->width  == NEUI_COMPOUND_FILL ? njson_str("fill") : njson_int(L->width));
          sz.push_back(L->height == NEUI_COMPOUND_FILL ? njson_str("fill") : njson_int(L->height));
          lo.emplace_back("size", njson_arr(std::move(sz)));
        }
        if (L->offset_x || L->offset_y) {
          mj::array_t off;
          off.push_back(njson_int(L->offset_x));
          off.push_back(njson_int(L->offset_y));
          lo.emplace_back("offset", njson_arr(std::move(off)));
        }
        if (L->alpha != 1.0f) lo.emplace_back("alpha", njson_num(L->alpha));
        if (L->show_when != 0u) {
          mj::array_t sw;
          if (L->show_when & NEUI_LAYER_STATE_ENABLED)     sw.push_back(njson_str("enabled"));
          if (L->show_when & NEUI_LAYER_STATE_NOT_ENABLED) sw.push_back(njson_str("!enabled"));
          if (L->show_when & NEUI_LAYER_STATE_HOVERED)     sw.push_back(njson_str("hovered"));
          if (L->show_when & NEUI_LAYER_STATE_NOT_HOVERED) sw.push_back(njson_str("!hovered"));
          if (L->show_when & NEUI_LAYER_STATE_PRESSED)     sw.push_back(njson_str("pressed"));
          if (L->show_when & NEUI_LAYER_STATE_NOT_PRESSED) sw.push_back(njson_str("!pressed"));
          if (L->show_when & NEUI_LAYER_STATE_SELECTED)     sw.push_back(njson_str("selected"));
          if (L->show_when & NEUI_LAYER_STATE_NOT_SELECTED) sw.push_back(njson_str("!selected"));
          lo.emplace_back("show_when", njson_arr(std::move(sw)));
        }

        if (L->kind == NEUI_COMPOUND_LAYER_TEXT) {
          if (!L->text_template.empty())       lo.emplace_back("text", njson_str(L->text_template));
          if (L->text_size != defL.text_size)  lo.emplace_back("font_size", njson_num(L->text_size));
          if (L->text_color_set)               lo.emplace_back("color", njson_str(hexcolor(L->text_color)));
          if (L->text_align_x != defL.text_align_x || L->text_align_y != defL.text_align_y) {
            mj::array_t al;
            al.push_back(njson_str(align_x_token(L->text_align_x)));
            al.push_back(njson_str(align_y_token(L->text_align_y)));
            lo.emplace_back("align", njson_arr(std::move(al)));
          }
          if (!L->text_family_template.empty()) lo.emplace_back("family", njson_str(L->text_family_template));
          if (L->text_weight != 0)              lo.emplace_back("weight", njson_int(L->text_weight));
        } else if (L->kind == NEUI_COMPOUND_LAYER_ASSET) {
          if (L->asset.id != asset_none.id) {
            if (const std::string* nm = name_for_handle(L->asset))
              lo.emplace_back("asset", njson_str(*nm));
          }
          if (L->rotation != 0.0f)        lo.emplace_back("rotation", njson_num(L->rotation));
          if (L->tint != defL.tint)       lo.emplace_back("tint", njson_str(hexcolor(L->tint)));
          if (L->frame != defL.frame)     lo.emplace_back("frame", njson_int(L->frame));
          // Re-emit the asset's frame-strip layout (recorded at build time) so
          // a load -> serialize -> reload round-trip keeps the strip tagged.
          // Emitted in the convention-matching shape: a single-axis grid as
          // { frames[, orientation] }, a true 2D grid as { cols, rows }.
          bool fl_already_emitted = false;
          for (uint32_t id : emitted_frame_layout_assets)
            if (id == L->asset.id) { fl_already_emitted = true; break; }
          if (in.asset_frame_layouts && L->asset.id != asset_none.id &&
              !fl_already_emitted) {
            for (const auto& fl : *in.asset_frame_layouts) {
              if (fl.first != L->asset.id) continue;
              const FilmstripLayout& lay = fl.second;
              emitted_frame_layout_assets.push_back(L->asset.id);
              mj::object_t flo;
              if (lay.cols == 1u) {
                flo.emplace_back("frames", njson_int(static_cast<int>(lay.rows)));
              } else if (lay.rows == 1u) {
                flo.emplace_back("frames", njson_int(static_cast<int>(lay.cols)));
                flo.emplace_back("orientation", njson_str("horizontal"));
              } else {
                flo.emplace_back("cols", njson_int(static_cast<int>(lay.cols)));
                flo.emplace_back("rows", njson_int(static_cast<int>(lay.rows)));
              }
              if (lay.gutter != 0u)
                flo.emplace_back("gutter", njson_int(static_cast<int>(lay.gutter)));
              lo.emplace_back("frame_layout", njson_obj(std::move(flo)));
              break;
            }
          }
        } else if (L->kind == NEUI_COMPOUND_LAYER_RECT ||
                   L->kind == NEUI_COMPOUND_LAYER_PATH) {
          if (L->fill_color)   lo.emplace_back("fill_color",   njson_str(hexcolor(L->fill_color)));
          if (L->stroke_color) lo.emplace_back("stroke_color", njson_str(hexcolor(L->stroke_color)));
          if (L->stroke_width != 0.0f) lo.emplace_back("stroke_width", njson_num(L->stroke_width));
          if (L->fill_rule != 0) lo.emplace_back("fill_rule", njson_str("evenodd"));
          if (L->kind == NEUI_COMPOUND_LAYER_PATH) {
            if (L->stroke_cap != 0)
              lo.emplace_back("stroke_linecap", njson_str(L->stroke_cap == 1 ? "round" : "square"));
            if (L->stroke_join != 0)
              lo.emplace_back("stroke_linejoin", njson_str(L->stroke_join == 1 ? "round" : "bevel"));
            if (L->stroke_miter != 4.0f)
              lo.emplace_back("stroke_miterlimit", njson_num(L->stroke_miter));
            if (!L->stroke_dash.empty()) {
              mj::array_t da;
              for (float d : L->stroke_dash) da.push_back(njson_num(d));
              lo.emplace_back("stroke_dasharray", njson_arr(std::move(da)));
              if (L->stroke_dash_offset != 0.0f)
                lo.emplace_back("stroke_dashoffset", njson_num(L->stroke_dash_offset));
            }
            // §A stroke trim. `value` is normally bound (emitted in the bind
            // block); a static override + the anchor round-trip here.
            if (L->trim_value != defL.trim_value) lo.emplace_back("value", njson_num(L->trim_value));
            if (L->trim_polarity != defL.trim_polarity)
              lo.emplace_back("polarity", njson_str(L->trim_polarity == 1 ? "center"
                                                   : L->trim_polarity == 2 ? "max" : "min"));
          }
          if (L->kind == NEUI_COMPOUND_LAYER_RECT) {
            if (L->corner_radius != 0.0f)
              lo.emplace_back("corner_radius", njson_num(L->corner_radius));
            // §B value-driven fill (the linear bar). Same round-trip rule.
            if (L->rect_value != defL.rect_value) lo.emplace_back("value", njson_num(L->rect_value));
            if (L->rect_orientation != defL.rect_orientation)
              lo.emplace_back("orientation", njson_str(L->rect_orientation == 1 ? "vertical" : "horizontal"));
            if (L->rect_polarity != defL.rect_polarity)
              lo.emplace_back("polarity", njson_str(L->rect_polarity == 1 ? "center"
                                                  : L->rect_polarity == 2 ? "max" : "min"));
          }
          if (L->kind == NEUI_COMPOUND_LAYER_PATH && !L->path_cmds.empty()) {
            mj::array_t parr;
            for (const auto& c : L->path_cmds) {
              mj::object_t co;
              mj::array_t a;
              switch (c.kind) {
                case NEUI_PATH_CMD_MOVE_TO:
                  a.push_back(njson_num(c.args[0])); a.push_back(njson_num(c.args[1]));
                  co.emplace_back("m", njson_arr(std::move(a))); break;
                case NEUI_PATH_CMD_LINE_TO:
                  a.push_back(njson_num(c.args[0])); a.push_back(njson_num(c.args[1]));
                  co.emplace_back("l", njson_arr(std::move(a))); break;
                case NEUI_PATH_CMD_ARC:
                  for (int i = 0; i < 5; ++i) a.push_back(njson_num(c.args[i]));
                  co.emplace_back("a", njson_arr(std::move(a))); break;
                case NEUI_PATH_CMD_CUBIC_TO:
                  for (int i = 0; i < 6; ++i) a.push_back(njson_num(c.args[i]));
                  co.emplace_back("c", njson_arr(std::move(a))); break;
                case NEUI_PATH_CMD_QUAD_TO:
                  for (int i = 0; i < 4; ++i) a.push_back(njson_num(c.args[i]));
                  co.emplace_back("q", njson_arr(std::move(a))); break;
                case NEUI_PATH_CMD_CLOSE:
                  co.emplace_back("z", njson_bool(true)); break;
                default: continue;
              }
              parr.push_back(njson_obj(std::move(co)));
            }
            lo.emplace_back("path", njson_arr(std::move(parr)));
          }
        } else if (L->kind == NEUI_COMPOUND_LAYER_ARC) {
          if (L->fill_color)   lo.emplace_back("fill_color",   njson_str(hexcolor(L->fill_color)));
          if (L->stroke_color) lo.emplace_back("stroke_color", njson_str(hexcolor(L->stroke_color)));
          if (L->stroke_width != 0.0f) lo.emplace_back("stroke_width", njson_num(L->stroke_width));
          if (L->stroke_cap != 0)
            lo.emplace_back("stroke_linecap", njson_str(L->stroke_cap == 1 ? "round" : "square"));
          if (L->arc_begin_deg != defL.arc_begin_deg) lo.emplace_back("begin_angle", njson_num(L->arc_begin_deg));
          if (L->arc_end_deg   != defL.arc_end_deg)   lo.emplace_back("end_angle",   njson_num(L->arc_end_deg));
          // `value` is normally bound (emitted in the bind block); a static
          // override round-trips here.
          if (L->arc_value     != defL.arc_value)     lo.emplace_back("value",       njson_num(L->arc_value));
          if (L->arc_polarity  != defL.arc_polarity)
            lo.emplace_back("polarity", njson_str(L->arc_polarity == 1 ? "center" : L->arc_polarity == 2 ? "max" : "min"));
          if (L->arc_direction != defL.arc_direction)
            lo.emplace_back("direction", njson_str(L->arc_direction == 1 ? "ccw" : "cw"));
        } else if (L->kind == NEUI_COMPOUND_LAYER_GROUP) {
          // Recurse: the group's children become its own nested layers array.
          lo.emplace_back("layers", njson_arr(emit_children(slot)));
        }

        if (!L->bindings.empty()) {
          std::vector<std::string> keys;
          keys.reserve(L->bindings.size());
          for (const auto& kv : L->bindings) keys.push_back(kv.first);
          std::sort(keys.begin(), keys.end());  // deterministic output
          mj::object_t bo;
          for (const auto& k : keys) {
            const CompoundBinding& b = L->bindings.at(k);
            mj::object_t one;
            one.emplace_back("attr", njson_str(b.attr_key));
            if (!b.is_asset) {
              one.emplace_back("scale", njson_num(b.scale));
              one.emplace_back("offset", njson_num(b.offset));
            }
            bo.emplace_back(k, njson_obj(std::move(one)));
          }
          lo.emplace_back("bind", njson_obj(std::move(bo)));
        }

        larr.push_back(njson_obj(std::move(lo)));
      }
      return larr;
      };
      root.emplace_back("layers", njson_arr(emit_children(/*parent_slot*/0)));
    }

    if (in.behavior) {
      mj::array_t harr;
      for (uint32_t i = 1; i < in.behavior->handlers.size(); ++i) {
        const BehaviorHandler* H = in.behavior->handlers[i].get();
        if (!H) continue;
        // Compare against a default-constructed handler so the model's defaults
        // (behavior.h) stay the single source of truth - a default change can't
        // silently desync the serializer (the old code duplicated the literals).
        const BehaviorHandler def{};
        mj::object_t ho;
        ho.emplace_back("kind", njson_str(behavior_kind_token(H->kind)));
        if (H->target != def.target)                  ho.emplace_back("target", njson_str(H->target));
        if (H->target_default != def.target_default)  ho.emplace_back("target_default", njson_str(H->target_default));
        if (H->target_y != def.target_y)              ho.emplace_back("target_y", njson_str(H->target_y));
        if (H->snap_attr != def.snap_attr)            ho.emplace_back("snap_attr", njson_str(H->snap_attr));
        if (H->cursor != def.cursor)                  ho.emplace_back("cursor", njson_str(H->cursor));
        if (H->fine_modifier != def.fine_modifier)    ho.emplace_back("fine_modifier", njson_str(fine_mod_token(H->fine_modifier)));
        if (H->min != def.min)                        ho.emplace_back("min", njson_num(H->min));
        if (H->max != def.max)                        ho.emplace_back("max", njson_num(H->max));
        if (H->step != def.step)                      ho.emplace_back("step", njson_num(H->step));
        if (H->coarse != def.coarse)                  ho.emplace_back("coarse", njson_num(H->coarse));
        if (H->fine_scale != def.fine_scale)          ho.emplace_back("fine_scale", njson_num(H->fine_scale));
        if (H->sweep != def.sweep)                    ho.emplace_back("sweep", njson_num(H->sweep));
        if (H->sweep_y != def.sweep_y)                ho.emplace_back("sweep_y", njson_num(H->sweep_y));
        if (H->deadzone != def.deadzone)              ho.emplace_back("deadzone", njson_num(H->deadzone));
        if (H->wrap != def.wrap)                      ho.emplace_back("wrap", njson_int(H->wrap));
        if (H->threshold_px != def.threshold_px)      ho.emplace_back("threshold_px", njson_num(H->threshold_px));
        if (H->allowed_actions != def.allowed_actions) ho.emplace_back("allowed_actions", njson_int(static_cast<int>(H->allowed_actions)));
        if (H->drag_data_key != def.drag_data_key)    ho.emplace_back("drag_data_key", njson_str(H->drag_data_key));
        if (H->drag_preview_key != def.drag_preview_key) ho.emplace_back("drag_preview_key", njson_str(H->drag_preview_key));
        if (H->drag_hot_x != def.drag_hot_x)          ho.emplace_back("drag_hot_x", njson_int(H->drag_hot_x));
        if (H->drag_hot_y != def.drag_hot_y)          ho.emplace_back("drag_hot_y", njson_int(H->drag_hot_y));
        if (H->result_attr != def.result_attr)        ho.emplace_back("result_attr", njson_str(H->result_attr));
        if (H->selected_attr != def.selected_attr)    ho.emplace_back("selected_attr", njson_str(H->selected_attr));
        if (H->anchor_parent != def.anchor_parent)    ho.emplace_back("anchor_parent", njson_int(static_cast<int>(H->anchor_parent)));
        if (H->anchor_self != def.anchor_self)        ho.emplace_back("anchor_self", njson_int(static_cast<int>(H->anchor_self)));
        if (H->offset_x != def.offset_x)              ho.emplace_back("offset_x", njson_int(H->offset_x));
        if (H->offset_y != def.offset_y)              ho.emplace_back("offset_y", njson_int(H->offset_y));
        if (H->width != def.width)                    ho.emplace_back("width", njson_int(H->width));
        if (H->height != def.height)                  ho.emplace_back("height", njson_int(H->height));
        harr.push_back(njson_obj(std::move(ho)));
      }
      root.emplace_back("behavior", njson_arr(std::move(harr)));
    }

    return mj::serialize(root, indent);
  }

} // namespace neui_detail

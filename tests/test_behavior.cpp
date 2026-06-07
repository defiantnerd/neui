#include "neui_test.h"

#include "behavior_runtime.h"

using namespace neui_detail;

// ---------------------------------------------------------------------------
// Pure math helpers
// ---------------------------------------------------------------------------

TEST_CASE("behavior_clamp pins below/above the range")
{
  CHECK_APPROX(behavior_clamp(-1.0f, 0.0f, 1.0f), 0.0);
  CHECK_APPROX(behavior_clamp(2.0f, 0.0f, 1.0f), 1.0);
  CHECK_APPROX(behavior_clamp(0.4f, 0.0f, 1.0f), 0.4);
}

TEST_CASE("behavior_wrap_pi folds into (-pi, pi]")
{
  CHECK_APPROX(behavior_wrap_pi(0.0f), 0.0);
  CHECK_APPROX(behavior_wrap_pi(BEHAVIOR_TWO_PI), 0.0);
  CHECK_APPROX(behavior_wrap_pi(BEHAVIOR_PI + 1.0f), 1.0 - (double)BEHAVIOR_PI);
  CHECK_APPROX(behavior_wrap_pi(-BEHAVIOR_PI - 1.0f), (double)BEHAVIOR_PI - 1.0);
}

TEST_CASE("behavior_snap_to_steps: steps < 2 is identity")
{
  CHECK_APPROX(behavior_snap_to_steps(0.37f, 0, 0.0f, 1.0f), 0.37);
  CHECK_APPROX(behavior_snap_to_steps(0.37f, 1, 0.0f, 1.0f), 0.37);
}

TEST_CASE("behavior_snap_to_steps: 2 steps snap to the endpoints")
{
  CHECK_APPROX(behavior_snap_to_steps(0.4f, 2, 0.0f, 1.0f), 0.0);
  CHECK_APPROX(behavior_snap_to_steps(0.6f, 2, 0.0f, 1.0f), 1.0);
}

TEST_CASE("behavior_snap_to_steps: 5 steps snap to nearest quarter")
{
  CHECK_APPROX(behavior_snap_to_steps(0.10f, 5, 0.0f, 1.0f), 0.0);
  CHECK_APPROX(behavior_snap_to_steps(0.30f, 5, 0.0f, 1.0f), 0.25);
  CHECK_APPROX(behavior_snap_to_steps(0.60f, 5, 0.0f, 1.0f), 0.5);
  CHECK_APPROX(behavior_snap_to_steps(0.95f, 5, 0.0f, 1.0f), 1.0);
}

TEST_CASE("behavior_snap_to_steps: out-of-range clamps before snapping")
{
  CHECK_APPROX(behavior_snap_to_steps(-1.0f, 5, 0.0f, 1.0f), 0.0);
  CHECK_APPROX(behavior_snap_to_steps(5.0f, 5, 0.0f, 1.0f), 1.0);
}

// ---------------------------------------------------------------------------
// Value read + fine-modifier resolution
// ---------------------------------------------------------------------------

TEST_CASE("behavior_read_value: clamps the stored attr into [min,max]")
{
  BehaviorHandler H;        // defaults: target=neui.param.value, min 0, max 1
  CHECK_APPROX(behavior_read_value(H, nullptr), 0.0);   // null bag -> min

  AttrBag bag;
  bag.set_float(NEUI_PARAM_VALUE, 0.75f);
  CHECK_APPROX(behavior_read_value(H, &bag), 0.75);

  bag.set_float(NEUI_PARAM_VALUE, 9.0f);
  CHECK_APPROX(behavior_read_value(H, &bag), 1.0);      // clamped to max
}

TEST_CASE("behavior_read_steps: empty snap_attr -> 0, else reads the int attr")
{
  BehaviorHandler H;
  AttrBag bag;
  CHECK_EQ(behavior_read_steps(H, &bag), 0);            // attr absent
  bag.set_int(NEUI_ATTR_STEPS, 7);
  CHECK_EQ(behavior_read_steps(H, &bag), 7);
  H.snap_attr.clear();
  CHECK_EQ(behavior_read_steps(H, &bag), 0);            // no snap attr configured
}

TEST_CASE("behavior_fine_mul: fine_scale only when the modifier bit is set")
{
  BehaviorHandler H;        // fine_modifier defaults to Shift, fine_scale 0.2
  CHECK_APPROX(behavior_fine_mul(H, 0), 1.0);
  CHECK_APPROX(behavior_fine_mul(H, NEUI_MK_SHIFT), 0.2);
  CHECK_APPROX(behavior_fine_mul(H, NEUI_MK_CONTROL), 1.0);   // wrong modifier

  CHECK_APPROX(behavior_fine_mul_kmod(H, NEUI_KMOD_SHIFT), 0.2);
  CHECK_APPROX(behavior_fine_mul_kmod(H, NEUI_KMOD_CTRL), 1.0);

  H.fine_modifier = FineModifier::None;
  CHECK_APPROX(behavior_fine_mul(H, NEUI_MK_SHIFT), 1.0);     // None never fine
}

// ---------------------------------------------------------------------------
// behavior_write_value: snap -> clamp -> write -> change detection
// ---------------------------------------------------------------------------

TEST_CASE("behavior_write_value: writes the clamped value and reports change")
{
  BehaviorHandler H;
  AttrBag bag;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;   // callbacks left null - dispatcher must tolerate that

  CHECK(behavior_write_value(H, ctx, NEUI_PARAM_VALUE, 0.5f));
  CHECK_APPROX(bag.get_float(NEUI_PARAM_VALUE, -1.0f), 0.5);

  // Writing the same value again is a no-op.
  CHECK_FALSE(behavior_write_value(H, ctx, NEUI_PARAM_VALUE, 0.5f));

  // Out-of-range writes clamp.
  CHECK(behavior_write_value(H, ctx, NEUI_PARAM_VALUE, 5.0f));
  CHECK_APPROX(bag.get_float(NEUI_PARAM_VALUE, -1.0f), 1.0);
}

TEST_CASE("behavior_write_value: honours the steps attr (snap before store)")
{
  BehaviorHandler H;
  AttrBag bag;
  bag.set_int(NEUI_ATTR_STEPS, 3);   // detents at 0, 0.5, 1 over [0,1]
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;

  CHECK(behavior_write_value(H, ctx, NEUI_PARAM_VALUE, 0.6f));
  CHECK_APPROX(bag.get_float(NEUI_PARAM_VALUE, -1.0f), 0.5);

  CHECK(behavior_write_value(H, ctx, NEUI_PARAM_VALUE, 0.9f));
  CHECK_APPROX(bag.get_float(NEUI_PARAM_VALUE, -1.0f), 1.0);
}

// ---------------------------------------------------------------------------
// Hit-region resolution (compound-style anchors)
// ---------------------------------------------------------------------------

TEST_CASE("behavior_hit_test: default rect fills the whole widget")
{
  BehaviorHandler H;   // width/height default to NEUI_COMPOUND_FILL
  CHECK(behavior_hit_test(H, 100, 50, 0, 0));
  CHECK(behavior_hit_test(H, 100, 50, 99, 49));
  CHECK_FALSE(behavior_hit_test(H, 100, 50, 100, 25));   // right edge exclusive
  CHECK_FALSE(behavior_hit_test(H, 100, 50, 50, 50));    // bottom edge exclusive
}

TEST_CASE("behavior_hit_test: explicit sub-rect via anchor + size")
{
  BehaviorHandler H;
  H.width = 20;
  H.height = 20;
  H.anchor_parent = NEUI_ANCHOR_CENTER;
  H.anchor_self = NEUI_ANCHOR_CENTER;
  // Centered 20x20 in a 100x100 widget -> rect [40,60) x [40,60).
  CHECK(behavior_hit_test(H, 100, 100, 50, 50));
  CHECK(behavior_hit_test(H, 100, 100, 40, 40));
  CHECK_FALSE(behavior_hit_test(H, 100, 100, 39, 50));
  CHECK_FALSE(behavior_hit_test(H, 100, 100, 60, 60));
}

// ---------------------------------------------------------------------------
// DRAG_SOURCE dispatch: arm on BUTTON_DOWN, fire begin_drag past threshold,
// stay quiet under threshold, re-entry safe.
// ---------------------------------------------------------------------------

namespace {
  struct DragSourceProbe {
    int call_count = 0;
    uint32_t last_item_id = 0;
    uint32_t last_allowed = 0;
    uint32_t last_preview = 0;
    int      last_hot_x   = 0;
    int      last_hot_y   = 0;
    uint32_t result = NEUI_DND_ACTION_COPY;
  };

  uint32_t probe_begin_drag(void* host_data, neui_data_item_t item,
                             uint32_t allowed, uint32_t preview,
                             int hot_x, int hot_y)
  {
    auto* p = static_cast<DragSourceProbe*>(host_data);
    p->call_count++;
    p->last_item_id = item.id;
    p->last_allowed = allowed;
    p->last_preview = preview;
    p->last_hot_x   = hot_x;
    p->last_hot_y   = hot_y;
    return p->result;
  }
}

TEST_CASE("DRAG_SOURCE: arms on BUTTON_DOWN but waits under threshold")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px = 5.0f;
  H->allowed_actions = NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE;

  BehaviorRuntime rt;
  AttrBag bag;
  DragSourceProbe probe;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 100; ctx.widget_h = 100;
  ctx.host_data = &probe;
  ctx.begin_drag = &probe_begin_drag;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 10, 10, NEUI_MK_LBUTTON };
  CHECK(behavior_dispatch_mouse(ba, rt, ctx, &down, 10, 10));
  CHECK(rt.dragging);

  // Move 3px - below threshold, no fire.
  neui_event_t move1 = { NEUI_EVENT_MOUSE_MOVE };
  move1.data.mouse = { wid, 13, 10, NEUI_MK_LBUTTON };
  CHECK(behavior_dispatch_mouse(ba, rt, ctx, &move1, 13, 10));
  CHECK_EQ(probe.call_count, 0);
  CHECK(rt.dragging);
}

TEST_CASE("DRAG_SOURCE: fires begin_drag once past threshold and clears state")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px = 5.0f;
  H->allowed_actions = NEUI_DND_ACTION_COPY;
  H->drag_data_key  = "myapp.drag_item";

  BehaviorRuntime rt;
  AttrBag bag;
  bag.set_int(H->drag_data_key, 0x1234);   // pre-staged item id
  DragSourceProbe probe;
  probe.result = NEUI_DND_ACTION_MOVE;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 100; ctx.widget_h = 100;
  ctx.host_data = &probe;
  ctx.begin_drag = &probe_begin_drag;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 10, 10, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &down, 10, 10);

  // Move 10px - past threshold, fires.
  neui_event_t move = { NEUI_EVENT_MOUSE_MOVE };
  move.data.mouse = { wid, 20, 10, NEUI_MK_LBUTTON };
  CHECK(behavior_dispatch_mouse(ba, rt, ctx, &move, 20, 10));
  CHECK_EQ(probe.call_count, 1);
  CHECK_EQ(probe.last_item_id, 0x1234u);
  CHECK_EQ(probe.last_allowed, (uint32_t)NEUI_DND_ACTION_COPY);
  // Drag state cleared before the (blocking) begin_drag returned - any
  // re-entrant MOVE arriving while the OS drag loop spun cannot fire
  // a second begin_drag.
  CHECK_FALSE(rt.dragging);

  // Another MOVE after fire is ignored (no handler armed).
  CHECK_FALSE(behavior_dispatch_mouse(ba, rt, ctx, &move, 25, 10));
  CHECK_EQ(probe.call_count, 1);
}

TEST_CASE("DRAG_SOURCE: empty drag_data_key sends data_item_none")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px = 2.0f;

  BehaviorRuntime rt;
  AttrBag bag;
  DragSourceProbe probe;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 100; ctx.widget_h = 100;
  ctx.host_data = &probe;
  ctx.begin_drag = &probe_begin_drag;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 0, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &down, 0, 0);
  neui_event_t move = { NEUI_EVENT_MOUSE_MOVE };
  move.data.mouse = { wid, 5, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &move, 5, 0);
  CHECK_EQ(probe.call_count, 1);
  CHECK_EQ(probe.last_item_id, neui_data_item_none.id);
}

TEST_CASE("DRAG_SOURCE: BUTTON_UP before threshold just cancels")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px = 100.0f;   // never reached in this test

  BehaviorRuntime rt;
  AttrBag bag;
  DragSourceProbe probe;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 200; ctx.widget_h = 200;
  ctx.host_data = &probe;
  ctx.begin_drag = &probe_begin_drag;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 10, 10, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &down, 10, 10);
  CHECK(rt.dragging);

  neui_event_t up = { NEUI_EVENT_MOUSE_BUTTON_UP };
  up.data.mouse = { wid, 15, 15, 0 };
  CHECK(behavior_dispatch_mouse(ba, rt, ctx, &up, 15, 15));
  CHECK_FALSE(rt.dragging);
  CHECK_EQ(probe.call_count, 0);
}

// ---------------------------------------------------------------------------
// DRAG_SOURCE preview wiring: drag_preview_key resolves from AttrBag,
// hot-spot props forward verbatim, defaults preserve "no preview".
// ---------------------------------------------------------------------------

TEST_CASE("DRAG_SOURCE preview: defaults forward 0 preview, -1 hot-spot")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px = 2.0f;

  BehaviorRuntime rt;
  AttrBag bag;
  DragSourceProbe probe;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 100; ctx.widget_h = 100;
  ctx.host_data = &probe;
  ctx.begin_drag = &probe_begin_drag;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 0, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &down, 0, 0);
  neui_event_t move = { NEUI_EVENT_MOUSE_MOVE };
  move.data.mouse = { wid, 5, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &move, 5, 0);
  CHECK_EQ(probe.call_count, 1);
  CHECK_EQ(probe.last_preview, 0u);
  CHECK_EQ(probe.last_hot_x, -1);
  CHECK_EQ(probe.last_hot_y, -1);
}

TEST_CASE("DRAG_SOURCE preview: drag_preview_key resolves from AttrBag")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px       = 2.0f;
  H->drag_preview_key   = "myapp.preview";
  H->drag_hot_x         = 8;
  H->drag_hot_y         = 12;

  BehaviorRuntime rt;
  AttrBag bag;
  bag.set_int(H->drag_preview_key, 0xCAFE);
  DragSourceProbe probe;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 100; ctx.widget_h = 100;
  ctx.host_data = &probe;
  ctx.begin_drag = &probe_begin_drag;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 0, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &down, 0, 0);
  neui_event_t move = { NEUI_EVENT_MOUSE_MOVE };
  move.data.mouse = { wid, 5, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &move, 5, 0);
  CHECK_EQ(probe.call_count, 1);
  CHECK_EQ(probe.last_preview, 0xCAFEu);
  CHECK_EQ(probe.last_hot_x, 8);
  CHECK_EQ(probe.last_hot_y, 12);
}

TEST_CASE("DRAG_SOURCE preview: missing attr value forwards 0 preview")
{
  // drag_preview_key set, but the AttrBag doesn't actually hold that key -
  // we treat it as "no preview" (id 0) and don't pass a stale value through.
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px     = 2.0f;
  H->drag_preview_key = "myapp.preview";   // attr absent below

  BehaviorRuntime rt;
  AttrBag bag;
  DragSourceProbe probe;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 100; ctx.widget_h = 100;
  ctx.host_data = &probe;
  ctx.begin_drag = &probe_begin_drag;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 0, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &down, 0, 0);
  neui_event_t move = { NEUI_EVENT_MOUSE_MOVE };
  move.data.mouse = { wid, 5, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &move, 5, 0);
  CHECK_EQ(probe.call_count, 1);
  CHECK_EQ(probe.last_preview, 0u);
}

TEST_CASE("DRAG_SOURCE preview: prop setters wire to handler fields")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);

  apply_behavior_set_string(*H, "drag_preview_key", "myapp.thumb");
  apply_behavior_set_int   (*H, "drag_hot_x", 5);
  apply_behavior_set_int   (*H, "drag_hot_y", 7);

  CHECK_EQ(H->drag_preview_key, std::string("myapp.thumb"));
  CHECK_EQ(H->drag_hot_x, 5);
  CHECK_EQ(H->drag_hot_y, 7);
}

// ---------------------------------------------------------------------------
// DRAG_SOURCE result_attr: runtime writes the negotiated action as int and
// fires emit_attr_changed + invalidate after begin_drag returns.
// ---------------------------------------------------------------------------

namespace {
  struct ResultProbe {
    int attr_changed_calls = 0;
    int invalidate_calls   = 0;
    std::string last_attr_key;
    float       last_attr_value = 0.0f;
  };

  void result_emit_attr_changed(void* host_data, const char* attr_key, float value)
  {
    auto* p = static_cast<ResultProbe*>(host_data);
    p->attr_changed_calls++;
    p->last_attr_key   = attr_key ? attr_key : "";
    p->last_attr_value = value;
  }
  void result_invalidate(void* host_data)
  {
    auto* p = static_cast<ResultProbe*>(host_data);
    p->invalidate_calls++;
  }
}

TEST_CASE("DRAG_SOURCE result_attr: negotiated action lands in the bag + ATTR_CHANGED fires")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px = 2.0f;
  H->result_attr  = "drag.result";

  // Use two separate probe structs so host_data has one type at a time;
  // glue them via a tiny shim that forwards to both. The simpler approach:
  // reuse DragSourceProbe for begin_drag and put the result-receiver in
  // host_data, then chain by hand inside the test.
  struct Glue { DragSourceProbe ds; ResultProbe rp; } glue;
  glue.ds.result = NEUI_DND_ACTION_MOVE;

  auto begin_drag = +[](void* hd, neui_data_item_t item, uint32_t allowed,
                         uint32_t preview, int hx, int hy) -> uint32_t
  {
    auto* g = static_cast<Glue*>(hd);
    g->ds.call_count++;
    g->ds.last_item_id = item.id;
    g->ds.last_allowed = allowed;
    g->ds.last_preview = preview;
    g->ds.last_hot_x   = hx;
    g->ds.last_hot_y   = hy;
    return g->ds.result;
  };
  auto on_attr_changed = +[](void* hd, const char* k, float v) {
    auto* g = static_cast<Glue*>(hd);
    g->rp.attr_changed_calls++;
    g->rp.last_attr_key   = k ? k : "";
    g->rp.last_attr_value = v;
  };
  auto on_invalidate = +[](void* hd) {
    auto* g = static_cast<Glue*>(hd);
    g->rp.invalidate_calls++;
  };

  BehaviorRuntime rt;
  AttrBag bag;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 100; ctx.widget_h = 100;
  ctx.host_data         = &glue;
  ctx.begin_drag        = begin_drag;
  ctx.emit_attr_changed = on_attr_changed;
  ctx.invalidate        = on_invalidate;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 0, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &down, 0, 0);
  // invalidate may also fire incidentally; capture the baseline so we can
  // assert the begin_drag-driven invalidate added one more.
  int invalidate_baseline = glue.rp.invalidate_calls;
  neui_event_t move = { NEUI_EVENT_MOUSE_MOVE };
  move.data.mouse = { wid, 5, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &move, 5, 0);

  CHECK_EQ(glue.ds.call_count, 1);
  CHECK_EQ(bag.get_int("drag.result", -1), (int)NEUI_DND_ACTION_MOVE);
  CHECK_EQ(glue.rp.attr_changed_calls, 1);
  CHECK_EQ(glue.rp.last_attr_key, std::string("drag.result"));
  CHECK_APPROX(glue.rp.last_attr_value, (double)NEUI_DND_ACTION_MOVE);
  CHECK(glue.rp.invalidate_calls > invalidate_baseline);
}

TEST_CASE("DRAG_SOURCE result_attr: NONE on cancel still writes + fires")
{
  // begin_drag returning NEUI_DND_ACTION_NONE (0) is a real signal
  // ("user cancelled"), not a silent path. The runtime writes 0.
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px = 2.0f;
  H->result_attr  = "drag.result";

  DragSourceProbe ds;
  ds.result = NEUI_DND_ACTION_NONE;
  ResultProbe rp;
  struct Pair { DragSourceProbe* ds; ResultProbe* rp; } pair { &ds, &rp };

  auto begin_drag = +[](void* hd, neui_data_item_t, uint32_t, uint32_t,
                         int, int) -> uint32_t
  {
    auto* p = static_cast<Pair*>(hd);
    p->ds->call_count++;
    return p->ds->result;
  };
  auto on_attr_changed = +[](void* hd, const char* k, float v) {
    auto* p = static_cast<Pair*>(hd);
    p->rp->attr_changed_calls++;
    p->rp->last_attr_value = v;
    p->rp->last_attr_key   = k ? k : "";
  };

  BehaviorRuntime rt;
  AttrBag bag;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 100; ctx.widget_h = 100;
  ctx.host_data         = &pair;
  ctx.begin_drag        = begin_drag;
  ctx.emit_attr_changed = on_attr_changed;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 0, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &down, 0, 0);
  neui_event_t move = { NEUI_EVENT_MOUSE_MOVE };
  move.data.mouse = { wid, 5, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &move, 5, 0);

  CHECK_EQ(ds.call_count, 1);
  CHECK_EQ(bag.get_int("drag.result", -1), 0);
  CHECK_EQ(rp.attr_changed_calls, 1);
  CHECK_APPROX(rp.last_attr_value, 0.0);
}

TEST_CASE("DRAG_SOURCE result_attr: empty/unset drops the action silently")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  H->threshold_px = 2.0f;
  // result_attr left empty.

  DragSourceProbe ds;
  ds.result = NEUI_DND_ACTION_LINK;
  ResultProbe rp;
  struct Pair { DragSourceProbe* ds; ResultProbe* rp; } pair { &ds, &rp };

  auto begin_drag = +[](void* hd, neui_data_item_t, uint32_t, uint32_t,
                         int, int) -> uint32_t
  {
    auto* p = static_cast<Pair*>(hd);
    p->ds->call_count++;
    return p->ds->result;
  };
  auto on_attr_changed = +[](void* hd, const char*, float) {
    auto* p = static_cast<Pair*>(hd);
    p->rp->attr_changed_calls++;
  };

  BehaviorRuntime rt;
  AttrBag bag;
  BehaviorDispatchCtx ctx;
  ctx.bag = &bag;
  ctx.widget_w = 100; ctx.widget_h = 100;
  ctx.host_data         = &pair;
  ctx.begin_drag        = begin_drag;
  ctx.emit_attr_changed = on_attr_changed;

  neui_widget_t wid = { 1 };
  neui_event_t down = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
  down.data.mouse = { wid, 0, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &down, 0, 0);
  neui_event_t move = { NEUI_EVENT_MOUSE_MOVE };
  move.data.mouse = { wid, 5, 0, NEUI_MK_LBUTTON };
  behavior_dispatch_mouse(ba, rt, ctx, &move, 5, 0);

  CHECK_EQ(ds.call_count, 1);
  CHECK_FALSE(bag.has("drag.result"));
  CHECK_EQ(rp.attr_changed_calls, 0);
}

TEST_CASE("DRAG_SOURCE result_attr: prop setter accepts the string key")
{
  BehaviorAsset ba;
  uint32_t slot = behavior_add_handler(ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
  BehaviorHandler* H = behavior_get_handler(ba, slot);
  apply_behavior_set_string(*H, "result_attr", "myapp.last_drop_action");
  CHECK_EQ(H->result_attr, std::string("myapp.last_drop_action"));
}

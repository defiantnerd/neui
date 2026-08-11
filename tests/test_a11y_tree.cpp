// Tier-1 coverage for the portable accessibility node model
// (hosts/shared/a11y_tree.h).
//
// This is where the real coverage for the accessibility wave lives. The platform
// providers are thin - they answer queries out of the tree this file builds - so
// a defect here shows up identically on macOS, Windows and Linux, and two of the
// invariants below are the ones that would crash an assistive technology rather
// than merely misinform it:
//
//   * PARENTAGE MUST BE CONSISTENT. Every node's parent must exist, and the
//     children lists must be exactly the inverse of the parent links. A provider
//     walks both directions; an orphan or a duplicate is a crash, not a wrong
//     announcement.
//   * MALFORMED INPUT MUST TERMINATE. A parent cycle or a self-parent has to
//     produce a (possibly empty) tree, never a hang - a hung accessibility query
//     freezes the screen reader, which for its user freezes the machine.
//
// Everything else is about not lying to the user: a tri-state checkbox must not
// report a definite "checked", a button must not report a value, a normalized
// 0.42 must not be announced when a real-world range was declared, and a stale
// node reference must resolve to nothing rather than to whatever widget later
// took the slot.

#include "neui_test.h"
#include <cmath>   // std::nanf for the NaN cases

#include "a11y_tree.h"

using namespace neui_detail;

namespace {

// A widget row. Ids are (widget_id, generation 1, kind widget, index -1).
A11yInput widget_row(uint32_t id, uint32_t parent, const char* type,
                     int x = 0, int y = 0, int w = 40, int h = 20)
{
  A11yInput r;
  r.id = { id, 1, 0, -1 };
  if (parent) r.parent = { parent, 1, 0, -1 };
  r.type = type;
  r.x = x; r.y = y; r.w = w; r.h = h;
  return r;
}

// A sub-element row belonging to `owner`.
A11yInput sub_row(uint32_t owner, A11ySubKind kind, int index,
                  const char* text, int x = 0, int y = 0, int w = 40, int h = 10)
{
  A11yInput r;
  r.id     = { owner, 1, static_cast<int32_t>(kind), index };
  r.parent = { owner, 1, 0, -1 };
  r.text = text;
  r.x = x; r.y = y; r.w = w; r.h = h;
  return r;
}

const A11yNode* find_widget(const std::vector<A11yNode>& nodes, uint32_t id)
{
  for (const auto& n : nodes)
    if (n.id.widget_id == id && n.id.sub_index == -1) return &n;
  return nullptr;
}

size_t count_children(const std::vector<A11yNode>& nodes, uint32_t parent_id)
{
  const A11yNode* p = find_widget(nodes, parent_id);
  return p ? p->children.size() : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Structural invariants

TEST_CASE("a11y: children are exactly the inverse of parent links")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  in.push_back(widget_row(2, 1, NEUI_W_SECTION,   0, 0, 200, 100));
  in.push_back(widget_row(3, 2, NEUI_W_BUTTON));
  in.push_back(widget_row(4, 2, NEUI_W_BUTTON));
  in.push_back(widget_row(5, 1, NEUI_W_LABEL));
  auto nodes = build_a11y_tree(in);

  CHECK_EQ(nodes.size(), (size_t)5);

  // Every non-root parent resolves, and each node appears in exactly one
  // children list - the invariant a provider's two-way walk depends on.
  for (const auto& n : nodes) {
    if (a11y_id_null(n.parent)) continue;
    CHECK(a11y_find(nodes, n.parent) != nullptr);
    size_t appearances = 0;
    for (const auto& p : nodes)
      for (const auto& c : p.children)
        if (a11y_id_equal(c, n.id)) ++appearances;
    CHECK_EQ(appearances, (size_t)1);
  }
  CHECK_EQ(count_children(nodes, 1), (size_t)2);   // section + label
  CHECK_EQ(count_children(nodes, 2), (size_t)2);   // two buttons
}

TEST_CASE("a11y: a parent cycle terminates instead of hanging")
{
  // Malformed input: 2's parent is 3 and 3's parent is 2. A hung accessibility
  // query freezes the screen reader, so termination is the contract.
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 100, 100));
  A11yInput a = widget_row(2, 3, NEUI_W_BUTTON);
  A11yInput b = widget_row(3, 2, NEUI_W_BUTTON);
  in.push_back(a);
  in.push_back(b);
  auto nodes = build_a11y_tree(in);
  CHECK(nodes.size() >= 1);            // reached here at all == did not hang
  CHECK(find_widget(nodes, 1) != nullptr);
}

TEST_CASE("a11y: a self-parent terminates")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 1, NEUI_W_BUTTON));   // parent == self
  auto nodes = build_a11y_tree(in);
  CHECK_EQ(nodes.size(), (size_t)1);
}

TEST_CASE("a11y: empty input yields an empty tree")
{
  std::vector<A11yInput> in;
  CHECK_EQ(build_a11y_tree(in).size(), (size_t)0);
}

TEST_CASE("a11y: a nonexistent parent is treated as a frame root")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(7, 99, NEUI_W_BUTTON));   // 99 never emitted
  auto nodes = build_a11y_tree(in);
  REQUIRE_EQ(nodes.size(), (size_t)1);
  CHECK(a11y_id_null(nodes[0].parent));
}

// ---------------------------------------------------------------------------
// Pruning

TEST_CASE("a11y: ROLE_NONE prunes the whole subtree, not just the node")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  A11yInput deco = widget_row(2, 1, NEUI_W_CUSTOMDRAW, 0, 0, 200, 100);
  deco.declared_role = NEUI_A11Y_ROLE_NONE;
  in.push_back(deco);
  in.push_back(widget_row(3, 2, NEUI_W_BUTTON));   // child of the pruned node
  auto nodes = build_a11y_tree(in);

  CHECK_EQ(nodes.size(), (size_t)1);
  CHECK(find_widget(nodes, 2) == nullptr);
  CHECK(find_widget(nodes, 3) == nullptr);
}

TEST_CASE("a11y: invisible and zero-size rows are dropped")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  A11yInput hidden = widget_row(2, 1, NEUI_W_BUTTON);
  hidden.visible = false;
  in.push_back(hidden);
  in.push_back(widget_row(3, 1, NEUI_W_BUTTON, 0, 0, 0, 0));   // zero size
  auto nodes = build_a11y_tree(in);
  CHECK_EQ(nodes.size(), (size_t)1);
}

TEST_CASE("a11y: a 0x0 menu container survives the zero-size rule")
{
  // MENUBAR / POPUPMENU widgets are created 0x0 and their ITEMS carry the
  // geometry, so the generic zero-size drop would delete the menu the role
  // table promises.
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  in.push_back(widget_row(2, 1, NEUI_W_MENUBAR, 0, 0, 0, 0));
  in.push_back(sub_row(2, A11ySubKind::menu_item, 0, "File", 0, 0, 40, 20));
  auto nodes = build_a11y_tree(in);

  const A11yNode* mb = find_widget(nodes, 2);
  CHECK(mb != nullptr);
  CHECK_EQ(mb->role, NEUI_A11Y_ROLE_MENU_BAR);
  CHECK_EQ(mb->children.size(), (size_t)1);
}

TEST_CASE("a11y: a ZERO-SIZE container is dropped but its children survive")
{
  // The distinction the paint walk makes and this model has to match: painting
  // is gated on size (host.cpp:2470) but DESCENT only on visibility (:2465), so
  // a zero-size container's children ARE on screen. Dropping them with their
  // parent would hide real, visible controls from an assistive technology.
  //
  // This is also the case that makes the nearest-surviving-ancestor walk
  // reachable: the button's recorded parent no longer exists in the tree, so it
  // must re-attach to the frame rather than become an orphan (which is a
  // provider crash, not a wrong announcement).
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  in.push_back(widget_row(2, 1, NEUI_W_SECTION, 0, 0, 0, 0));   // zero size
  in.push_back(widget_row(3, 2, NEUI_W_BUTTON, 10, 10, 40, 20));  // child of it
  auto nodes = build_a11y_tree(in);

  CHECK(find_widget(nodes, 2) == nullptr);          // the container is gone
  const A11yNode* btn = find_widget(nodes, 3);
  CHECK(btn != nullptr);                            // but the control is not
  CHECK_EQ(btn->parent.widget_id, (uint32_t)1);     // re-parented to the frame
  CHECK_EQ(count_children(nodes, 1), (size_t)1);
  // And it is reachable by hit-test at its real position.
  const A11yNode* hit = a11y_hit_test(nodes, 20, 15);
  CHECK(hit != nullptr);
  CHECK_EQ(hit->id.widget_id, (uint32_t)3);
}

TEST_CASE("a11y: an INVISIBLE container takes its subtree with it")
{
  // The other half of the distinction: the paint walk does not descend into an
  // invisible parent, so nothing underneath is on screen and the whole subtree
  // must go - unlike the zero-size case above.
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  A11yInput hidden = widget_row(2, 1, NEUI_W_SECTION, 0, 0, 200, 100);
  hidden.visible = false;
  in.push_back(hidden);
  in.push_back(widget_row(3, 2, NEUI_W_BUTTON, 10, 10, 40, 20));
  auto nodes = build_a11y_tree(in);

  CHECK_EQ(nodes.size(), (size_t)1);
  CHECK(find_widget(nodes, 3) == nullptr);
}

TEST_CASE("a11y: a ROLE_NONE container takes its subtree with it, several deep")
{
  // Declared decoration prunes everything beneath it, however deep - the
  // fixed-point propagation, not just one level.
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  A11yInput deco = widget_row(2, 1, NEUI_W_SECTION, 0, 0, 200, 100);
  deco.declared_role = NEUI_A11Y_ROLE_NONE;
  in.push_back(deco);
  in.push_back(widget_row(3, 2, NEUI_W_SECTION, 0, 0, 100, 50));
  in.push_back(widget_row(4, 3, NEUI_W_BUTTON, 5, 5, 40, 20));
  auto nodes = build_a11y_tree(in);

  CHECK_EQ(nodes.size(), (size_t)1);
  CHECK(find_widget(nodes, 4) == nullptr);   // three levels down, still pruned
}

TEST_CASE("a11y: subtree pruning does not depend on parents preceding children")
{
  // The adapter emits in tree order today. A model that silently relied on that
  // would break the first time an adapter batched sub-elements, so the
  // propagation is a fixed point - prove it with the input REVERSED.
  std::vector<A11yInput> in;
  in.push_back(widget_row(4, 3, NEUI_W_BUTTON, 5, 5, 40, 20));
  in.push_back(widget_row(3, 2, NEUI_W_SECTION, 0, 0, 100, 50));
  A11yInput deco = widget_row(2, 1, NEUI_W_SECTION, 0, 0, 200, 100);
  deco.declared_role = NEUI_A11Y_ROLE_NONE;
  in.push_back(deco);
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  auto nodes = build_a11y_tree(in);

  REQUIRE_EQ(nodes.size(), (size_t)1);
  CHECK_EQ(nodes[0].id.widget_id, (uint32_t)1);
}

// ---------------------------------------------------------------------------
// Roles

TEST_CASE("a11y: default roles come from the widget type")
{
  struct { const char* type; int role; } cases[] = {
    { NEUI_W_APPWINDOW,  NEUI_A11Y_ROLE_WINDOW },
    { NEUI_W_PLUGWINDOW, NEUI_A11Y_ROLE_WINDOW },
    { NEUI_W_DIALOG,     NEUI_A11Y_ROLE_WINDOW },
    { NEUI_W_LABEL,      NEUI_A11Y_ROLE_STATIC_TEXT },
    { NEUI_W_BUTTON,     NEUI_A11Y_ROLE_BUTTON },
    { NEUI_W_INPUTBOX,   NEUI_A11Y_ROLE_TEXT_FIELD },
    { NEUI_W_MULTILINE,  NEUI_A11Y_ROLE_TEXT_AREA },
    { NEUI_W_CHECKBOX,   NEUI_A11Y_ROLE_CHECKBOX },
    { NEUI_W_CHECKBOX3,  NEUI_A11Y_ROLE_CHECKBOX },
    { NEUI_W_LISTBOX,    NEUI_A11Y_ROLE_LIST },
    { NEUI_W_COMBOBOX,   NEUI_A11Y_ROLE_COMBOBOX },
    { NEUI_W_TREEVIEW,   NEUI_A11Y_ROLE_TREE },
    { NEUI_W_GRID,       NEUI_A11Y_ROLE_TABLE },
    { NEUI_W_MENUBAR,    NEUI_A11Y_ROLE_MENU_BAR },
    { NEUI_W_POPUPMENU,  NEUI_A11Y_ROLE_MENU },
    { NEUI_W_IMAGE,      NEUI_A11Y_ROLE_IMAGE },
    { NEUI_W_TABVIEW,    NEUI_A11Y_ROLE_TAB_LIST },
    { NEUI_W_TABPAGE,    NEUI_A11Y_ROLE_GROUP },
    { NEUI_W_SECTION,    NEUI_A11Y_ROLE_GROUP },
  };
  for (const auto& c : cases) {
    A11yInput r = widget_row(1, 0, c.type);
    CHECK_EQ(a11y_derive_role(r), c.role);
  }
}

TEST_CASE("a11y: KNOB and SLIDER both derive SLIDER")
{
  // No platform has a "knob" role, and a slider is the contract every AT knows
  // how to drive.
  CHECK_EQ(a11y_derive_role(widget_row(1, 0, NEUI_W_KNOB)),
           NEUI_A11Y_ROLE_SLIDER);
  CHECK_EQ(a11y_derive_role(widget_row(1, 0, NEUI_W_SLIDER)),
           NEUI_A11Y_ROLE_SLIDER);
}

TEST_CASE("a11y: CUSTOMDRAW derives GROUP and is never guessed at")
{
  // The framework must not infer a role from an attached behavior asset: a
  // confidently wrong role is worse for a screen-reader user than a generic one.
  A11yInput r = widget_row(1, 0, NEUI_W_CUSTOMDRAW);
  r.has_value = true;                 // even with a value present
  r.tab_stop  = true;
  CHECK_EQ(a11y_derive_role(r), NEUI_A11Y_ROLE_GROUP);
}

TEST_CASE("a11y: a scrolling SECTION is a scroll area, a plain one is a group")
{
  A11yInput plain = widget_row(1, 0, NEUI_W_SECTION);
  CHECK_EQ(a11y_derive_role(plain), NEUI_A11Y_ROLE_GROUP);
  A11yInput scrolling = widget_row(1, 0, NEUI_W_SECTION);
  scrolling.scrollable = true;
  CHECK_EQ(a11y_derive_role(scrolling), NEUI_A11Y_ROLE_SCROLL_AREA);
}

TEST_CASE("a11y: scroll-area-ness comes from the section itself, not from a clip")
{
  // Both directions of the bug the host adapter exposed. The model used to
  // derive SCROLL_AREA from has_clip, which is the clip an ANCESTOR imposes.
  //
  // Direction 1: a scrolling section's own row carries its ancestor's clip -
  // usually none - so it looked like a plain group and an AT would never know
  // the content scrolls.
  A11yInput scrolls_but_unclipped = widget_row(1, 0, NEUI_W_SECTION);
  scrolls_but_unclipped.scrollable = true;
  scrolls_but_unclipped.has_clip   = false;
  CHECK_EQ(a11y_derive_role(scrolls_but_unclipped), NEUI_A11Y_ROLE_SCROLL_AREA);

  // Direction 2: a PLAIN section nested inside a scrolling one inherits that
  // clip, so it used to be announced as a scroll area that cannot scroll.
  A11yInput clipped_but_static = widget_row(1, 0, NEUI_W_SECTION);
  clipped_but_static.scrollable = false;
  clipped_but_static.has_clip   = true;
  clipped_but_static.clip_x = 0; clipped_but_static.clip_y = 0;
  clipped_but_static.clip_w = 40; clipped_but_static.clip_h = 20;
  CHECK_EQ(a11y_derive_role(clipped_but_static), NEUI_A11Y_ROLE_GROUP);

  // The clip still does its own job on that row: OFFSCREEN is unaffected.
  A11yInput off = widget_row(1, 0, NEUI_W_SECTION);
  off.x = 100; off.y = 0; off.w = 10; off.h = 10;
  off.has_clip = true;
  off.clip_x = 0; off.clip_y = 0; off.clip_w = 40; off.clip_h = 20;
  CHECK((a11y_derive_state(off) & NEUI_A11Y_STATE_OFFSCREEN) != 0);
}

TEST_CASE("a11y: sub-element kinds derive their own roles")
{
  CHECK_EQ(a11y_derive_role(sub_row(1, A11ySubKind::list_row, 0, "a")),
           NEUI_A11Y_ROLE_LIST_ITEM);
  CHECK_EQ(a11y_derive_role(sub_row(1, A11ySubKind::tree_item, 0, "a")),
           NEUI_A11Y_ROLE_TREE_ITEM);
  CHECK_EQ(a11y_derive_role(sub_row(1, A11ySubKind::grid_header, 0, "a")),
           NEUI_A11Y_ROLE_COLUMN_HEADER);
  CHECK_EQ(a11y_derive_role(sub_row(1, A11ySubKind::grid_row, 0, "a")),
           NEUI_A11Y_ROLE_ROW);
  CHECK_EQ(a11y_derive_role(sub_row(1, A11ySubKind::grid_cell, 0, "a")),
           NEUI_A11Y_ROLE_CELL);
  CHECK_EQ(a11y_derive_role(sub_row(1, A11ySubKind::tab_chip, 0, "a")),
           NEUI_A11Y_ROLE_TAB);
  CHECK_EQ(a11y_derive_role(sub_row(1, A11ySubKind::menu_item, 0, "a")),
           NEUI_A11Y_ROLE_MENU_ITEM);
}

TEST_CASE("a11y: a declared role beats the type default, ROLE_DEFAULT does not")
{
  std::vector<A11yInput> in;
  A11yInput cd = widget_row(1, 0, NEUI_W_CUSTOMDRAW);
  cd.declared_role = NEUI_A11Y_ROLE_SLIDER;
  in.push_back(cd);
  A11yInput plain = widget_row(2, 0, NEUI_W_BUTTON);
  plain.declared_role = NEUI_A11Y_ROLE_DEFAULT;
  in.push_back(plain);
  auto nodes = build_a11y_tree(in);

  CHECK_EQ(find_widget(nodes, 1)->role, NEUI_A11Y_ROLE_SLIDER);
  CHECK_EQ(find_widget(nodes, 2)->role, NEUI_A11Y_ROLE_BUTTON);
}

// ---------------------------------------------------------------------------
// Names

TEST_CASE("a11y: name priority is declared, then labelled_by, then text")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));

  A11yInput declared = widget_row(2, 1, NEUI_W_BUTTON);
  declared.name = "Declared";
  declared.text = "Text";
  in.push_back(declared);

  A11yInput lbl = widget_row(3, 1, NEUI_W_LABEL);
  lbl.text = "Frequency";
  in.push_back(lbl);

  A11yInput field = widget_row(4, 1, NEUI_W_INPUTBOX);
  field.labelled_by = { 3, 1, 0, -1 };
  in.push_back(field);

  A11yInput texty = widget_row(5, 1, NEUI_W_BUTTON);
  texty.text = "OwnText";
  in.push_back(texty);

  auto nodes = build_a11y_tree(in);
  CHECK_EQ(find_widget(nodes, 2)->name, std::string("Declared"));
  CHECK_EQ(find_widget(nodes, 4)->name, std::string("Frequency"));
  CHECK_EQ(find_widget(nodes, 5)->name, std::string("OwnText"));
}

TEST_CASE("a11y: a consumed LABEL is dropped so its words are not read twice")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  A11yInput lbl = widget_row(2, 1, NEUI_W_LABEL);
  lbl.text = "Gain";
  in.push_back(lbl);
  A11yInput field = widget_row(3, 1, NEUI_W_INPUTBOX);
  field.labelled_by = { 2, 1, 0, -1 };
  in.push_back(field);

  auto nodes = build_a11y_tree(in);
  CHECK(find_widget(nodes, 2) == nullptr);          // label consumed
  CHECK_EQ(find_widget(nodes, 3)->name, std::string("Gain"));
  CHECK_EQ(count_children(nodes, 1), (size_t)1);    // and not left dangling
}

TEST_CASE("a11y: labelled_by pointing nowhere degrades to the widget's own text")
{
  std::vector<A11yInput> in;
  // A BUTTON, not an INPUTBOX: for a text field the text is the VALUE, not a
  // name - see the next case, which pins that exception.
  A11yInput btn = widget_row(1, 0, NEUI_W_BUTTON);
  btn.labelled_by = { 42, 1, 0, -1 };   // never emitted
  btn.text = "Fallback";
  in.push_back(btn);
  auto nodes = build_a11y_tree(in);
  CHECK_EQ(nodes[0].name, std::string("Fallback"));
}

TEST_CASE("a11y: a text field's text is its value, never its name")
{
  // The adapter puts an INPUTBOX / MULTILINE's contents in value_text (a text
  // field's text IS its value). If the name fallback also took it, an AT would
  // read the contents twice and call them a label - and a field that DOES have a
  // name would then have to choose between announcing the label and announcing
  // what the user typed. So the text-as-name fallback skips these two roles.
  std::vector<A11yInput> in;
  A11yInput field = widget_row(1, 0, NEUI_W_INPUTBOX);
  field.text       = "440";
  field.value_text = "440";
  in.push_back(field);
  A11yInput area = widget_row(2, 0, NEUI_W_MULTILINE);
  area.text       = "line one";
  area.value_text = "line one";
  in.push_back(area);

  auto nodes = build_a11y_tree(in);
  const A11yNode* f = find_widget(nodes, 1);
  const A11yNode* a = find_widget(nodes, 2);
  CHECK(f != nullptr);
  CHECK(a != nullptr);
  CHECK(f->name.empty());                                  // unnamed, not "440"
  CHECK_EQ(f->value_text, std::string("440"));
  CHECK(a->name.empty());
  CHECK_EQ(a->value_text, std::string("line one"));

  // A declared name still wins, and still does not become the value.
  in[0].name = "Frequency";
  auto named = build_a11y_tree(in);
  const A11yNode* nf = find_widget(named, 1);
  CHECK(nf != nullptr);
  CHECK_EQ(nf->name, std::string("Frequency"));
  CHECK_EQ(nf->value_text, std::string("440"));
}

TEST_CASE("a11y: a labelled_by cycle terminates and does not hang")
{
  std::vector<A11yInput> in;
  A11yInput a = widget_row(1, 0, NEUI_W_INPUTBOX);
  a.labelled_by = { 2, 1, 0, -1 };
  A11yInput b = widget_row(2, 0, NEUI_W_INPUTBOX);
  b.labelled_by = { 1, 1, 0, -1 };
  in.push_back(a);
  in.push_back(b);
  auto nodes = build_a11y_tree(in);
  CHECK_EQ(nodes.size(), (size_t)2);   // reached here == did not hang
}

// ---------------------------------------------------------------------------
// State

TEST_CASE("a11y: derived state covers the framework-tracked flags")
{
  A11yInput r = widget_row(1, 0, NEUI_W_INPUTBOX);
  r.enabled = false; r.focused = true; r.tab_stop = true;
  r.selected = true; r.readonly = true; r.password = true; r.multiline = true;
  const uint32_t s = a11y_derive_state(r);
  CHECK((s & NEUI_A11Y_STATE_DISABLED)  != 0);
  CHECK((s & NEUI_A11Y_STATE_FOCUSED)   != 0);
  CHECK((s & NEUI_A11Y_STATE_FOCUSABLE) != 0);
  CHECK((s & NEUI_A11Y_STATE_SELECTED)  != 0);
  CHECK((s & NEUI_A11Y_STATE_READONLY)  != 0);
  CHECK((s & NEUI_A11Y_STATE_PROTECTED) != 0);
  CHECK((s & NEUI_A11Y_STATE_MULTILINE) != 0);
}

TEST_CASE("a11y: tri-state indeterminate is MIXED, never CHECKED")
{
  // Reporting a "maybe" as a definite "on" is a wrong answer, not a rounding.
  A11yInput on = widget_row(1, 0, NEUI_W_CHECKBOX);
  on.check_state = NEUI_CHECK_CHECKED;
  CHECK((a11y_derive_state(on) & NEUI_A11Y_STATE_CHECKED) != 0);
  CHECK((a11y_derive_state(on) & NEUI_A11Y_STATE_MIXED) == 0);

  A11yInput mixed = widget_row(2, 0, NEUI_W_CHECKBOX3);
  mixed.check_state = NEUI_CHECK_INDETERMINATE;
  CHECK((a11y_derive_state(mixed) & NEUI_A11Y_STATE_MIXED) != 0);
  CHECK((a11y_derive_state(mixed) & NEUI_A11Y_STATE_CHECKED) == 0);

  A11yInput off = widget_row(3, 0, NEUI_W_CHECKBOX);
  off.check_state = NEUI_CHECK_UNCHECKED;
  CHECK((a11y_derive_state(off) & NEUI_A11Y_STATE_CHECKED) == 0);
  CHECK((a11y_derive_state(off) & NEUI_A11Y_STATE_MIXED) == 0);

  A11yInput notcheckable = widget_row(4, 0, NEUI_W_BUTTON);   // check_state -1
  CHECK((a11y_derive_state(notcheckable) & NEUI_A11Y_STATE_CHECKED) == 0);
  CHECK((a11y_derive_state(notcheckable) & NEUI_A11Y_STATE_MIXED) == 0);
}

TEST_CASE("a11y: expandable reports EXPANDED or COLLAPSED, never both")
{
  A11yInput open = sub_row(1, A11ySubKind::tree_item, 0, "n");
  open.expandable = true; open.expanded = true;
  CHECK((a11y_derive_state(open) & NEUI_A11Y_STATE_EXPANDED) != 0);
  CHECK((a11y_derive_state(open) & NEUI_A11Y_STATE_COLLAPSED) == 0);

  A11yInput shut = sub_row(1, A11ySubKind::tree_item, 1, "n");
  shut.expandable = true; shut.expanded = false;
  CHECK((a11y_derive_state(shut) & NEUI_A11Y_STATE_COLLAPSED) != 0);
  CHECK((a11y_derive_state(shut) & NEUI_A11Y_STATE_EXPANDED) == 0);

  // A leaf claims neither - "collapsed" on something that cannot open is a lie.
  A11yInput leaf = sub_row(1, A11ySubKind::tree_item, 2, "n");
  const uint32_t s = a11y_derive_state(leaf);
  CHECK((s & (NEUI_A11Y_STATE_EXPANDED | NEUI_A11Y_STATE_COLLAPSED)) == 0);
}

TEST_CASE("a11y: a modal-blocked row reports DISABLED")
{
  A11yInput r = widget_row(1, 0, NEUI_W_BUTTON);
  r.enabled = true; r.modal_blocked = true;
  CHECK((a11y_derive_state(r) & NEUI_A11Y_STATE_DISABLED) != 0);
}

TEST_CASE("a11y: OFFSCREEN marks rows outside their clip, and only those")
{
  A11yInput inside = widget_row(1, 0, NEUI_W_BUTTON, 10, 10, 40, 20);
  inside.has_clip = true;
  inside.clip_x = 0; inside.clip_y = 0; inside.clip_w = 100; inside.clip_h = 100;
  CHECK((a11y_derive_state(inside) & NEUI_A11Y_STATE_OFFSCREEN) == 0);

  A11yInput below = widget_row(2, 0, NEUI_W_BUTTON, 10, 200, 40, 20);
  below.has_clip = true;
  below.clip_x = 0; below.clip_y = 0; below.clip_w = 100; below.clip_h = 100;
  CHECK((a11y_derive_state(below) & NEUI_A11Y_STATE_OFFSCREEN) != 0);

  A11yInput above = widget_row(3, 0, NEUI_W_BUTTON, 10, -40, 40, 20);
  above.has_clip = true;
  above.clip_x = 0; above.clip_y = 0; above.clip_w = 100; above.clip_h = 100;
  CHECK((a11y_derive_state(above) & NEUI_A11Y_STATE_OFFSCREEN) != 0);

  // Partly visible is NOT offscreen - it is reachable and pointable.
  A11yInput partial = widget_row(4, 0, NEUI_W_BUTTON, 10, 90, 40, 20);
  partial.has_clip = true;
  partial.clip_x = 0; partial.clip_y = 0; partial.clip_w = 100; partial.clip_h = 100;
  CHECK((a11y_derive_state(partial) & NEUI_A11Y_STATE_OFFSCREEN) == 0);
}

TEST_CASE("a11y: an offscreen row stays IN the tree rather than being pruned")
{
  // A focused control that scrolls out of view must remain reachable; deleting
  // it would make focus point at nothing.
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  A11yInput scrolled = widget_row(2, 1, NEUI_W_BUTTON, 10, 500, 40, 20);
  scrolled.has_clip = true;
  scrolled.clip_x = 0; scrolled.clip_y = 0; scrolled.clip_w = 100; scrolled.clip_h = 100;
  scrolled.focused = true; scrolled.tab_stop = true;
  in.push_back(scrolled);
  auto nodes = build_a11y_tree(in);

  const A11yNode* nd = find_widget(nodes, 2);
  CHECK(nd != nullptr);
  CHECK((nd->state & NEUI_A11Y_STATE_OFFSCREEN) != 0);
  CHECK((nd->state & NEUI_A11Y_STATE_FOCUSED) != 0);
}

TEST_CASE("a11y: a state override honours the mask precisely")
{
  std::vector<A11yInput> in;
  A11yInput r = widget_row(1, 0, NEUI_W_CUSTOMDRAW);
  r.enabled = false;                 // derives DISABLED
  r.tab_stop = true;                 // derives FOCUSABLE
  // Take over CHECKED only. DISABLED and FOCUSABLE must stay derived.
  r.state_mask   = NEUI_A11Y_STATE_CHECKED;
  r.state_values = NEUI_A11Y_STATE_CHECKED;
  in.push_back(r);
  auto nodes = build_a11y_tree(in);

  const uint32_t s = nodes[0].state;
  CHECK((s & NEUI_A11Y_STATE_CHECKED)   != 0);   // overridden on
  CHECK((s & NEUI_A11Y_STATE_DISABLED)  != 0);   // still derived
  CHECK((s & NEUI_A11Y_STATE_FOCUSABLE) != 0);   // still derived
}

TEST_CASE("a11y: an override can also CLEAR a derived bit")
{
  std::vector<A11yInput> in;
  A11yInput r = widget_row(1, 0, NEUI_W_CUSTOMDRAW);
  r.enabled = false;                                  // derives DISABLED
  r.state_mask   = NEUI_A11Y_STATE_DISABLED;          // take it over...
  r.state_values = 0;                                 // ...and say "not disabled"
  in.push_back(r);
  auto nodes = build_a11y_tree(in);
  CHECK((nodes[0].state & NEUI_A11Y_STATE_DISABLED) == 0);
}

// ---------------------------------------------------------------------------
// Value formatting

TEST_CASE("a11y: no value means NO value string")
{
  // A BUTTON that reports "0 %" would have an AT announcing a value for
  // something that has none.
  A11yInput r = widget_row(1, 0, NEUI_W_BUTTON);
  CHECK_EQ(a11y_format_value(r), std::string());
}

TEST_CASE("a11y: an explicit value text always wins")
{
  A11yInput r = widget_row(1, 0, NEUI_W_KNOB);
  r.has_value = true; r.value = 0.42f;
  r.has_range = true; r.vmin = -60.0f; r.vmax = 6.0f;
  r.value_text = "-6.0 dB";
  CHECK_EQ(a11y_format_value(r), std::string("-6.0 dB"));
}

TEST_CASE("a11y: a declared range maps the normalized value into real units")
{
  A11yInput r = widget_row(1, 0, NEUI_W_KNOB);
  r.has_value = true; r.value = 0.5f;
  r.has_range = true; r.vmin = -60.0f; r.vmax = 0.0f;
  CHECK_EQ(a11y_format_value(r), std::string("-30"));

  r.value = 0.0f;
  CHECK_EQ(a11y_format_value(r), std::string("-60"));
  r.value = 1.0f;
  CHECK_EQ(a11y_format_value(r), std::string("0"));
}

TEST_CASE("a11y: without a range the value is a percentage, not a raw fraction")
{
  A11yInput r = widget_row(1, 0, NEUI_W_KNOB);
  r.has_value = true; r.value = 0.42f;
  CHECK_EQ(a11y_format_value(r), std::string("42 %"));
}

TEST_CASE("a11y: value formatting survives NaN, out-of-range and degenerate input")
{
  A11yInput r = widget_row(1, 0, NEUI_W_KNOB);
  r.has_value = true;

  r.value = 2.0f;
  CHECK_EQ(a11y_format_value(r), std::string("100 %"));   // clamped
  r.value = -1.0f;
  CHECK_EQ(a11y_format_value(r), std::string("0 %"));     // clamped
  r.value = std::nanf("");
  CHECK_EQ(a11y_format_value(r), std::string("0 %"));     // NaN -> 0

  // min == max: no span to map onto. Report the bound rather than dividing.
  r.value = 0.5f; r.has_range = true; r.vmin = 5.0f; r.vmax = 5.0f;
  CHECK_EQ(a11y_format_value(r), std::string("5"));

  // Inverted range is a caller error; the announced value must still sit
  // between the two bounds the client named.
  r.vmin = 10.0f; r.vmax = 0.0f; r.value = 0.25f;
  CHECK_EQ(a11y_format_value(r), std::string("2.5"));
}

TEST_CASE("a11y: value strings drop trailing zeros")
{
  A11yInput r = widget_row(1, 0, NEUI_W_KNOB);
  r.has_value = true; r.has_range = true;
  r.vmin = 0.0f; r.vmax = 10.0f;
  r.value = 0.6f;
  CHECK_EQ(a11y_format_value(r), std::string("6"));      // not "6.00"
  r.value = 0.625f;
  CHECK_EQ(a11y_format_value(r), std::string("6.25"));
}

// ---------------------------------------------------------------------------
// Sub-elements and virtualization

TEST_CASE("a11y: list rows become ordered LIST_ITEM children")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  in.push_back(widget_row(2, 1, NEUI_W_LISTBOX, 0, 0, 120, 60));
  in.push_back(sub_row(2, A11ySubKind::list_row, 0, "alpha", 0,  0, 120, 20));
  in.push_back(sub_row(2, A11ySubKind::list_row, 1, "beta",  0, 20, 120, 20));
  in.push_back(sub_row(2, A11ySubKind::list_row, 2, "gamma", 0, 40, 120, 20));
  auto nodes = build_a11y_tree(in);

  const A11yNode* list = find_widget(nodes, 2);
  CHECK(list != nullptr);
  CHECK_EQ(list->role, NEUI_A11Y_ROLE_LIST);
  REQUIRE_EQ(list->children.size(), (size_t)3);
  // Order must be row order, not discovery order.
  CHECK_EQ(list->children[0].sub_index, 0);
  CHECK_EQ(list->children[1].sub_index, 1);
  CHECK_EQ(list->children[2].sub_index, 2);
  CHECK_EQ(a11y_find(nodes, list->children[1])->name, std::string("beta"));
}

TEST_CASE("a11y: an empty container yields the container and no children")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_LISTBOX, 0, 0, 120, 60));
  auto nodes = build_a11y_tree(in);
  REQUIRE_EQ(nodes.size(), (size_t)1);
  CHECK_EQ(nodes[0].children.size(), (size_t)0);
}

TEST_CASE("a11y: a GRID yields headers, rows and cells with correct parentage")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 400, 200));
  in.push_back(widget_row(2, 1, NEUI_W_GRID, 0, 0, 300, 120));
  in.push_back(sub_row(2, A11ySubKind::grid_header, 0, "Name", 0, 0, 150, 20));
  in.push_back(sub_row(2, A11ySubKind::grid_header, 1, "Size", 150, 0, 150, 20));

  // Two rows, each with two cells parented to their row.
  for (int row = 0; row < 2; ++row) {
    A11yInput r = sub_row(2, A11ySubKind::grid_row, row, nullptr,
                          0, 20 + row * 20, 300, 20);
    in.push_back(r);
    for (int col = 0; col < 2; ++col) {
      A11yInput c = sub_row(2, A11ySubKind::grid_cell, row * 2 + col,
                            col ? "12 KB" : "file",
                            col * 150, 20 + row * 20, 150, 20);
      c.parent = { 2, 1, static_cast<int32_t>(A11ySubKind::grid_row), row };
      in.push_back(c);
    }
  }
  auto nodes = build_a11y_tree(in);

  const A11yNode* grid = find_widget(nodes, 2);
  CHECK(grid != nullptr);
  CHECK_EQ(grid->role, NEUI_A11Y_ROLE_TABLE);
  // 2 headers + 2 rows directly under the table; cells hang off the rows.
  CHECK_EQ(grid->children.size(), (size_t)4);

  A11yNodeId row0{ 2, 1, static_cast<int32_t>(A11ySubKind::grid_row), 0 };
  const A11yNode* r0 = a11y_find(nodes, row0);
  CHECK(r0 != nullptr);
  CHECK_EQ(r0->role, NEUI_A11Y_ROLE_ROW);
  REQUIRE_EQ(r0->children.size(), (size_t)2);
  CHECK_EQ(a11y_find(nodes, r0->children[0])->role, NEUI_A11Y_ROLE_CELL);
  CHECK_EQ(a11y_find(nodes, r0->children[0])->name, std::string("file"));
}

TEST_CASE("a11y: a virtualized container keeps its true totals")
{
  // Only a window of rows is emitted, but the container must still advertise the
  // real set size - otherwise an AT tells the user "3 items" for a 10000-row grid.
  std::vector<A11yInput> in;
  A11yInput grid = widget_row(1, 0, NEUI_W_GRID, 0, 0, 300, 60);
  grid.total_child_count = 10000;
  grid.first_child_index = 5000;
  in.push_back(grid);
  in.push_back(sub_row(1, A11ySubKind::grid_row, 5000, nullptr, 0,  0, 300, 20));
  in.push_back(sub_row(1, A11ySubKind::grid_row, 5001, nullptr, 0, 20, 300, 20));
  auto nodes = build_a11y_tree(in);

  const A11yNode* nd = find_widget(nodes, 1);
  CHECK_EQ(nd->total_child_count, 10000);
  CHECK_EQ(nd->first_child_index, 5000);
  CHECK_EQ(nd->children.size(), (size_t)2);   // only the window is materialized
}

// ---------------------------------------------------------------------------
// Tab order

TEST_CASE("a11y: tab_index follows input order over the focusable rows only")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  A11yInput b1 = widget_row(2, 1, NEUI_W_BUTTON); b1.tab_stop = true;
  A11yInput lbl = widget_row(3, 1, NEUI_W_LABEL); lbl.text = "x";  // not a stop
  A11yInput b2 = widget_row(4, 1, NEUI_W_BUTTON); b2.tab_stop = true;
  in.push_back(b1); in.push_back(lbl); in.push_back(b2);
  auto nodes = build_a11y_tree(in);

  CHECK_EQ(find_widget(nodes, 2)->tab_index, 0);
  CHECK_EQ(find_widget(nodes, 3)->tab_index, -1);
  CHECK_EQ(find_widget(nodes, 4)->tab_index, 1);
  CHECK_EQ(find_widget(nodes, 1)->tab_index, -1);   // the frame is not a stop
}

TEST_CASE("a11y: a disabled or modal-blocked row is not a tab stop")
{
  std::vector<A11yInput> in;
  A11yInput off = widget_row(1, 0, NEUI_W_BUTTON);
  off.tab_stop = true; off.enabled = false;
  A11yInput blocked = widget_row(2, 0, NEUI_W_BUTTON);
  blocked.tab_stop = true; blocked.modal_blocked = true;
  in.push_back(off); in.push_back(blocked);
  auto nodes = build_a11y_tree(in);
  CHECK_EQ(find_widget(nodes, 1)->tab_index, -1);
  CHECK_EQ(find_widget(nodes, 2)->tab_index, -1);
}

// ---------------------------------------------------------------------------
// Lookup and hit-testing

TEST_CASE("a11y: a stale generation resolves to NOTHING, not to the slot's new owner")
{
  // The whole point of the generation field. Widget ids are session|slot with
  // slot reuse, so without this an AT holding a reference to a destroyed widget
  // would silently get the widget that later took the slot - answering with the
  // wrong role, name and bounds.
  std::vector<A11yInput> in;
  A11yInput reused = widget_row(5, 0, NEUI_W_BUTTON);
  reused.id.generation = 2;               // slot 5, second occupant
  in.push_back(reused);
  auto nodes = build_a11y_tree(in);

  CHECK(a11y_find(nodes, A11yNodeId{ 5, 2, 0, -1 }) != nullptr);   // current
  CHECK(a11y_find(nodes, A11yNodeId{ 5, 1, 0, -1 }) == nullptr);   // stale
}

TEST_CASE("a11y: hit-test returns the innermost containing node")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));
  in.push_back(widget_row(2, 1, NEUI_W_SECTION, 0, 0, 200, 100));
  in.push_back(widget_row(3, 2, NEUI_W_BUTTON, 10, 10, 40, 20));
  auto nodes = build_a11y_tree(in);

  const A11yNode* hit = a11y_hit_test(nodes, 20, 15);
  CHECK(hit != nullptr);
  CHECK_EQ(hit->id.widget_id, (uint32_t)3);          // the button, not the section

  // Inside the section but outside the button.
  hit = a11y_hit_test(nodes, 150, 80);
  CHECK_EQ(hit->id.widget_id, (uint32_t)2);

  // Outside everything.
  CHECK(a11y_hit_test(nodes, 5000, 5000) == nullptr);
}

TEST_CASE("a11y: hit-test skips pruned and offscreen nodes")
{
  std::vector<A11yInput> in;
  in.push_back(widget_row(1, 0, NEUI_W_APPWINDOW, 0, 0, 300, 200));

  // Decorative overlay covering the frame: pruned, so it must not swallow hits.
  A11yInput deco = widget_row(2, 1, NEUI_W_CUSTOMDRAW, 0, 0, 300, 200);
  deco.declared_role = NEUI_A11Y_ROLE_NONE;
  in.push_back(deco);

  // A scrolled-away control sitting where we click: it is in the tree but is not
  // actually at that position, so it must not be reported.
  A11yInput gone = widget_row(3, 1, NEUI_W_BUTTON, 10, 10, 40, 20);
  gone.has_clip = true;
  gone.clip_x = 0; gone.clip_y = 500; gone.clip_w = 100; gone.clip_h = 100;
  in.push_back(gone);

  const auto nodes = build_a11y_tree(in);
  const A11yNode* hit = a11y_hit_test(nodes, 20, 15);
  CHECK(hit != nullptr);
  CHECK_EQ(hit->id.widget_id, (uint32_t)1);   // falls through to the frame
}

TEST_CASE("a11y: id equality and the null-id predicate")
{
  A11yNodeId a{ 3, 1, 0, -1 };
  CHECK(a11y_id_equal(a, A11yNodeId{ 3, 1, 0, -1 }));
  CHECK(!a11y_id_equal(a, A11yNodeId{ 3, 2, 0, -1 }));   // generation matters
  CHECK(!a11y_id_equal(a, A11yNodeId{ 3, 1, 1, -1 }));   // kind matters
  CHECK(!a11y_id_equal(a, A11yNodeId{ 3, 1, 0,  0 }));   // index matters
  CHECK(a11y_id_null(A11yNodeId{}));
  CHECK(!a11y_id_null(a));
}

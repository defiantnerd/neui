// macOS NSAccessibility provider for the crossplatform host.
//
// WHAT THIS DOES. A neui frame is ONE NSView, so VoiceOver sees a window with a
// single opaque rectangle in it. This file publishes the synthetic node tree
// that a11y_adapter.cpp + hosts/shared/a11y_tree.h build (from the live widget
// tree) as real NSAccessibility elements, so every button, knob, list row and
// grid cell becomes something an AT can read, point at and operate.
//
// PULL, NOT PUSH. The tree is built on query behind Session::a11y_revision,
// never eagerly on change - see that accessor for why a paint is the
// invalidation signal. An AT traversal asks for children and then for each
// child's role / name / value / frame, which is many queries against one tree,
// so the cache is what makes this affordable.
//
// ELEMENT IDENTITY IS THE SUBTLE PART. VoiceOver holds element references
// across long spans and asks them questions much later, so the same node must
// come back as the SAME object across rebuilds - hence _elements, keyed by node
// id. And a node id carries a per-widget-instance generation (see
// Session::a11y_generation), so a reference to a destroyed widget answers
// "gone" rather than silently answering about whatever widget took its slot.
//
// THE COORDINATE FLIP LIVES IN ONE PLACE. Nodes are frame-local logical px,
// y-down. NSAccessibility wants screen points, y-up. -screenRectForNode: is the
// only conversion: multiply by the frame's zoom to get view points, then let
// AppKit do the flip (NEUIView is isFlipped, so -convertRect:toView:nil
// accounts for it). Nothing else in this file touches coordinates.

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cstring>
#include <string>
#include <vector>

#include "a11y_macos.h"
#include "a11y_adapter.h"
#include "host.h"
#include "platform.h"

using neui_detail::A11yNode;
using neui_detail::A11yNodeId;
using neui_detail::A11ySubKind;
using neui_detail::a11y_hit_test;
using neui_detail::a11y_id_equal;

// ---------------------------------------------------------------------------
// Role mapping

namespace {

// neui role -> NSAccessibility role. Deliberately conservative: every mapping
// is a role the platform already has a well-understood interaction contract
// for, because an AT drives what it recognises and inventing a role means
// VoiceOver falls back to "unknown".
NSString* ns_role_for(int role)
{
  switch (role) {
    case NEUI_A11Y_ROLE_WINDOW:        return NSAccessibilityWindowRole;
    case NEUI_A11Y_ROLE_STATIC_TEXT:   return NSAccessibilityStaticTextRole;
    case NEUI_A11Y_ROLE_BUTTON:        return NSAccessibilityButtonRole;
    // No toggle-button role on macOS; a button with the toggle SUBROLE is the
    // AppKit convention and keeps the pressed state meaningful (see below).
    case NEUI_A11Y_ROLE_TOGGLE_BUTTON: return NSAccessibilityButtonRole;
    case NEUI_A11Y_ROLE_CHECKBOX:      return NSAccessibilityCheckBoxRole;
    case NEUI_A11Y_ROLE_RADIO_BUTTON:  return NSAccessibilityRadioButtonRole;
    case NEUI_A11Y_ROLE_SLIDER:        return NSAccessibilitySliderRole;
    case NEUI_A11Y_ROLE_PROGRESS:      return NSAccessibilityProgressIndicatorRole;
    case NEUI_A11Y_ROLE_METER:         return NSAccessibilityLevelIndicatorRole;
    // A protected field is a text field with the SECURE SUBROLE (there is no
    // secure role) - see -accessibilitySubrole. That subrole is what tells
    // VoiceOver not to speak the contents; withholding the value ourselves is
    // the belt to its braces.
    case NEUI_A11Y_ROLE_TEXT_FIELD:    return NSAccessibilityTextFieldRole;
    case NEUI_A11Y_ROLE_TEXT_AREA:     return NSAccessibilityTextAreaRole;
    case NEUI_A11Y_ROLE_LIST:          return NSAccessibilityListRole;
    // Rows, not static text: a List / Table / Outline's children are rows on
    // this platform, and VoiceOver's list navigation keys off that.
    case NEUI_A11Y_ROLE_LIST_ITEM:     return NSAccessibilityRowRole;
    // A neui COMBOBOX is a closed bar plus a drop list with no text entry, so
    // it is a pop-up button rather than macOS's editable AXComboBox.
    case NEUI_A11Y_ROLE_COMBOBOX:      return NSAccessibilityPopUpButtonRole;
    case NEUI_A11Y_ROLE_TREE:          return NSAccessibilityOutlineRole;
    case NEUI_A11Y_ROLE_TREE_ITEM:     return NSAccessibilityRowRole;
    case NEUI_A11Y_ROLE_TABLE:         return NSAccessibilityTableRole;
    case NEUI_A11Y_ROLE_ROW:           return NSAccessibilityRowRole;
    case NEUI_A11Y_ROLE_CELL:          return NSAccessibilityCellRole;
    // A grid header IS a button - clicking it sorts - so Button is the honest
    // role; -accessibilityRoleDescription says "column header" so the user
    // hears what kind of button it is.
    case NEUI_A11Y_ROLE_COLUMN_HEADER: return NSAccessibilityButtonRole;
    case NEUI_A11Y_ROLE_TAB_LIST:      return NSAccessibilityTabGroupRole;
    // AppKit exposes NSTabView's tabs as radio buttons; matching that means
    // VoiceOver announces and drives them the way users already expect.
    case NEUI_A11Y_ROLE_TAB:           return NSAccessibilityRadioButtonRole;
    case NEUI_A11Y_ROLE_MENU_BAR:      return NSAccessibilityMenuBarRole;
    case NEUI_A11Y_ROLE_MENU:          return NSAccessibilityMenuRole;
    case NEUI_A11Y_ROLE_MENU_ITEM:     return NSAccessibilityMenuItemRole;
    case NEUI_A11Y_ROLE_IMAGE:         return NSAccessibilityImageRole;
    case NEUI_A11Y_ROLE_SCROLL_AREA:   return NSAccessibilityScrollAreaRole;
    case NEUI_A11Y_ROLE_GROUP:
    default:                           return NSAccessibilityGroupRole;
  }
}

// Roles whose accessibilityValue is a NUMBER on this platform (0 / 1, or 2 for
// a tri-state's mixed). VoiceOver reads a checkbox's state from the value, and
// handing it our formatted string instead would make it say nothing useful.
bool role_value_is_number(int role)
{
  return role == NEUI_A11Y_ROLE_CHECKBOX ||
         role == NEUI_A11Y_ROLE_TOGGLE_BUTTON ||
         role == NEUI_A11Y_ROLE_RADIO_BUTTON ||
         role == NEUI_A11Y_ROLE_TAB;
}

// Roles an AT can press. Sub-element rows are handled separately (they press by
// synthesised click, see -performPress:), so this is about widget rows.
bool role_is_pressable(int role)
{
  return role == NEUI_A11Y_ROLE_BUTTON || role == NEUI_A11Y_ROLE_CHECKBOX ||
         role == NEUI_A11Y_ROLE_TOGGLE_BUTTON ||
         role == NEUI_A11Y_ROLE_RADIO_BUTTON ||
         role == NEUI_A11Y_ROLE_COLUMN_HEADER;
}

bool role_is_stepper(int role)
{
  return role == NEUI_A11Y_ROLE_SLIDER;
}

NSString* key_for_id(const A11yNodeId& id)
{
  return [NSString stringWithFormat:@"%u.%u.%d.%d",
                   id.widget_id, id.generation, id.sub_kind, id.sub_index];
}

// Associated-object key for the per-view provider. The provider is retained by
// the view, so it dies with the view and nothing has to track view lifetime.
char kProviderKey;

} // namespace

// ---------------------------------------------------------------------------

@class NEUIA11yProvider;

// One accessibility element per node. Holds only the node id: everything else
// is answered from the provider's current tree, so an element that outlives its
// node degrades to "gone" instead of reporting stale facts.
@interface NEUIA11yElement : NSAccessibilityElement
{
@public
  A11yNodeId node_id;
}
@property (nonatomic, weak) NEUIA11yProvider* provider;
@end

@interface NEUIA11yProvider : NSObject
- (instancetype)initWithView:(NSView*)v
                     session:(xpl_host::Session*)s
                       frame:(uint32_t)frame_index;
- (void)refresh;
- (NSArray*)topLevelElements;
- (NSArray*)childrenOfNode:(const A11yNodeId&)nid;
// Points INTO the cached node vector, which -refresh replaces. Read what you
// need out of it before calling anything else on the provider.
- (const A11yNode*)nodeFor:(const A11yNodeId&)nid;
// The view, as the parent for a top-level element.
- (id)topLevelParent;
- (NEUIA11yElement*)elementFor:(const A11yNodeId&)nid;
- (NSRect)screenRectForNode:(const A11yNode&)nd;
- (id)hitTestScreen:(NSPoint)p;
- (id)focusedElement;
- (NSString*)roleDescriptionForNode:(const A11yNodeId&)nid;
- (BOOL)canPress:(const A11yNodeId&)nid;
- (BOOL)performPress:(const A11yNodeId&)nid;
- (BOOL)performStep:(const A11yNodeId&)nid up:(BOOL)up;
- (BOOL)focusNode:(const A11yNodeId&)nid;
- (void)postForWidget:(uint32_t)widget_id change:(int)change;
@end

// ---------------------------------------------------------------------------

@implementation NEUIA11yElement

- (NEUIA11yProvider*)prov { return self.provider; }

- (NSString*)accessibilityRole
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  if (!nd) return NSAccessibilityUnknownRole;
  return ns_role_for(nd->role);
}

- (NSString*)accessibilitySubrole
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  if (!nd) return nil;
  if (nd->role == NEUI_A11Y_ROLE_TOGGLE_BUTTON) return NSAccessibilityToggleSubrole;
  if (nd->role == NEUI_A11Y_ROLE_TREE_ITEM)     return NSAccessibilityOutlineRowSubrole;
  if (nd->role == NEUI_A11Y_ROLE_TEXT_FIELD &&
      (nd->state & NEUI_A11Y_STATE_PROTECTED))  return NSAccessibilitySecureTextFieldSubrole;
  return nil;
}

- (NSString*)accessibilityRoleDescription
{
  NSString* custom = [[self prov] roleDescriptionForNode:node_id];
  if (custom) return custom;
  return NSAccessibilityRoleDescription([self accessibilityRole],
                                        [self accessibilitySubrole]);
}

- (NSString*)accessibilityLabel
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  if (!nd || nd->name.empty()) return nil;
  return [NSString stringWithUTF8String:nd->name.c_str()];
}

- (NSString*)accessibilityHelp
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  if (!nd || nd->description.empty()) return nil;
  return [NSString stringWithUTF8String:nd->description.c_str()];
}

- (id)accessibilityValue
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  if (!nd) return nil;
  if (role_value_is_number(nd->role)) {
    if (nd->state & NEUI_A11Y_STATE_MIXED)   return @2;
    if (nd->state & NEUI_A11Y_STATE_CHECKED) return @1;
    // A tab / radio reports its selection as its value; a checkbox that is
    // neither checked nor mixed is simply off.
    if (nd->state & NEUI_A11Y_STATE_SELECTED) return @1;
    return @0;
  }
  // Never speak a protected field's contents, whatever the model resolved as
  // its value text.
  if (nd->state & NEUI_A11Y_STATE_PROTECTED) return nil;
  if (nd->value_text.empty()) return nil;
  return [NSString stringWithUTF8String:nd->value_text.c_str()];
}

- (NSRect)accessibilityFrame
{
  NEUIA11yProvider* p = [self prov];
  const A11yNode* nd = [p nodeFor:node_id];
  if (!nd) return NSZeroRect;
  return [p screenRectForNode:*nd];
}

- (BOOL)isAccessibilityElement { return YES; }

- (BOOL)isAccessibilityEnabled
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  if (!nd) return NO;
  return (nd->state & NEUI_A11Y_STATE_DISABLED) == 0;
}

- (BOOL)isAccessibilityFocused
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  return nd && (nd->state & NEUI_A11Y_STATE_FOCUSED) != 0;
}

- (void)setAccessibilityFocused:(BOOL)focused
{
  if (!focused) return;    // "unfocus this" has no meaning in a single-focus model
  [[self prov] focusNode:node_id];
}

- (BOOL)isAccessibilitySelected
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  return nd && (nd->state & NEUI_A11Y_STATE_SELECTED) != 0;
}

- (BOOL)isAccessibilityExpanded
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  return nd && (nd->state & NEUI_A11Y_STATE_EXPANDED) != 0;
}

- (id)accessibilityParent
{
  NEUIA11yProvider* p = [self prov];
  const A11yNode* nd = [p nodeFor:node_id];
  if (!nd) return nil;
  if (!neui_detail::a11y_id_null(nd->parent)) {
    NEUIA11yElement* pe = [p elementFor:nd->parent];
    if (pe) return NSAccessibilityUnignoredAncestor(pe);
  }
  // A top-level node's parent is the view: the view stands for the frame, so
  // the frame node itself is never published as an element of its own.
  return [p topLevelParent];
}

- (NSArray*)accessibilityChildren
{
  return [[self prov] childrenOfNode:node_id];
}

- (id)accessibilityHitTest:(NSPoint)point
{
  id hit = [[self prov] hitTestScreen:point];
  return hit ? hit : self;
}

// Actions. isAccessibilitySelectorAllowed: is what makes VoiceOver OFFER the
// action ("press to activate"), so it has to agree with what perform actually
// does - claiming an action that does nothing is worse than claiming none.
- (BOOL)isAccessibilitySelectorAllowed:(SEL)selector
{
  const A11yNode* nd = [[self prov] nodeFor:node_id];
  if (!nd) return NO;
  if (nd->state & NEUI_A11Y_STATE_DISABLED) return NO;
  if (selector == @selector(accessibilityPerformPress))
    return [[self prov] canPress:node_id];
  if (selector == @selector(accessibilityPerformIncrement) ||
      selector == @selector(accessibilityPerformDecrement))
    return role_is_stepper(nd->role) &&
           node_id.sub_index < 0 &&
           node_id.sub_kind == static_cast<int32_t>(A11ySubKind::widget);
  if (selector == @selector(setAccessibilityFocused:))
    return nd->tab_index >= 0;
  return [super isAccessibilitySelectorAllowed:selector];
}

- (BOOL)accessibilityPerformPress     { return [[self prov] performPress:node_id]; }
- (BOOL)accessibilityPerformIncrement { return [[self prov] performStep:node_id up:YES]; }
- (BOOL)accessibilityPerformDecrement { return [[self prov] performStep:node_id up:NO]; }

@end

// ---------------------------------------------------------------------------

@implementation NEUIA11yProvider
{
  __weak NSView*        _view;
  // Re-validated on every refresh, never trusted across one. A provider can
  // outlive both the session and the frame it describes: the AX runtime keeps
  // vended elements alive while a remote client holds tokens, and a plugin
  // editor is routinely destroyed while the DAW's window - which IS our native
  // handle when embedded - is still up. So the pointer is only as good as the
  // (session id, frame instance id) pair it was resolved from.
  xpl_host::Session*    _session;
  uint32_t              _session_id;
  uint32_t              _frame_index;
  uint32_t              _frame_generation;
  BOOL                  _dead;
  std::vector<A11yNode> _nodes;
  // node key -> NEUIA11yElement. Persistent across rebuilds so an AT-held
  // reference keeps working; entries for nodes that disappear are dropped.
  NSMutableDictionary<NSString*, NEUIA11yElement*>* _elements;
  uint32_t              _built_revision;
  BOOL                  _built;
}

- (instancetype)initWithView:(NSView*)v
                     session:(xpl_host::Session*)s
                       frame:(uint32_t)frame_index
{
  self = [super init];
  if (!self) return nil;
  _view             = v;
  _session          = s;
  _session_id       = s ? (s->get_session_id() & 0xffff) : 0;
  _frame_index      = frame_index;
  _frame_generation = s ? s->a11y_generation(frame_index) : 0;
  _dead             = NO;
  _elements         = [NSMutableDictionary dictionary];
  _built_revision   = 0;
  _built            = NO;
  return self;
}

// Everything the provider answers goes through -refresh first, so this is the
// single point where the session pointer is re-established. Once dead, dead:
// there is no path back, and every element degrades to "gone" rather than
// answering about a window that no longer exists.
- (BOOL)revalidate
{
  if (_dead) return NO;
  xpl_host::Session* s = xpl_host::a11y_live_session(_session_id);
  // s != _session catches a session slot recycled under the same id; the
  // generation test below catches it again if the allocator happened to hand
  // out the same address.
  if (!s || s != _session ||
      !xpl_host::a11y_frame_is_live(*s, _frame_index, _frame_generation)) {
    _dead    = YES;
    _session = nullptr;
    _nodes.clear();
    [_elements removeAllObjects];
    _built   = NO;
    return NO;
  }
  return YES;
}

- (id)topLevelParent
{
  NSView* v = _view;
  return v ? NSAccessibilityUnignoredAncestor(v) : nil;
}

- (float)frameZoom
{
  if (!_session) return 1.0f;
  xpl_host::WidgetData* wd = _session->get_widget(_frame_index);
  const float z = wd ? wd->ui_scale() : 1.0f;
  return (z > 0.0f) ? z : 1.0f;
}

- (void)refresh
{
  // The frame or its whole session may be gone since the last query - see
  // -revalidate. This must come before any other use of _session.
  if (![self revalidate]) return;
  // First query of any kind is what backs NEUI_API_A11Y::is_active.
  _session->mark_a11y_queried();

  const uint32_t rev = _session->a11y_revision();
  if (_built && rev == _built_revision) return;

  std::vector<A11yNode> fresh =
    xpl_host::a11y_build_frame_tree(*_session, _frame_index);

  // An empty result means the frame could not be DESCRIBED right now (never
  // painted, or the query arrived mid-paint and the in-paint guard refused),
  // not that the window emptied. Keep whatever we last had and do NOT record
  // the revision, so the next query retries: reporting "no children" would make
  // VoiceOver announce an empty window and stop. "The frame is gone" is a
  // different thing entirely and -revalidate has already handled it above.
  //
  // Not gated on _built: a FIRST query that is refused must also retry, or the
  // empty tree gets cached against a revision that paint_frame already bumped
  // at its start - so nothing would bump again when the paint ends and the
  // window would stay empty to the AT indefinitely.
  if (fresh.empty()) return;

  _nodes          = std::move(fresh);
  _built_revision = rev;
  _built          = YES;

  // Rebuild the element table, REUSING objects by node id. Recreating them
  // would invalidate every reference VoiceOver is holding.
  NSMutableDictionary<NSString*, NEUIA11yElement*>* next =
    [NSMutableDictionary dictionaryWithCapacity:_nodes.size()];
  const A11yNodeId frame_id =
    xpl_host::a11y_widget_node_id(*_session, _frame_index);
  for (const A11yNode& nd : _nodes) {
    // NO ELEMENT FOR THE FRAME NODE. The VIEW stands for the frame, and the
    // NSWindow above it already carries the window role and title. Publishing
    // one would make -accessibilityParent hand an AT a hidden AXWindow that
    // appears in nobody's children and whose own children are a second copy of
    // the entire window - parent and children walks would disagree.
    if (a11y_id_equal(nd.id, frame_id)) continue;
    NSString* k = key_for_id(nd.id);
    NEUIA11yElement* el = _elements[k];
    if (!el) {
      el = [[NEUIA11yElement alloc] init];
      el->node_id = nd.id;
      el.provider = self;
    }
    next[k] = el;
  }
  _elements = next;
}

- (const A11yNode*)nodeFor:(const A11yNodeId&)nid
{
  [self refresh];
  for (const A11yNode& nd : _nodes)
    if (a11y_id_equal(nd.id, nid)) return &nd;
  return nullptr;
}

- (NEUIA11yElement*)elementFor:(const A11yNodeId&)nid
{
  [self refresh];
  return _elements[key_for_id(nid)];
}

- (NSArray*)topLevelElements
{
  [self refresh];
  if (!_session) return @[];
  const A11yNodeId frame_id =
    xpl_host::a11y_widget_node_id(*_session, _frame_index);
  // Children of the frame node. The frame itself has no element (see -refresh),
  // so a top-level widget's -accessibilityParent resolves to the view and the
  // two walks agree. Nodes with no resolvable parent are included too:
  // build_a11y_tree re-parents survivors onto their nearest surviving ancestor,
  // and anything it could not place is still real content that must not vanish.
  NSMutableArray* out = [NSMutableArray array];
  for (const A11yNode& nd : _nodes) {
    if (a11y_id_equal(nd.id, frame_id)) continue;
    const bool top = a11y_id_equal(nd.parent, frame_id) ||
                     neui_detail::a11y_id_null(nd.parent);
    if (!top) continue;
    NEUIA11yElement* el = _elements[key_for_id(nd.id)];
    if (el) [out addObject:el];
  }
  return out;
}

- (NSArray*)childrenOfNode:(const A11yNodeId&)nid
{
  [self refresh];
  NSMutableArray* out = [NSMutableArray array];
  for (const A11yNode& nd : _nodes) {
    if (!a11y_id_equal(nd.id, nid)) continue;
    for (const A11yNodeId& cid : nd.children) {
      NEUIA11yElement* el = _elements[key_for_id(cid)];
      if (el) [out addObject:el];
    }
    break;
  }
  return out;
}

// THE Y-FLIP. Frame-local logical px (y-down) -> view points (x the zoom) ->
// screen points (y-up). The flip itself is AppKit's: NEUIView is isFlipped, so
// -convertRect:toView:nil accounts for it.
- (NSRect)screenRectForNode:(const A11yNode&)nd
{
  NSView* v = _view;
  if (!v) return NSZeroRect;
  const CGFloat z = (CGFloat)[self frameZoom];
  NSRect local = NSMakeRect(nd.x * z, nd.y * z, nd.w * z, nd.h * z);
  NSRect in_window = [v convertRect:local toView:nil];
  NSWindow* w = v.window;
  // No window yet (an embedded view before the DAW inserts it): window-space is
  // the best answer available, and it is at least self-consistent.
  if (!w) return in_window;
  return [w convertRectToScreen:in_window];
}

- (id)hitTestScreen:(NSPoint)screen_point
{
  [self refresh];
  NSView* v = _view;
  NSWindow* w = v.window;
  if (!v || !w) return nil;
  NSRect sr = NSMakeRect(screen_point.x, screen_point.y, 1.0, 1.0);
  NSPoint in_window = [w convertRectFromScreen:sr].origin;
  NSPoint in_view   = [v convertPoint:in_window fromView:nil];
  const CGFloat z = (CGFloat)[self frameZoom];
  const int lx = (int)(in_view.x / z);
  const int ly = (int)(in_view.y / z);
  const A11yNode* hit = a11y_hit_test(_nodes, lx, ly);
  if (!hit) return nil;
  if (_session &&
      a11y_id_equal(hit->id, xpl_host::a11y_widget_node_id(*_session, _frame_index)))
    return nil;                       // the frame itself: let AppKit answer the view
  return _elements[key_for_id(hit->id)];
}

- (id)focusedElement
{
  [self refresh];
  if (!_session) return nil;
  const uint32_t focused = _session->_focused_widget;
  if (focused == 0) return nil;
  // Focus is ONE value per session, and this provider speaks for one frame, so
  // a focused widget in another frame is not ours to report. Note what actually
  // enforces that: _elements only ever holds THIS frame's nodes, so the lookup
  // below would miss anyway (verified by mutation - removing this line changes
  // no observable behaviour). It stays as an explicit early-out, so the
  // per-frame rule is visible here rather than being an emergent property of
  // the cache, and so a future shared element table cannot silently break it.
  if (_session->frame_of(focused) != _frame_index) return nil;
  if (focused == _frame_index) {
    NSView* v = _view;
    return v ? NSAccessibilityUnignoredAncestor(v) : nil;
  }
  const A11yNodeId nid = xpl_host::a11y_widget_node_id(*_session, focused);
  return _elements[key_for_id(nid)];
}

- (NSString*)roleDescriptionForNode:(const A11yNodeId&)nid
{
  if (!_session) return nil;
  if (nid.sub_kind == static_cast<int32_t>(A11ySubKind::grid_header))
    return @"column header";
  if (nid.sub_index >= 0) return nil;
  const uint32_t slot = xpl_host::a11y_slot_of_node_id(*_session, nid);
  if (slot == 0) return nil;
  xpl_host::WidgetData* wd = _session->get_widget(slot);
  if (!wd || !wd->type) return nil;
  // A KNOB is exposed as a slider - no platform has a knob role - but saying
  // "knob" keeps the announcement true to what the user is looking at.
  if (!strcmp(wd->type, NEUI_W_KNOB)) return @"knob";
  return nil;
}

// ---- Actions -------------------------------------------------------------
//
// Every action routes through a path the USER already has, and routes through
// ALL of it: a widget row presses via the same two-stage key dispatch the space
// bar takes (client's KEYDOWN handler first, then the widget's own on_keydown),
// a stepper via the same arrow-key dispatch (so an AT increment fires
// GESTURE_BEGIN / VALUE_CHANGED / GESTURE_END exactly like a keypress, which is
// what a DAW needs to record one automation edit), and a sub-element via a
// synthesised click at its own reported centre. Nothing here reimplements widget
// behaviour.
//
// The client-first half matters most for the case <neui/d/a11y.h> calls the
// highest-value declaration a client can make: a CUSTOMDRAW with a declared
// role. Its activation lives in the CLIENT's handlers, and CustomDrawWidget's
// own on_keydown only forwards to a behavior asset - so dispatching straight to
// the widget would have made VoiceOver OFFER press / increment on every declared
// button and slider and then do nothing, which this file's own rule calls worse
// than offering nothing.

// The space bar / arrow key path, verbatim: the client gets first refusal via a
// KEYDOWN scoped to this widget, then the widget's virtual. Mirrors
// -[NEUIView keyDown:] (platform_macos.mm).
- (BOOL)sendKey:(uint32_t)keycode to:(xpl_host::WidgetData*)wd
{
  if (!wd || !_session) return NO;
  if (wd->emit_events) {
    neui_event_t ev{};
    ev.type     = NEUI_EVENT_KEYDOWN;
    ev.data.key = { { wd->widget_id }, keycode, 0 };
    if (_session->dispatch_event(&ev)) return YES;
  }
  return wd->on_keydown(keycode, 0) ? YES : NO;
}

- (BOOL)canPress:(const A11yNodeId&)nid
{
  const A11yNode* nd = [self nodeFor:nid];
  if (!nd || !_session) return NO;
  if (nd->state & (NEUI_A11Y_STATE_DISABLED | NEUI_A11Y_STATE_OFFSCREEN)) return NO;
  if (nid.sub_index < 0) return role_is_pressable(nd->role);

  const A11ySubKind kind = static_cast<A11ySubKind>(nid.sub_kind);
  // Menu items and an open COMBOBOX's drop rows are hit-tested at FRAME level
  // by the platform layer's own mouse handler, not by the owning widget, so a
  // click synthesised into the widget would land nowhere. Rather than claim an
  // action that does nothing, decline: both are still fully operable from the
  // keyboard (a focused combobox takes arrows + Return), which is also how a
  // VoiceOver user drives a pop-up button on this platform anyway.
  if (kind == A11ySubKind::menu_item) return NO;
  const uint32_t owner = xpl_host::a11y_slot_of_node_id(*_session, nid);
  if (owner == 0) return NO;
  if (_session->_open_combo == owner) return NO;
  xpl_host::WidgetData* wd = _session->get_widget(owner);
  return wd && wd->enabled && wd->emit_events;
}

- (BOOL)performPress:(const A11yNodeId&)nid
{
  if (![self canPress:nid]) return NO;
  const A11yNode* nd = [self nodeFor:nid];
  if (!nd || !_session) return NO;
  const uint32_t slot = xpl_host::a11y_slot_of_node_id(*_session, nid);
  if (slot == 0) return NO;
  xpl_host::WidgetData* wd = _session->get_widget(slot);
  if (!wd) return NO;

  // Any action we perform changes state the CACHED tree does not know about,
  // and an AT reads the value straight back after pressing. Bumping here rather
  // than relying on the repaint the widget requests is what makes that read
  // correct: platform_invalidate only schedules a paint, so the follow-up query
  // can easily arrive first.
  _session->bump_a11y_revision();

  if (nid.sub_index < 0)
    return [self sendKey:NEUI_KEY_SPACE to:wd];

  // Sub-element: click its centre. The rect is the one this element just
  // reported to the AT, and dispatch_mouse_event takes frame-local px, so the
  // press lands exactly where the user would have clicked.
  const int cx = nd->x + nd->w / 2;
  const int cy = nd->y + nd->h / 2;
  neui_event_t ev{};
  ev.data.mouse.widget.id = wd->widget_id;
  ev.data.mouse.x         = cx;
  ev.data.mouse.y         = cy;
  ev.data.mouse.buttonmap = NEUI_MK_LBUTTON;
  ev.type = NEUI_EVENT_MOUSE_BUTTON_DOWN;
  _session->dispatch_mouse_event(slot, &ev);
  ev.data.mouse.x = cx;    // dispatch_mouse_event restores these, but be explicit
  ev.data.mouse.y = cy;
  ev.type = NEUI_EVENT_MOUSE_BUTTON_UP;
  _session->dispatch_mouse_event(slot, &ev);
  // ...and the CLICK a real release produces when down and up land on the same
  // widget (see -[NEUIView mouseUp:]). Without it a client keyed on CLICK over a
  // grid / tab / list sees nothing from an AT press, even though the widget's own
  // internal handler already acted on the DOWN.
  ev.data.mouse.x = cx;
  ev.data.mouse.y = cy;
  ev.data.mouse.buttonmap = 0;      // the button is no longer held
  ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
  _session->dispatch_mouse_event(slot, &ev);
  return YES;
}

- (BOOL)performStep:(const A11yNodeId&)nid up:(BOOL)up
{
  const A11yNode* nd = [self nodeFor:nid];
  if (!nd || !_session) return NO;
  if (nd->state & NEUI_A11Y_STATE_DISABLED) return NO;
  if (nid.sub_index >= 0 || !role_is_stepper(nd->role)) return NO;
  const uint32_t slot = xpl_host::a11y_slot_of_node_id(*_session, nid);
  if (slot == 0) return NO;
  xpl_host::WidgetData* wd = _session->get_widget(slot);
  if (!wd) return NO;
  _session->bump_a11y_revision();   // see -performPress:
  // A declared real-world step is a promise in <neui/d/a11y.h>: "the increment
  // an AT increment/decrement action should move by, in real-world units". The
  // arrow keys move 10 % of the range (or one NEUI_ATTR_STEPS detent), which is
  // not the same number, so honour the declaration when there is one - through a
  // host helper, so the gesture + VALUE_CHANGED events are identical either way.
  if (_session->a11y_step_value(slot, up ? true : false)) return YES;
  return [self sendKey:(up ? NEUI_KEY_RIGHT : NEUI_KEY_LEFT) to:wd];
}

- (BOOL)focusNode:(const A11yNodeId&)nid
{
  const A11yNode* nd = [self nodeFor:nid];
  if (!nd || !_session) return NO;
  if (nd->tab_index < 0) return NO;          // not focusable; refuse rather than lie
  if (nid.sub_index >= 0) return NO;         // sub-elements are not focus targets
  const uint32_t slot = xpl_host::a11y_slot_of_node_id(*_session, nid);
  if (slot == 0) return NO;
  _session->set_focus(slot);
  return YES;
}

- (void)postForWidget:(uint32_t)widget_id change:(int)change
{
  if (_dead || !_session) return;
  // Structure changes are about the container, not one element - and the tree
  // is rebuilt lazily anyway, so the view is the right thing to announce.
  NSView* v = _view;
  if (change == xpl_host::a11y_notify_structure) {
    if (v) NSAccessibilityPostNotification(v, NSAccessibilityLayoutChangedNotification);
    return;
  }

  // Deliberately does NOT rebuild. A notification only says "re-ask about this
  // element", so the element merely has to exist; rebuilding here would put a
  // full tree build (and possibly a forced paint, via ensure_abs_positions) on
  // every knob-drag event once an AT is attached, which is exactly the eager
  // work the pull model exists to avoid. An element nothing has published yet
  // has no AT waiting on it either.
  const uint32_t slot = widget_id & 0xffff;
  const A11yNodeId nid = xpl_host::a11y_widget_node_id(*_session, slot);
  NEUIA11yElement* el = _elements[key_for_id(nid)];
  if (!el) return;

  switch (change) {
    case xpl_host::a11y_notify_focus:
      NSAccessibilityPostNotification(
        el, NSAccessibilityFocusedUIElementChangedNotification);
      break;
    case xpl_host::a11y_notify_name:
      NSAccessibilityPostNotification(el, NSAccessibilityTitleChangedNotification);
      break;
    case xpl_host::a11y_notify_selection:
      NSAccessibilityPostNotification(
        el, NSAccessibilitySelectedChildrenChangedNotification);
      break;
    case xpl_host::a11y_notify_value:
    case xpl_host::a11y_notify_state:
    default:
      // State rides on the value notification: macOS has no separate
      // state-changed, and for the roles where state matters (checkbox, toggle,
      // tab) the state IS the accessibility value.
      NSAccessibilityPostNotification(el, NSAccessibilityValueChangedNotification);
      break;
  }
}

@end

// ---------------------------------------------------------------------------
// The seam the platform layer calls.

namespace xpl_host
{

namespace {

// The view's provider, creating it on first use. Retained by the view via an
// associated object, so its lifetime is the view's and nothing tracks it.
NEUIA11yProvider* provider_for(NSView* view, Session* s, uint32_t frame_index,
                               bool create)
{
  if (!view || !s) return nil;
  NEUIA11yProvider* p = objc_getAssociatedObject(view, &kProviderKey);
  if (p || !create) return p;
  p = [[NEUIA11yProvider alloc] initWithView:view session:s frame:frame_index];
  objc_setAssociatedObject(view, &kProviderKey, p, OBJC_ASSOCIATION_RETAIN);
  return p;
}

} // namespace

NSArray* mac_a11y_children(NSView* view, Session* s, uint32_t frame_index)
{
  NEUIA11yProvider* p = provider_for(view, s, frame_index, true);
  return p ? [p topLevelElements] : @[];
}

id mac_a11y_hit_test(NSView* view, Session* s, uint32_t frame_index,
                     NSPoint screen_point)
{
  NEUIA11yProvider* p = provider_for(view, s, frame_index, true);
  return p ? [p hitTestScreen:screen_point] : nil;
}

id mac_a11y_focused_element(NSView* view, Session* s, uint32_t frame_index)
{
  NEUIA11yProvider* p = provider_for(view, s, frame_index, true);
  return p ? [p focusedElement] : nil;
}

void mac_a11y_notify(NSView* view, uint32_t widget_id, int change)
{
  if (!view) return;
  // Deliberately does NOT create a provider: no provider means nothing has ever
  // queried this frame, so there is no AT to tell and no tree to describe.
  NEUIA11yProvider* p = objc_getAssociatedObject(view, &kProviderKey);
  [p postForWidget:widget_id change:change];
}

void mac_a11y_announce(NSView* view, const char* utf8, bool assertive)
{
  if (!utf8 || !*utf8) return;
  NSString* msg = [NSString stringWithUTF8String:utf8];
  if (!msg) return;
  // The announcement is posted on the WINDOW (Apple's documented requirement
  // for NSAccessibilityAnnouncementRequested), falling back to the app when
  // there is no window yet.
  id target = (view && view.window) ? (id)view.window : (id)NSApp;
  NSAccessibilityPostNotificationWithUserInfo(
    target, NSAccessibilityAnnouncementRequestedNotification,
    @{ NSAccessibilityAnnouncementKey : msg,
       NSAccessibilityPriorityKey :
         @(assertive ? NSAccessibilityPriorityHigh : NSAccessibilityPriorityMedium) });
}

} // namespace xpl_host

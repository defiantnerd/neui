#pragma once

// macOS accessibility provider - the NSAccessibility half of NEUI_API_A11Y.
//
// Split out of platform_macos.mm on purpose. The provider needs none of that
// file's window / input / drag machinery: it takes the frame's content NSView
// and the Session, and everything else it needs comes from the portable node
// tree (a11y_adapter.h -> hosts/shared/a11y_tree.h). Keeping it separate also
// keeps the NEUIView overrides down to one line each, so the accessibility
// surface is readable in one file rather than scattered through 2700 lines.
//
// The view argument is always the frame's CONTENT view (frame_content_view() on
// the native handle), which is the element the OS asks about a neui window's
// contents - one native surface per frame, so one provider per frame.

#ifdef __OBJC__

#import <AppKit/AppKit.h>

#include <cstdint>

namespace xpl_host
{
  class Session;

  // The frame's top-level accessibility children: elements for the children of
  // the frame node, since the VIEW itself stands for the frame. Builds or
  // refreshes the cached node tree as needed (pull, not push - see
  // Session::a11y_revision), and empty when the frame cannot be described yet.
  NSArray* mac_a11y_children(NSView* view, Session* s, uint32_t frame_index);

  // Deepest element at a SCREEN point, or nil to let AppKit answer with the
  // view itself. Offscreen (scrolled-away) nodes are skipped - they stay in the
  // tree so a focused control that scrolls out of view is still reachable, but
  // they are not the thing at that position.
  id mac_a11y_hit_test(NSView* view, Session* s, uint32_t frame_index,
                       NSPoint screen_point);

  // The element for this FRAME's focused widget, the view itself when focus is
  // on the frame, or nil when focus is elsewhere. Per-frame by design: focus is
  // session-global internally, and a provider that reported another window's
  // focused control would make VoiceOver follow the wrong window.
  id mac_a11y_focused_element(NSView* view, Session* s, uint32_t frame_index);

  // Post a change notification for `widget_id` (a PUBLIC widget id). Does
  // nothing when this view has no provider yet: no provider means nothing has
  // ever queried us, so there is no AT listening and no tree to talk about.
  void mac_a11y_notify(NSView* view, uint32_t widget_id, int change);

  // Speak `utf8` with no node behind it. Needs no provider and no tree.
  void mac_a11y_announce(NSView* view, const char* utf8, bool assertive);
}

#endif  // __OBJC__

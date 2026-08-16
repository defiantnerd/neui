#pragma once

// Declared keyboard navigation for a popup surface.
//
// A popup surface is normally painted by the client, so the host has no rows to
// walk: inside a NEUI_W_CUSTOMDRAW it cannot know what an "item" is. The client
// therefore DECLARES a list - a count, a current index, a page step, a wrap flag
// (NEUI_ATTR_NAV_* in <neui/d/attrs.h>) - and this header owns the arithmetic
// over that declaration while the client keeps painting from the index.
//
// Pure and header-only on purpose. The keys are the boring part; getting the
// EDGES right is not (wrap vs clamp, paging that never wraps, the "nothing
// selected yet" state a freshly opened menu is in, and a count that shrinks
// under a live filter while an index still points past its end). All of that is
// portable, so it lives here and is Tier-1 tested rather than being re-derived
// per platform - and the only alternative home would have been Session, which no
// test can construct without a window.

#include <neui/d/keys.h>

#include <stdint.h>

namespace neui_detail {

// The client's declaration, as read from the surface's attributes.
struct PopupNav
{
  int  count = 0;    // items the client is drawing; <= 0 disables navigation
  int  index = -1;   // current item, or -1 for "none selected yet"
  int  page  = 10;   // PageUp / PageDown step; <= 0 falls back to 10
  bool wrap  = true; // Up from the first -> the last, and back again
};

struct PopupNavResult
{
  int  index    = -1;     // the index after the key (== the one before, if inert)
  bool handled  = false;  // this key belongs to navigation; the host acted
  bool changed  = false;  // ...and the index actually moved
  bool activate = false;  // Enter on a real selection: commit it
};

// Clamp a declared index into the legal range for `count`, mapping anything
// out of range back to "none". Applied before every key, because count is live:
// a filtered list re-declares it per keystroke, and an index left pointing at
// row 40 of a list that is now 3 long must not survive into the arithmetic.
inline int popup_nav_clamp_index(int index, int count)
{
  if (count <= 0) return -1;
  if (index < 0 || index >= count) return -1;
  return index;
}

// Apply one key to the declared list. `handled == false` means the key is none
// of navigation's business - the caller decides what else to do with it (the
// popup gate swallows it either way, so it never reaches the owner).
inline PopupNavResult popup_nav_key(const PopupNav& nav, uint32_t keycode)
{
  PopupNavResult r;
  const int count = nav.count;
  r.index = popup_nav_clamp_index(nav.index, count);
  if (count <= 0) return r;             // nothing declared: not ours

  const int  last  = count - 1;
  const int  page  = (nav.page > 0) ? nav.page : 10;
  const int  cur   = r.index;
  const bool none  = (cur < 0);
  int next = cur;

  switch (keycode) {
  case NEUI_KEY_UP:
    // From "nothing selected" an Up enters the list at the BOTTOM and a Down at
    // the top, which is what every OS menu does when it is opened by keyboard.
    next = none ? last : (cur > 0 ? cur - 1 : (nav.wrap ? last : 0));
    break;
  case NEUI_KEY_DOWN:
    next = none ? 0 : (cur < last ? cur + 1 : (nav.wrap ? 0 : last));
    break;
  case NEUI_KEY_HOME:
    next = 0;
    break;
  case NEUI_KEY_END:
    next = last;
    break;
  case NEUI_KEY_PAGEUP: {
    // Paging CLAMPS even when wrapping is on: a page step that jumped from the
    // top to the bottom of the list would lose the user completely, and no
    // platform's list does it.
    const int base = none ? last : cur;
    next = (base - page > 0) ? base - page : 0;
    break;
  }
  case NEUI_KEY_PAGEDOWN: {
    const int base = none ? 0 : cur;
    next = (base + page < last) ? base + page : last;
    break;
  }
  case NEUI_KEY_RETURN:
    // Enter with nothing selected is deliberately NOT handled: there is nothing
    // to commit, and reporting it as handled would let a client's own
    // "Enter accepts the typed text" fall silently on the floor.
    if (none) return r;
    r.handled  = true;
    r.activate = true;
    return r;
  default:
    return r;                            // not a navigation key
  }

  r.handled = true;
  r.changed = (next != cur);
  r.index   = next;
  return r;
}

} // namespace neui_detail

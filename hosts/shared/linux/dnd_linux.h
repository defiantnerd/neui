#pragma once

#if defined(__linux__) || (defined(__unix__) && !defined(__APPLE__))

// XDND (X Drag-and-Drop) protocol helpers for the Linux platform layer.
//
// Pure, Session-agnostic pieces only: the interned XDND atom set and the
// atom<->MIME / atom<->action mappings. The actual drop-target state machine
// (which drives Session::dispatch_dnd_*) lives in platform_linux.cpp, because
// it is coupled to xpl_host::Session and the per-window LinuxWindow - matching
// the codebase's choice to keep DnD dispatch per-host rather than unified.
//
// XDND reference: https://www.freedesktop.org/wiki/Specifications/XDND/
// We speak version 5 as a drop target.

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <neui/neui.h>   // NEUI_DND_ACTION_*

#include <string>

namespace neui_detail
{
  struct XdndAtoms
  {
    Atom aware = None, enter = None, position = None, status = None;
    Atom leave = None, drop = None, finished = None;
    Atom selection = None, type_list = None;
    Atom action_copy = None, action_move = None, action_link = None;
    Atom action_ask = None, action_private = None;
    Atom utf8 = None, text = None, recv_prop = None, incr = None;

    void intern(Display* d)
    {
      aware          = XInternAtom(d, "XdndAware",          False);
      enter          = XInternAtom(d, "XdndEnter",          False);
      position       = XInternAtom(d, "XdndPosition",       False);
      status         = XInternAtom(d, "XdndStatus",         False);
      leave          = XInternAtom(d, "XdndLeave",          False);
      drop           = XInternAtom(d, "XdndDrop",           False);
      finished       = XInternAtom(d, "XdndFinished",       False);
      selection      = XInternAtom(d, "XdndSelection",      False);
      type_list      = XInternAtom(d, "XdndTypeList",       False);
      action_copy    = XInternAtom(d, "XdndActionCopy",     False);
      action_move    = XInternAtom(d, "XdndActionMove",     False);
      action_link    = XInternAtom(d, "XdndActionLink",     False);
      action_ask     = XInternAtom(d, "XdndActionAsk",      False);
      action_private = XInternAtom(d, "XdndActionPrivate",  False);
      utf8           = XInternAtom(d, "UTF8_STRING",        False);
      text           = XInternAtom(d, "TEXT",               False);
      recv_prop      = XInternAtom(d, "NEUI_XDND_RECV",     False);
      incr           = XInternAtom(d, "INCR",               False);
    }
  };

  // XDND version we advertise / speak.
  inline constexpr long kXdndVersion = 5;

  // XDND action atom -> neui_dnd_action_t. Ask/Private collapse to Copy
  // (we don't surface the action-negotiation popup). Unknown -> NONE.
  inline uint32_t xdnd_action_to_neui(const XdndAtoms& a, Atom action)
  {
    if (action == a.action_copy)    return NEUI_DND_ACTION_COPY;
    if (action == a.action_move)    return NEUI_DND_ACTION_MOVE;
    if (action == a.action_link)    return NEUI_DND_ACTION_LINK;
    if (action == a.action_ask)     return NEUI_DND_ACTION_COPY;
    if (action == a.action_private) return NEUI_DND_ACTION_COPY;
    return NEUI_DND_ACTION_NONE;
  }

  // neui_dnd_action_t -> the XDND action atom to report back in
  // XdndStatus / XdndFinished. NONE -> None.
  inline Atom xdnd_neui_to_action(const XdndAtoms& a, uint32_t act)
  {
    switch (act) {
      case NEUI_DND_ACTION_COPY: return a.action_copy;
      case NEUI_DND_ACTION_MOVE: return a.action_move;
      case NEUI_DND_ACTION_LINK: return a.action_link;
      default:                   return None;
    }
  }

  // X target atom -> neui MIME string. Text targets fold to the canonical
  // text/plain MIME; any atom whose name looks like a MIME ("x/y") passes
  // through verbatim (matches GTK/Qt: "text/uri-list", "text/html",
  // "image/png", arbitrary "x/y"). Returns "" for atoms to ignore
  // (TIMESTAMP, MULTIPLE, non-MIME named selections, ...).
  inline std::string xdnd_atom_to_mime(Display* d, Atom target, const XdndAtoms& a)
  {
    if (target == a.utf8 || target == XA_STRING || target == a.text)
      return "text/plain;charset=utf-8";
    char* name = XGetAtomName(d, target);
    if (!name) return std::string();
    std::string s = name;
    XFree(name);
    if (s == "text/plain") return "text/plain;charset=utf-8";
    if (s.find('/') != std::string::npos) return s;   // looks like a MIME
    return std::string();
  }

} // namespace neui_detail

#endif // linux

#pragma once

// win32 UI Automation provider - the Windows half of NEUI_API_A11Y.
//
// This file SHIPPED UNVERIFIED - written on a machine that could not compile it -
// and has since been compiled and RUN (2026-08-11). See plans/accessibility.md
// 6.4 and docs/accessibility.md.
//
// VERIFIED BY tests/a11y_provider_smoke_win32.cpp: 57 checks through the real UIA
// CLIENT stack (CUIAutomation, the same API Narrator uses), with the client on its
// own MTA thread while the UI thread pumps - so every call crosses an apartment
// boundary and ProviderOptions_UseComThreading is exercised the way an AT
// exercises it, rather than the one arrangement no AT produces. All five risk
// areas this file shipped with hold up (element lifetime across a frame destroy,
// Navigate vs FindAll, UseComThreading, the UiaRaiseNotificationEvent guard,
// nullptr from ElementProviderFromPoint for the frame). The ~40 static_asserts on
// UIA constants pass against the real SDK, which was the highest-value safeguard
// and had never been exercised before that run.
//
// The first run found ONE real defect, and it was in the hand-written glue rather
// than anywhere the shared model reaches - which is where this header predicted it
// would be: range_of() ignored NEUI_ATTR_A11Y_VALUE, so a CUSTOMDRAW holding its
// value in client state reported 0.0. See the comment at that fix.
//
// STILL NOT VERIFIED: whether the announcements are SENSIBLE. No screen reader and
// no Inspect.exe session has looked at a neui window. An automated client proves
// the tree, roles, names, geometry and actions are what the code intends; it
// cannot tell you Narrator reads a knob in a way a blind user would want. macOS
// had that pass (VoiceOver); Windows has not. Narrator on neui_a11y_example is the
// remaining task, and it is a judgement call rather than pass/fail.
//
// What stood in for execution while it was unverified - and still earns its keep
// for anyone editing this file from macOS or Linux:
//   * the node tree, the adapter and the cache-invalidation SCHEME are shared
//     with the macOS provider, which IS verified against VoiceOver by
//     tests/a11y_provider_smoke_macos.mm. Note carefully what that does and does
//     not carry over: the shared MODEL is verified, but the per-platform GLUE
//     re-implements the same decisions, and review found two of the nine macOS
//     defects re-broken here in exactly that glue (the notify path rebuilt the
//     whole tree per event; the root's child list omitted unparented nodes).
//     "Shares the design" is not "inherits the fixes" - and the one defect that
//     survived to the first run was in the glue as well;
//   * the role / pattern / state mapping tables live in
//     hosts/shared/a11y_uia_map.h and are Tier-1 tested on every platform;
//   * every UIA constant is static_asserted against the real SDK headers in the
//     .cpp, so a wrong number fails the BUILD instead of producing a subtly
//     wrong tree at runtime;
//   * a stub-header parse check under -Wall -Wextra confirms it parses and that
//     every symbol it references is one that was accounted for. It is a
//     typo-and-signature net only: the range_of defect was perfectly-formed C++.
//
// The provider is bound to the FRAME's HWND: one native window per frame, so one
// fragment root per frame, and per-frame focus (see G3 in the plan).

#ifdef _WIN32

#include <cstdint>

namespace xpl_host
{
  class Session;

  // WM_GETOBJECT. Returns true when it handled the message (and wrote
  // *out_lresult); false means "not a UIA request, fall through to DefWindowProc"
  // - which is also what an MSAA/IAccessible request gets, since there is no
  // MSAA bridge here.
  bool a11y_win32_handle_get_object(void* hwnd, Session* s, uint32_t frame_index,
                                    intptr_t wparam, intptr_t lparam,
                                    intptr_t* out_lresult);

  // Raise a UIA event / property change for `widget_id` (a PUBLIC widget id).
  // Does nothing when this HWND has no provider yet: no provider means nothing
  // has queried us, so there is no client listening and no tree to talk about.
  void a11y_win32_notify(void* hwnd, uint32_t widget_id, int change);

  // Speak `utf8` with no element behind it (UiaRaiseNotificationEvent).
  void a11y_win32_announce(void* hwnd, const char* utf8, bool assertive);

  // Drop the provider bound to this HWND. Called from WM_NCDESTROY: UIA may
  // still hold element references afterwards, and they must answer "gone"
  // rather than reach into a destroyed Session.
  void a11y_win32_window_destroyed(void* hwnd);

  // UiaClientsAreListening(), used only as the advisory answer to
  // NEUI_API_A11Y::is_active.
  bool a11y_win32_clients_listening();
}

#endif  // _WIN32

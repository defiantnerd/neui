// Regression test for the LP64 format-32 property read (clipboard_linux.h).
//
// Xlib widens format-32 properties to C `long` (8 bytes on 64-bit), so a
// TARGETS atom list must be copied at sizeof(long) per item, not 4. The old
// code copied nitems*4, silently dropping the second half of the list.
//
// This harness stands up a *foreign* CLIPBOARD owner (own X connection, raw
// Xlib) that advertises a TARGETS list with several MIME atoms and UTF8_STRING
// deliberately placed LAST (well past the midpoint). It then drives a
// neui_detail::ClipboardX11 reader on a separate connection and asserts:
//   - has_text() finds UTF8_STRING (only possible if the tail survived), and
//   - read_item() recovers every advertised MIME format.
// With the truncation bug both fail. Linux-only; needs a live X display.

#include "linux/clipboard_linux.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using neui_detail::ClipboardX11;
using neui_detail::DataItem;

// The MIME formats the foreign owner advertises, in TARGETS order. UTF8_STRING
// (the text format has_text() looks for) is implied last by put_text below.
static const char* kMimes[] = {
  "application/x-neui-a", "application/x-neui-b", "application/x-neui-c",
  "application/x-neui-d", "application/x-neui-e", "application/x-neui-f",
  "application/x-neui-g",
};
static const int kMimeCount = (int)(sizeof(kMimes) / sizeof(kMimes[0]));

static std::atomic<bool> g_owner_ready{false};
static std::atomic<bool> g_stop{false};

// Raw foreign owner: own CLIPBOARD, serve TARGETS + each MIME + UTF8_STRING.
static void owner_thread()
{
  Display* d = XOpenDisplay(nullptr);
  if (!d) { printf("owner: no display\n"); return; }
  Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), -10, -10, 1, 1, 0, 0, 0);
  Atom CLIP    = XInternAtom(d, "CLIPBOARD", False);
  Atom TARGETS = XInternAtom(d, "TARGETS", False);
  Atom UTF8    = XInternAtom(d, "UTF8_STRING", False);
  std::vector<Atom> mime_atoms;
  for (int i = 0; i < kMimeCount; ++i) mime_atoms.push_back(XInternAtom(d, kMimes[i], False));

  XSetSelectionOwner(d, CLIP, w, CurrentTime);
  XFlush(d);
  g_owner_ready = true;

  while (!g_stop) {
    while (XPending(d)) {
      XEvent ev; XNextEvent(d, &ev);
      if (ev.type != SelectionRequest) continue;
      XSelectionRequestEvent& r = ev.xselectionrequest;
      XSelectionEvent se; std::memset(&se, 0, sizeof se);
      se.type = SelectionNotify; se.display = r.display; se.requestor = r.requestor;
      se.selection = r.selection; se.target = r.target; se.time = r.time; se.property = None;
      Atom prop = r.property ? r.property : r.target;
      if (r.target == TARGETS) {
        // TARGETS, mime0..mime6, UTF8_STRING  (UTF8_STRING LAST on purpose).
        std::vector<Atom> tg; tg.push_back(TARGETS);
        for (Atom a : mime_atoms) tg.push_back(a);
        tg.push_back(UTF8);
        XChangeProperty(d, r.requestor, prop, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(tg.data()), (int)tg.size());
        se.property = prop;
      } else if (r.target == UTF8) {
        const char* txt = "hello-lp64";
        XChangeProperty(d, r.requestor, prop, UTF8, 8, PropModeReplace,
                        (unsigned char*)txt, (int)strlen(txt));
        se.property = prop;
      } else {
        // Each MIME serves a distinct one-byte payload (its index).
        for (int i = 0; i < kMimeCount; ++i) {
          if (r.target == mime_atoms[i]) {
            unsigned char b = (unsigned char)('0' + i);
            XChangeProperty(d, r.requestor, prop, r.target, 8, PropModeReplace, &b, 1);
            se.property = prop;
            break;
          }
        }
      }
      XSendEvent(d, r.requestor, False, 0, (XEvent*)&se);
      XFlush(d);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  XCloseDisplay(d);
}

int main()
{
  Display* d = XOpenDisplay(nullptr);
  if (!d) { printf("no display - skipping\n"); return 0; }

  std::thread owner(owner_thread);
  for (int i = 0; i < 500 && !g_owner_ready; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

  ClipboardX11 cb; cb.init(d);

  int fail = 0;

  // has_text() scans the full TARGETS list for a text atom; UTF8_STRING is
  // last, so truncation would hide it.
  bool ht = cb.has_text();
  if (ht) printf("ok:   has_text found UTF8_STRING (tail survived)\n");
  else    { printf("FAIL: has_text false - TARGETS list truncated\n"); ++fail; }

  // read_item() must recover every advertised MIME format + the text.
  DataItem item;
  bool got = cb.read_item(item);
  if (!got) { printf("FAIL: read_item returned nothing\n"); ++fail; }
  int found_mimes = 0;
  for (int i = 0; i < kMimeCount; ++i)
    if (item.has_format(kMimes[i])) ++found_mimes;
  if (found_mimes == kMimeCount)
    printf("ok:   read_item recovered all %d MIME formats\n", kMimeCount);
  else { printf("FAIL: read_item recovered %d/%d MIME formats\n", found_mimes, kMimeCount); ++fail; }

  if (!item.has_format("text/plain;charset=utf-8")) {
    printf("FAIL: read_item missing text\n"); ++fail;
  } else printf("ok:   read_item recovered text\n");

  g_stop = true; owner.join();
  XCloseDisplay(d);
  printf(fail ? "\nCLIPBOARD-TARGETS FAILED (%d)\n" : "\nCLIPBOARD-TARGETS OK\n", fail);
  return fail ? 1 : 0;
}

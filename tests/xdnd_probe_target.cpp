// A minimal XDND v5 DROP TARGET - pure Xlib, deliberately NO neui.
//
// WHY THIS EXISTS. neui_dnd_source_smoke has two modes, and only one of them
// could be run: with no NEUI_DND_TARGET it drops onto its own pane, and that
// INTERNAL path short-circuits the wire protocol entirely - the drag spin calls
// Session::dispatch_dnd_* directly for our own windows (deliberately: an X
// selection round-trip to ourselves would deadlock the blocking spin). So
// XdndEnter / XdndPosition / XdndStatus / XdndDrop / the XdndSelection transfer
// / XdndFinished - every byte neui puts on the wire for a real drag onto another
// application - were never executed by any test.
//
// Exercising them needs a second, genuinely foreign X client that speaks XDND,
// and one cannot assume a desktop has one installed (this machine has no xterm,
// no GTK app, nothing XdndAware at all). Hence a fixture rather than a
// dependency: it is ~150 lines of Xlib and it is what makes the foreign half of
// platform_dnd_begin_drag testable.
//
// USAGE - two processes, this one first:
//
//   ./tests/neui_xdnd_probe_target "xdnd probe target" 900 600 copy &
//   NEUI_DND_TARGET="xdnd probe target" ./tests/neui_dnd_source_smoke
//
// It prints TARGET OK / TARGET FAILED and exits non-zero on failure, so the two
// sides can be asserted independently - the source checks the action it
// negotiated, this checks that the messages and the payload actually arrived.
//
// The last argument decides how it answers XdndPosition:
//   copy   (default) accept as XdndActionCopy  -> source should return COPY
//   move             accept as XdndActionMove  -> source should return MOVE,
//                    which is what proves the source reads the negotiated action
//                    rather than echoing the one it asked for
//   reject           refuse                    -> source must send NO XdndDrop
//                    and return NONE
//
// Note `move` and `reject` make the SOURCE harness report failure, because it
// asserts COPY; they are for driving this by hand, and the interesting output is
// the "begin_drag returned action=N" line.
//
// Place it well away from the source window: the drag picks the TOPMOST toplevel
// under the pointer and descends into that one for an XdndAware window, so an
// overlapping window decides the target before this one is ever consulted.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

static Display* d;
static Window   win;
static Atom A_aware, A_enter, A_position, A_status, A_leave, A_drop, A_finished;
static Atom A_selection, A_action_copy, A_type_list, A_text_plain, A_utf8, A_netname;
static Atom A_prop;

static void send_client(Window to, Atom type, long l0, long l1, long l2, long l3, long l4)
{
  XClientMessageEvent m;
  memset(&m, 0, sizeof m);
  m.type = ClientMessage;
  m.display = d;
  m.window = to;
  m.message_type = type;
  m.format = 32;
  m.data.l[0] = l0; m.data.l[1] = l1; m.data.l[2] = l2; m.data.l[3] = l3; m.data.l[4] = l4;
  XSendEvent(d, to, False, NoEventMask, (XEvent*)&m);
  XFlush(d);
}

int main(int argc, char** argv)
{
  const char* name = argc > 1 ? argv[1] : "xdnd probe target";
  const int px = argc > 3 ? atoi(argv[2]) : 900;
  const int py = argc > 3 ? atoi(argv[3]) : 600;
  // How this target answers XdndPosition: accept as copy (default), accept as
  // move, or refuse outright. Refusing is the mutation that proves the source
  // really PARSES XdndStatus instead of assuming its own requested action.
  const char* mode = argc > 4 ? argv[4] : "copy";

  d = XOpenDisplay(NULL);
  if (!d) { printf("TARGET: no display\n"); return 2; }
  int s = DefaultScreen(d);

  A_aware       = XInternAtom(d, "XdndAware", False);
  A_enter       = XInternAtom(d, "XdndEnter", False);
  A_position    = XInternAtom(d, "XdndPosition", False);
  A_status      = XInternAtom(d, "XdndStatus", False);
  A_leave       = XInternAtom(d, "XdndLeave", False);
  A_drop        = XInternAtom(d, "XdndDrop", False);
  A_finished    = XInternAtom(d, "XdndFinished", False);
  A_selection   = XInternAtom(d, "XdndSelection", False);
  A_action_copy = XInternAtom(d, "XdndActionCopy", False);
  A_type_list   = XInternAtom(d, "XdndTypeList", False);
  A_text_plain  = XInternAtom(d, "text/plain", False);
  A_utf8        = XInternAtom(d, "UTF8_STRING", False);
  A_netname     = XInternAtom(d, "_NET_WM_NAME", False);
  A_prop        = XInternAtom(d, "XDND_PROBE_DATA", False);
  Atom A_action_move = XInternAtom(d, "XdndActionMove", False);
  const int  refuse    = strcmp(mode, "reject") == 0;
  const Atom reply_act = strcmp(mode, "move") == 0 ? A_action_move : A_action_copy;
  printf("TARGET: mode=%s\n", mode); fflush(stdout);

  win = XCreateSimpleWindow(d, RootWindow(d, s), px, py, 400, 300, 2,
                            BlackPixel(d, s), WhitePixel(d, s));
  XStoreName(d, win, name);
  // The harness finds its warp target by _NET_WM_NAME, which XStoreName does
  // NOT set - it sets WM_NAME. Publish both.
  XChangeProperty(d, win, A_netname, A_utf8, 8, PropModeReplace,
                  (const unsigned char*)name, (int)strlen(name));
  // Advertise XDND v5. This single property is what makes the drag's
  // deepest-XdndAware descent stop here.
  {
    long ver = 5;
    XChangeProperty(d, win, A_aware, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&ver, 1);
  }
  XSelectInput(d, win, StructureNotifyMask | ExposureMask | PropertyChangeMask);
  XMapRaised(d, win);
  XFlush(d);

  // Report readiness only once the WM has actually put us on screen, so the
  // driver never drags at a window that is not there yet.
  for (int i = 0; i < 150; i++) {
    XWindowAttributes a;
    int rx = 0, ry = 0; Window ch;
    if (XGetWindowAttributes(d, win, &a) && a.map_state == IsViewable &&
        a.width == 400 && a.height == 300) {
      XTranslateCoordinates(d, win, RootWindow(d, s), 0, 0, &rx, &ry, &ch);
      if (rx > -10000) {
        printf("TARGET: ready 0x%lx '%s' at (%d,%d) %dx%d\n",
               (unsigned long)win, name, rx, ry, a.width, a.height);
        fflush(stdout);
        break;
      }
    }
    usleep(100000);
  }

  Window source = 0;
  int    got_drop = 0, got_enter = 0, positions = 0;
  char   payload[512]; payload[0] = 0;

  const int timeout_s = 60;
  time_t start = time(NULL);
  while (time(NULL) - start < timeout_s) {
    while (XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);

      if (e.type == ClientMessage) {
        if (e.xclient.message_type == A_enter) {
          source   = (Window)e.xclient.data.l[0];
          got_enter = 1;
          printf("TARGET: XdndEnter from 0x%lx (version %ld, more_types=%ld)\n",
                 (unsigned long)source, e.xclient.data.l[1] >> 24,
                 e.xclient.data.l[1] & 1);
          fflush(stdout);
        }
        else if (e.xclient.message_type == A_position) {
          source = (Window)e.xclient.data.l[0];
          ++positions;
          // Accept, in the whole window, as COPY.
          send_client(source, A_status, (long)win, refuse ? 0 : 1,
                      0 /* x<<16|y  */, (400 << 16) | 300,
                      refuse ? (long)None : (long)reply_act);
        }
        else if (e.xclient.message_type == A_leave) {
          printf("TARGET: XdndLeave\n"); fflush(stdout);
        }
        else if (e.xclient.message_type == A_drop) {
          source = (Window)e.xclient.data.l[0];
          Time when = (Time)e.xclient.data.l[2];
          printf("TARGET: XdndDrop - requesting the selection\n"); fflush(stdout);
          XConvertSelection(d, A_selection, A_text_plain, A_prop, win, when);
          XFlush(d);
        }
      }
      else if (e.type == SelectionNotify) {
        if (e.xselection.property != None) {
          Atom t; int f; unsigned long n = 0, rem = 0; unsigned char* v = NULL;
          if (XGetWindowProperty(d, win, A_prop, 0, 1024, True, AnyPropertyType,
                                 &t, &f, &n, &rem, &v) == Success && v) {
            size_t len = n < sizeof payload - 1 ? n : sizeof payload - 1;
            memcpy(payload, v, len);
            payload[len] = 0;
            XFree(v);
          }
        }
        printf("TARGET: got payload '%s'\n", payload); fflush(stdout);
        // Tell the source the drop is complete and which action was taken.
        // Report the action we ACTUALLY took - the source is entitled to trust
        // this field over the earlier XdndStatus.
        if (source) send_client(source, A_finished, (long)win, 1, (long)reply_act, 0, 0);
        got_drop = 1;
      }
    }
    if (got_drop) break;
    usleep(10000);
  }

  printf("TARGET: enter=%d positions=%d drop=%d payload='%s'\n",
         got_enter, positions, got_drop, payload);
  const int ok = refuse ? (got_enter && positions > 0 && !got_drop)
                        : (got_enter && positions > 0 && got_drop);
  printf(ok ? "TARGET OK\n" : "TARGET FAILED\n");
  fflush(stdout);
  XDestroyWindow(d, win);
  XCloseDisplay(d);
  return ok ? 0 : 1;
}

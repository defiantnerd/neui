// Phase 3 acceptance harness for the Linux DAW-embedding path.
//
// Plays the role of a DAW: creates a foreign "host parent" X11 window, embeds
// a neui PLUGWINDOW under it via the public NEUI_API_EMBED interface, then
// drives the UI purely through embed->pump_and_tick on a 16 ms cadence (no
// neui-owned event loop). Verifies the embedded child exists under the parent
// and actually rendered. Linux-only; needs a live X display.

#include <neui/neui.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <unistd.h>

static bool onevent(void*, neui_event_t*) { return false; }
static neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
static void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
static neui_client_t g_client = { NEUI_VERSION, iface };

int main()
{
  Display* hd = XOpenDisplay(nullptr);
  if (!hd) { std::printf("no display - skipping\n"); return 0; }
  int scr = DefaultScreen(hd);

  // Fake DAW host parent window, filled with a known grey.
  Window parent = XCreateSimpleWindow(hd, RootWindow(hd, scr), 60, 60, 320, 200,
                                      0, BlackPixel(hd, scr), 0x00202020);
  XStoreName(hd, parent, "fake daw host");
  XMapWindow(hd, parent);
  XSync(hd, False);
  usleep(200000);

  // Build a neui PLUGWINDOW + a child button via the public API.
  neui_init();
  neui_api_t* api = neui_get_api(nullptr);
  neui_session_t sess = api->create_session(&g_client, nullptr);
  auto* w = (neui_widget_api_t*)api->get_interface(sess, NEUI_API_WIDGETS);
  auto* embed = (neui_embed_api_t*)api->get_interface(sess, NEUI_API_EMBED);
  if (!embed) { std::printf("FAIL: no NEUI_API_EMBED\n"); return 1; }
  neui_widget_t plug = w->create(sess, widget_none, NEUI_W_PLUGWINDOW, 0, 0, 320, 200, nullptr);
  neui_widget_t btn  = w->create(sess, plug, NEUI_W_BUTTON, 20, 20, 160, 32, nullptr);
  w->set_text(sess, btn, "Embedded!");

  // The DAW-provided parent travels as a void* (X11 Window id on Linux).
  if (!embed->set_parent(sess, plug, (void*)(uintptr_t)parent)) {
    std::printf("FAIL: set_parent rejected\n");
    return 1;
  }

  w->show(sess, plug);

  int fd = embed->event_fd(sess, plug);
  std::printf("embed_event_fd = %d\n", fd);

  // The child's background is unpainted = BlackPixel until the Cairo blit lands
  // a non-black themed frame, so "has it rendered" is directly observable.
  auto count_nonblack = [&](Window win) -> long {
    if (!win) return 0;
    XSync(hd, False);
    XImage* img = XGetImage(hd, win, 0, 0, 320, 200, AllPlanes, ZPixmap);
    if (!img) return 0;
    long n = 0;
    for (int y = 0; y < 200; ++y)
      for (int x = 0; x < 320; ++x)
        if ((XGetPixel(img, x, y) & 0xffffff) > 0x080808) ++n;
    XDestroyImage(img);
    return n;
  };

  // Drive the UI purely via the host-driven pump (no neui loop) UNTIL the child
  // has actually rendered, rather than for a fixed number of ticks.
  //
  // An embedded editor paints when the DAW pumps it, and how many pumps that
  // takes depends on the machine and on whatever else is competing for the X
  // server. A fixed ~1 s was enough on an idle desktop and not enough directly
  // after the rest of the Linux suite had run, which failed this harness with
  // "child did not render" about two runs in three - a timing artefact reported
  // as a rendering bug.
  Window child = 0;
  unsigned int nk = 0;
  long nonblack = 0;
  for (int i = 0; i < 700 && nonblack < 500; ++i) {   // up to ~11 s
    embed->pump_and_tick(sess, plug);
    usleep(16000);
    if (i < 20) continue;              // let the window be created and mapped
    if (!child) {
      // The embedded window must be a child of the foreign parent.
      Window rr, pp, *kids = nullptr;
      XQueryTree(hd, parent, &rr, &pp, &kids, &nk);
      if (nk > 0) child = kids[0];
      if (kids) XFree(kids);
    }
    if (child && (i % 4) == 0) nonblack = count_nonblack(child);
  }
  std::printf("parent children = %u\n", nk);
  std::printf("child nonblack = %ld\n", nonblack);

  int fail = 0;
  if (fd < 0)            { std::printf("FAIL: bad event fd\n");       ++fail; }
  if (!child)            { std::printf("FAIL: no embedded child\n");  ++fail; }
  if (nonblack < 500)    { std::printf("FAIL: child did not render\n"); ++fail; }

  w->destroy(sess, plug);
  XDestroyWindow(hd, parent);
  XCloseDisplay(hd);

  std::printf(fail ? "\nEMBED FAILED (%d)\n" : "\nEMBED OK\n", fail);
  return fail ? 1 : 0;
}

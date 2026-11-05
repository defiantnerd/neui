// Drag-preview swap harness (NEUI_API_DND::update_drag_preview), macOS / both
// hosts.
//
// WHAT IT CAN AND CANNOT REACH. A drag image that changes on screen needs a real
// mouse button held down, which a harness cannot press; so what is asserted here
// is everything short of the pixels:
//
//   A. WIRING     - the appended vtable slot is non-NULL on BOTH macOS hosts.
//   B. REFUSAL    - with no drag of this session in flight the call returns
//                   false, whatever it is given: a valid preview, NULL, or an
//                   `image` of asset_none. (NULL / asset_none are refused while
//                   a drag runs too - phase C.)
//   C. LIVE       - only with the argument `live xpl` or `live native`: a real
//                   drag session is begun through begin_drag_with_preview (the
//                   real NSDraggingSession, with two dragging items, one per
//                   URL), and from a timer inside its pump the preview is
//                   swapped. Asserted against the session's own dragging items,
//                   read back through AppKit: both take the new size, the new
//                   image's hot-spot sits at the pointer, a NULL / asset_none
//                   swap is refused and leaves them as they were. The session
//                   has no button held, so it never ends by itself - the
//                   harness exits from the timer, and the drag image flashes at
//                   the pointer meanwhile. What is NOT asserted is the refresh
//                   of the image on screen, which AppKit does on the drag's next
//                   real mouse event.
//
// Realizes real NSWindows (and, in `live`, an OS drag session), so built but NOT
// ctest-registered; run ./tests/<config>/neui_dnd_preview_smoke_macos manually,
// then `... live xpl` and `... live native`, one process each.

#import <AppKit/AppKit.h>

#include <neui/neui.h>
#include <hosts/shared/macos/dnd_source_macos.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

bool onevent(void*, neui_event_t*) { return false; }
neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

void pump_briefly()
{
  [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];
}

// A solid w x h BGRA bitmap asset.
neui_asset_t make_bitmap(neui_asset_api_t* assets, neui_session_t sess,
                         uint32_t w, uint32_t h, uint8_t b, uint8_t g, uint8_t r)
{
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < px.size(); i += 4) { px[i] = b; px[i + 1] = g; px[i + 2] = r; px[i + 3] = 0xFF; }
  return assets->create_bitmap(sess, w, h, px.data(), 1.0f);
}

struct HostApis {
  neui_api_t*           api    = nullptr;
  neui_session_t        sess   = { 0 };
  neui_widget_api_t*    w      = nullptr;
  neui_dnd_api_t*       dnd    = nullptr;
  neui_asset_api_t*     assets = nullptr;
  neui_clipboard_api_t* clip   = nullptr;
  neui_widget_t         win    = {}, src = {};
};

bool open_host(const char* id, const char* title, HostApis* h)
{
  h->api = neui_get_api(id);
  if (!h->api) { std::printf("FAIL: no %s host\n", id); return false; }
  void* app = nullptr;
  h->sess   = h->api->create_session(&g_client, &app);
  h->w      = (neui_widget_api_t*)   h->api->get_interface(h->sess, NEUI_API_WIDGETS);
  h->dnd    = (neui_dnd_api_t*)      h->api->get_interface(h->sess, NEUI_API_DND);
  h->assets = (neui_asset_api_t*)    h->api->get_interface(h->sess, NEUI_API_ASSETS);
  h->clip   = (neui_clipboard_api_t*)h->api->get_interface(h->sess, NEUI_API_CLIPBOARD);
  if (!h->w || !h->dnd || !h->assets || !h->clip) {
    std::printf("FAIL: missing interface on %s\n", id);
    return false;
  }
  h->win = h->w->create(h->sess, widget_none, NEUI_W_APPWINDOW, 80, 120, 320, 200, nullptr);
  h->w->set_text(h->sess, h->win, title);
  h->src = h->w->create(h->sess, h->win, NEUI_W_CUSTOMDRAW, 20, 20, 120, 80, nullptr);
  h->w->show(h->sess, h->win);
  pump_briefly();
  return true;
}

// ---- A + B: no drag in flight ---------------------------------------------

void run_headless(const char* id, const char* title)
{
  std::printf("\n-- %s --\n", id);
  HostApis h;
  if (!open_host(id, title, &h)) { ++g_failures; return; }

  check(h.dnd->update_drag_preview != nullptr, "A: update_drag_preview slot is wired");

  neui_asset_t img = make_bitmap(h.assets, h.sess, 40, 20, 0x30, 0x60, 0xC0);
  check(img.id != asset_none.id, "a bitmap asset to use as the preview");

  neui_drag_preview_t ok_preview = { img, -1, -1 };
  check(!h.dnd->update_drag_preview(h.sess, &ok_preview),
        "B: a valid preview with no drag in flight -> false");
  check(!h.dnd->update_drag_preview(h.sess, nullptr),
        "B: NULL preview -> false");
  neui_drag_preview_t none = { asset_none, 3, 3 };
  check(!h.dnd->update_drag_preview(h.sess, &none),
        "B: image == asset_none -> false");
  neui_drag_preview_t bad_session = { img, 0, 0 };
  check(!h.dnd->update_drag_preview({ 0 }, &bad_session),
        "B: an invalid session -> false");

  h.assets->destroy(h.sess, img);
}

// ---- C: a real session ------------------------------------------------------

struct Live {
  HostApis      h;
  neui_asset_t  first = {}, second = {};
};
Live* g_live = nullptr;

// Every dragging item of the session in flight, read back through AppKit in
// screen coordinates.
struct Frames { int count = 0; bool all_size = false; bool all_origin = false; };
Frames read_frames(NSSize want_size, NSPoint want_origin)
{
  Frames f;
  f.all_size = f.all_origin = true;
  NSDraggingSession* session = neui_detail::dnd_active_drag_session().session;
  if (!session) { f.all_size = f.all_origin = false; return f; }
  __block Frames out = f;
  [session enumerateDraggingItemsWithOptions:0
                                     forView:nil
                                     classes:@[[NSPasteboardItem class]]
                               searchOptions:@{}
                                  usingBlock:^(NSDraggingItem* di, NSInteger idx, BOOL* stop) {
    (void)idx; (void)stop;
    const NSRect fr = [di draggingFrame];
    ++out.count;
    if (std::fabs(fr.size.width - want_size.width) > 0.5 ||
        std::fabs(fr.size.height - want_size.height) > 0.5) out.all_size = false;
    if (std::fabs(fr.origin.x - want_origin.x) > 1.5 ||
        std::fabs(fr.origin.y - want_origin.y) > 1.5) out.all_origin = false;
  }];
  return out;
}

void live_checks()
{
  std::printf("\n-- live session --\n");
  HostApis& h = g_live->h;
  check(neui_detail::dnd_active_drag_session().session != nil,
        "C: the drag session is in flight and reachable");

  // Begun with a 32x16 image: both dragging items carry that size.
  Frames before = read_frames(NSMakeSize(32, 16), NSZeroPoint);
  check(before.count == 2, "C: two dragging items (one per URL)");
  check(before.all_size, "C: both items start at the begin image's size (32 x 16)");

  // Swap to a 20 x 40 image, hot-spot (5, 10) from its top-left.
  neui_drag_preview_t pv = { g_live->second, 5, 10 };
  check(h.dnd->update_drag_preview(h.sess, &pv), "C: update_drag_preview during the drag -> true");

  const NSPoint m = [NSEvent mouseLocation];
  // Bottom-left origin: the image's top sits hot_y above the pointer.
  const NSPoint want_origin = NSMakePoint(m.x - 5, m.y + 10 - 40);
  Frames after = read_frames(NSMakeSize(20, 40), want_origin);
  check(after.count == 2, "C: still two dragging items");
  check(after.all_size,   "C: both items took the new image's size (20 x 40)");
  check(after.all_origin, "C: the hot-spot (5, 10) of the new image sits at the pointer");

  // A refused swap leaves the image as it is.
  neui_drag_preview_t none = { asset_none, 0, 0 };
  check(!h.dnd->update_drag_preview(h.sess, &none), "C: image == asset_none during the drag -> false");
  check(!h.dnd->update_drag_preview(h.sess, nullptr), "C: NULL during the drag -> false");
  Frames kept = read_frames(NSMakeSize(20, 40), want_origin);
  check(kept.all_size && kept.all_origin, "C: the refused swaps left both items as they were");

  // Centre hot-spot (-1) and a third size.
  neui_drag_preview_t centred = { g_live->first, -1, -1 };
  check(h.dnd->update_drag_preview(h.sess, &centred), "C: a swap back with the centre hot-spot -> true");
  const NSPoint m2 = [NSEvent mouseLocation];
  Frames back = read_frames(NSMakeSize(32, 16), NSMakePoint(m2.x - 16, m2.y + 8 - 16));
  check(back.all_size && back.all_origin, "C: -1 / -1 centres the 32 x 16 image on the pointer");

  std::printf("\n%s\n", g_failures ? "DND PREVIEW FAILED" : "dnd preview: all checks passed");
  std::fflush(stdout);
  std::exit(g_failures ? 1 : 0);   // the session has no button held and never ends by itself
}

int run_live(const char* which)
{
  const bool native = std::strcmp(which, "native") == 0;
  const char* id    = native ? "neui.host.macos" : "neui.host.crossplatform";
  std::printf("-- %s, live drag session --\n", id);

  g_live = new Live();
  if (!open_host(id, native ? "dnd preview native" : "dnd preview xpl", &g_live->h)) return 1;
  HostApis& h = g_live->h;
  g_live->first  = make_bitmap(h.assets, h.sess, 32, 16, 0x30, 0x60, 0xC0);
  g_live->second = make_bitmap(h.assets, h.sess, 20, 40, 0xC0, 0x60, 0x30);

  neui_data_item_t item = h.clip->create_item(h.sess);
  const char* uris = "file:///tmp/neui-preview-a.txt\r\nfile:///tmp/neui-preview-b.txt\r\n";
  h.clip->item_set_format(h.sess, item, NEUI_MIME_URI_LIST, uris, (int)std::strlen(uris));
  const char* text = "neui drag preview";
  h.clip->item_set_format(h.sess, item, NEUI_MIME_TEXT, text, (int)std::strlen(text) + 1);

  // Common modes, so the timer fires inside the drag's own event-tracking pump.
  NSTimer* t = [NSTimer timerWithTimeInterval:1.0 repeats:NO block:^(NSTimer*) { live_checks(); }];
  [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
  // Safety net: a session that never reached its pump must not hang the harness.
  NSTimer* wd = [NSTimer timerWithTimeInterval:15.0 repeats:NO block:^(NSTimer*) {
    std::printf("[FAIL]  the live phase never ran\n"); std::fflush(stdout); std::exit(1); }];
  [[NSRunLoop mainRunLoop] addTimer:wd forMode:NSRunLoopCommonModes];

  neui_drag_preview_t begin_pv = { g_live->first, 4, 4 };
  neui_dnd_action_t res = h.dnd->begin_drag_with_preview(
      h.sess, h.src, item, NEUI_DND_ACTION_COPY, &begin_pv);
  // Only reached when the OS ended the drag on its own - which the checks above
  // never saw.
  std::printf("[FAIL]  begin_drag_with_preview returned (%d) before the live checks ran\n", (int)res);
  return 1;
}

} // namespace

int main(int argc, char** argv)
{
  @autoreleasepool {
    [NSApplication sharedApplication];
    neui_init();

    if (argc >= 3 && std::strcmp(argv[1], "live") == 0) {
      setvbuf(stdout, nullptr, _IONBF, 0);
      return run_live(argv[2]);
    }

    run_headless("neui.host.crossplatform", "dnd preview xpl");
    run_headless("neui.host.macos",         "dnd preview native");

    std::printf("\n%s\n", g_failures ? "DND PREVIEW FAILED" : "dnd preview: all checks passed");
    return g_failures ? 1 : 0;
  }
}

// A client's custom MIME on the macOS pasteboard.
//
// A pasteboard type must be a UTI, and a MIME string is not one: writing
// "application/x-example-programs" as a type logs "... is not a valid UTI
// string. Cannot set data for an invalid UTI." and the bytes never arrive. The
// helpers in hosts/shared/macos/pasteboard_mime_macos.h write a custom MIME under
// the UTI of its MIME tag and read it back as the MIME. What this pins down,
// without touching the user's clipboard (every pasteboard here is a unique-named
// one, never the general pasteboard):
//
//   1. MAPPING   - MIME -> type -> MIME is the identity for a custom MIME, a MIME
//                  the system declares a type for, and a MIME with parameters;
//                  the same on a second ask (another process asks the same
//                  question); the types that merely carry a MIME tag stay out.
//   2. CLIPBOARD - pb_write_item_macos / pb_read_item_macos: every format of an
//                  item, custom ones included, comes back byte for byte under its
//                  MIME, the built-ins are as they were, and nothing is logged
//                  about an invalid UTI.
//   3. DRAG SRC  - the NSDraggingItems build_dragging_items makes (eager and lazy
//                  custom MIMEs) read back the same way, the lazy provider is
//                  called with the MIME and only when the bytes are asked for.
//   4. DROP      - on BOTH hosts a frame's content view is registered for the
//                  pasteboard type of the MIMEs its widgets accept (set before
//                  show, and added after show), and a drag whose pasteboard
//                  carries only a custom type reaches the widget: DND_ENTER's
//                  `formats` names the MIME, DND_DROP's data item holds the bytes.
//
// Nothing here drives a real OS drag: AppKit's NSDraggingInfo is replaced by a
// stand-in that answers the three questions the hosts ask of it. Realizes real
// NSWindows, so built but NOT ctest-registered; run
// ./tests/<config>/neui_mime_pasteboard_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../hosts/shared/macos/clipboard_macos.h"
#include "../hosts/shared/macos/dnd_helpers_macos.h"
#include "../hosts/shared/macos/dnd_source_macos.h"

using namespace neui_detail;

// What AppKit's NSDraggingInfo answers, for the three questions the drop
// target methods ask. Deliberately not declared as conforming to the protocol
// (it has more required methods); the hosts only message it.
@interface MimeFakeDragInfo : NSObject
{ @public NSPasteboard* pb; NSPoint loc; NSDragOperation mask; }
- (NSPasteboard*)draggingPasteboard;
- (NSPoint)draggingLocation;
- (NSDragOperation)draggingSourceOperationMask;
@end
@implementation MimeFakeDragInfo
- (NSPasteboard*)draggingPasteboard { return pb; }
- (NSPoint)draggingLocation { return loc; }
- (NSDragOperation)draggingSourceOperationMask { return mask; }
@end

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

const char* const kCustom = "application/x-example-programs";
const char* const kLate   = "application/x-example-late";

std::vector<uint8_t> bytes_of(const char* s)
{ return std::vector<uint8_t>(s, s + std::strlen(s)); }

NSPasteboard* named_pasteboard()
{ return [NSPasteboard pasteboardWithUniqueName]; }

// True if some type on `pb` is named like a MIME - a string AppKit does not take
// for a UTI, which is what the custom MIMEs used to be written as.
bool has_mime_named_type(NSPasteboard* pb)
{
  for (NSPasteboardType t in [pb types])
    if ([t containsString:@"/"]) return true;
  return false;
}

bool item_has(const DataItem& item, const char* mime, const std::vector<uint8_t>& want)
{
  const int n = item.get_format(mime, nullptr, 0);
  if (n != (int)want.size()) return false;
  std::vector<uint8_t> got((size_t)n);
  item.get_format(mime, got.data(), n);
  return got == want;
}

// Everything written to stderr while `body` runs (NSLog goes there).
template <class F> std::string captured_stderr(F&& body)
{
  std::fflush(stderr);
  FILE* tmp = std::tmpfile();
  const int saved = ::dup(2);
  ::dup2(::fileno(tmp), 2);
  body();
  std::fflush(stderr);
  ::dup2(saved, 2);
  ::close(saved);
  std::rewind(tmp);
  std::string out;
  char buf[512];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0) out.append(buf, n);
  std::fclose(tmp);
  return out;
}

// ---------------------------------------------------------------------------
// 1. MAPPING
// ---------------------------------------------------------------------------

std::string type_of(const char* mime)
{
  NSString* t = pb_type_for_mime_macos(mime);
  return t ? std::string([t UTF8String]) : std::string();
}

bool round_trips(const char* mime)
{
  NSString* t = pb_type_for_mime_macos(mime);
  return t && pb_mime_for_type_macos(t) == mime;
}

void check_mapping()
{
  std::printf("\n-- 1. MIME <-> pasteboard type --\n");
  const std::string t = type_of(kCustom);
  std::printf("        %s -> %s\n", kCustom, t.c_str());
  check(!t.empty() && t.find('/') == std::string::npos,
        "a custom MIME maps to a type that is not the MIME string (a valid UTI)");
  check(t == type_of(kCustom), "the same MIME maps to the same type on a second ask");
  check(round_trips(kCustom), "a custom MIME comes back as itself");
  check(round_trips("application/x-example-programs;version=2"),
        "a MIME with a parameter comes back as itself");
  check(type_of("application/x-example-programs;version=2") != t,
        "...and is a different type from the one without");
  check(round_trips("application/json") && type_of("application/json") == "public.json",
        "a MIME the system declares a type for: that type, back as itself (application/json)");
  check(round_trips("text/csv") && round_trips("image/jpeg") && round_trips("application/pdf"),
        "text/csv, image/jpeg, application/pdf come back as themselves");

  // The documented limit: a MIME the system declares under another spelling is
  // written under that type (what other applications look for) and read back
  // under the system's spelling.
  check(pb_mime_for_type_macos(pb_type_for_mime_macos("audio/x-wav")) == "audio/vnd.wave",
        "audio/x-wav is written as the system's WAVE type and read back as audio/vnd.wave");
  check(pb_mime_for_type_macos(pb_type_for_mime_macos("text/xml")) == "application/xml",
        "text/xml is written as public.xml and read back as application/xml");

  check(pb_mime_for_type_macos(@"application/x-legacy") == "application/x-legacy",
        "a type named like a MIME (a legacy writer) is still taken as that MIME");
  check(pb_mime_for_type_macos(@"public.utf8-plain-text") == "text/plain;charset=utf-8"
        && pb_mime_for_type_macos(@"public.html") == "text/html"
        && pb_mime_for_type_macos(@"public.png") == "image/png",
        "the built-in types come out under the built-in MIME names");
  check(pb_mime_for_type_macos(@"public.file-url").empty()
        && pb_mime_for_type_macos(@"public.url").empty()
        && pb_mime_for_type_macos(@"public.data").empty()
        && pb_mime_for_type_macos(@"com.apple.pasteboard.promised-file-url").empty()
        && pb_mime_for_type_macos(@"NSStringPboardType").empty(),
        "the system's own types without a MIME of their own stay out");
  check(pb_type_for_mime_macos("") == nil && pb_mime_for_type_macos(nil).empty(),
        "empty in, nothing out");
}

// ---------------------------------------------------------------------------
// 2. CLIPBOARD
// ---------------------------------------------------------------------------

void check_clipboard()
{
  std::printf("\n-- 2. clipboard write / read on a named pasteboard --\n");
  const std::vector<uint8_t> custom_bin = { 'a', 'l', 'p', 'h', 'a', 0, 'b', 0x01, 0xff, 0x80 };
  const std::vector<uint8_t> html   = bytes_of("<b>hi</b>");
  const std::vector<uint8_t> png    = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3 };
  const std::vector<uint8_t> json   = bytes_of("{\"a\":1}");

  DataItem item;
  item.set_format("text/plain;charset=utf-8", "hello\0", 6);
  item.set_format("text/html", html.data(), (uint32_t)html.size());
  item.set_format("image/png", png.data(), (uint32_t)png.size());
  item.set_format(kCustom, custom_bin.data(), (uint32_t)custom_bin.size());
  item.set_format("application/json", json.data(), (uint32_t)json.size());

  NSPasteboard* pb = named_pasteboard();
  bool wrote = false;
  const std::string log = captured_stderr([&] { wrote = pb_write_item_macos(pb, item); });
  check(wrote, "pb_write_item_macos publishes the item");
  check(log.find("not a valid UTI") == std::string::npos,
        "...and AppKit logs nothing about an invalid UTI");
  if (!log.empty()) std::printf("        stderr: %s\n", log.c_str());

  check(!has_mime_named_type(pb), "...and no type on the pasteboard is a bare MIME string");
  NSString* ctype = pb_type_for_mime_macos(kCustom);
  NSData* raw = [pb dataForType:ctype];
  check(raw && [raw length] == custom_bin.size()
        && std::memcmp([raw bytes], custom_bin.data(), custom_bin.size()) == 0,
        "the custom bytes are on the pasteboard under the MIME's type");

  DataItem back;
  check(pb_read_item_macos(pb, back), "pb_read_item_macos reads the pasteboard");
  check(item_has(back, kCustom, custom_bin), "custom MIME: same bytes under the same MIME");
  check(item_has(back, "application/json", json), "application/json: same bytes under the same MIME");
  check(item_has(back, "text/html", html), "text/html is as it was");
  check(item_has(back, "image/png", png), "image/png is as it was");
  check(back.has_format("text/plain;charset=utf-8"), "text/plain is as it was");

  auto mimes = pb_collect_mimes_macos(pb);
  auto listed = [&](const char* m) {
    for (auto& s : mimes) if (s == m) return true;
    return false;
  };
  check(listed(kCustom) && listed("application/json") && listed("text/html")
        && listed("image/png") && listed("text/plain;charset=utf-8"),
        "pb_collect_mimes_macos lists them all by MIME name (what DND_ENTER's `formats` is made of)");
  int dups = 0;
  for (size_t i = 0; i < mimes.size(); ++i)
    for (size_t j = i + 1; j < mimes.size(); ++j) if (mimes[i] == mimes[j]) ++dups;
  check(dups == 0, "...each once");
}

// ---------------------------------------------------------------------------
// 3. DRAG SOURCE
// ---------------------------------------------------------------------------

int g_lazy_calls = 0;
std::string g_lazy_mime;
const uint8_t* lazy_provider(void*, const char* mime, uint32_t* size)
{
  ++g_lazy_calls;
  g_lazy_mime = mime ? mime : "";
  static const uint8_t bytes[] = { 'l', 'a', 'z', 'y', 0, 7 };
  *size = (uint32_t)sizeof bytes;
  return bytes;
}

void check_drag_source()
{
  std::printf("\n-- 3. drag-source items on a named pasteboard --\n");
  const std::vector<uint8_t> eager = { 'e', 'a', 'g', 'e', 'r', 0, 9 };
  const std::vector<uint8_t> lazy  = { 'l', 'a', 'z', 'y', 0, 7 };
  DataItem item;
  item.set_format("text/plain;charset=utf-8", "drag\0", 5);
  item.set_format(kCustom, eager.data(), (uint32_t)eager.size());
  item.set_format_provider("application/x-example-lazy", lazy_provider, nullptr);

  NSArray<NSDraggingItem*>* items = nil;
  const std::string log = captured_stderr([&] {
    items = build_dragging_items(item, NSMakePoint(10, 10), nil, -1, -1);
  });
  check([items count] == 1, "build_dragging_items makes one item for a payload without URLs");
  check(log.find("not a valid UTI") == std::string::npos,
        "...and AppKit logs nothing about an invalid UTI while building it");

  NSPasteboard* pb = named_pasteboard();
  [pb clearContents];
  NSMutableArray* writers = [NSMutableArray array];
  for (NSDraggingItem* di in items) [writers addObject:di.item];
  const std::string wlog = captured_stderr([&] { [pb writeObjects:writers]; });
  check(wlog.find("not a valid UTI") == std::string::npos, "...nor while writing it");

  check(!has_mime_named_type(pb), "...and no type on the pasteboard is a bare MIME string");
  g_lazy_calls = 0;
  auto mimes = pb_collect_mimes_macos(pb);
  auto listed = [&](const char* m) {
    for (auto& s : mimes) if (s == m) return true;
    return false;
  };
  check(listed(kCustom) && listed("application/x-example-lazy"),
        "both custom MIMEs are listed by MIME name, as a DND_ENTER's `formats` would be");
  check(g_lazy_calls == 0, "listing the formats does not call the lazy provider");

  DataItem back;
  pb_read_item_macos(pb, back);
  check(item_has(back, kCustom, eager), "the eager custom MIME: same bytes under the same MIME");
  check(item_has(back, "application/x-example-lazy", lazy),
        "the lazy custom MIME: the provider's bytes under the same MIME");
  check(g_lazy_calls >= 1 && g_lazy_mime == "application/x-example-lazy",
        "...and the provider was called with the MIME, not the type");
}

// ---------------------------------------------------------------------------
// 4. DROP, both hosts
// ---------------------------------------------------------------------------

neui_session_t       g_sess{};
neui_dnd_api_t*      g_dnd  = nullptr;
neui_clipboard_api_t* g_clip = nullptr;
uint32_t             g_target = 0;
std::vector<std::string> g_enter_formats;
int                  g_enters = 0;
std::vector<uint8_t> g_dropped;

bool onevent(void*, neui_event_t* ev)
{
  if (ev->type != NEUI_EVENT_DND_ENTER && ev->type != NEUI_EVENT_DND_MOVE
      && ev->type != NEUI_EVENT_DND_DROP) return false;
  const neui_event_dnd_t& d = ev->data.dnd;
  if (d.widget.id != g_target) return false;
  if (ev->type == NEUI_EVENT_DND_ENTER) {
    ++g_enters;
    g_enter_formats.clear();
    for (uint32_t i = 0; i < d.formats_count; ++i) g_enter_formats.emplace_back(d.formats[i]);
  }
  if (ev->type == NEUI_EVENT_DND_DROP) {
    const int n = g_clip->item_get_format(g_sess, d.data, kCustom, nullptr, 0);
    g_dropped.assign(n > 0 ? (size_t)n : 0, 0);
    if (n > 0) g_clip->item_get_format(g_sess, d.data, kCustom, g_dropped.data(), n);
  }
  g_dnd->accept(g_sess, NEUI_DND_ACTION_COPY);
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

NSWindow* window_titled(NSString* title)
{
  for (NSWindow* w in [NSApp windows])
    if ([[w title] isEqualToString:title]) return w;
  return nil;
}

void pump_briefly()
{ [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]]; }

bool registered_for(NSView* v, NSString* type)
{ return type && [[v registeredDraggedTypes] containsObject:type]; }

void check_drop(const char* host_id, const char* title)
{
  std::printf("\n-- 4. drop onto %s --\n", host_id);
  neui_api_t* api = neui_get_api(host_id);
  if (!api) { check(false, "the host is registered"); return; }
  void* app = nullptr;
  g_sess = api->create_session(&g_client, &app);
  auto* w = (neui_widget_api_t*)api->get_interface(g_sess, NEUI_API_WIDGETS);
  g_dnd  = (neui_dnd_api_t*)api->get_interface(g_sess, NEUI_API_DND);
  g_clip = (neui_clipboard_api_t*)api->get_interface(g_sess, NEUI_API_CLIPBOARD);
  if (!w || !g_dnd || !g_clip) { check(false, "the widget / dnd / clipboard interfaces"); return; }

  // 320x200 client; the first target is at (20,20) 120x80, the second at (160,20).
  neui_widget_t win = w->create(g_sess, widget_none, NEUI_W_APPWINDOW, 80, 120, 320, 200, nullptr);
  w->set_text(g_sess, win, title);
  neui_widget_t t1 = w->create(g_sess, win, NEUI_W_CUSTOMDRAW, 20, 20, 120, 80, nullptr);
  const char* const accepted[] = { kCustom };
  g_dnd->set_drop_target(g_sess, t1, true);
  g_dnd->set_accepted_formats(g_sess, t1, accepted, 1);   // BEFORE show
  w->show(g_sess, win);
  pump_briefly();

  NSWindow* nswin = window_titled([NSString stringWithUTF8String:title]);
  if (!nswin) { check(false, "the window is realized"); return; }
  NSView* view = [nswin contentView];

  NSString* ctype = pb_type_for_mime_macos(kCustom);
  check(registered_for(view, ctype), "the content view is registered for the type of a MIME set before show");
  check(registered_for(view, NSPasteboardTypeString) && registered_for(view, NSPasteboardTypeHTML)
        && registered_for(view, NSPasteboardTypeFileURL),
        "...on top of the three it always was");

  neui_widget_t t2 = w->create(g_sess, win, NEUI_W_CUSTOMDRAW, 160, 20, 120, 80, nullptr);
  const char* const late[] = { kLate };
  g_dnd->set_drop_target(g_sess, t2, true);
  check(!registered_for(view, pb_type_for_mime_macos(kLate)), "a MIME nobody accepted yet is not registered");
  g_dnd->set_accepted_formats(g_sess, t2, late, 1);        // AFTER show
  pump_briefly();
  check(registered_for(view, pb_type_for_mime_macos(kLate)), "a MIME set after show is added to the registration");
  check(registered_for(view, ctype), "...and the first one is still there");

  // A drag whose pasteboard carries ONLY the custom type, over the first target.
  NSPasteboard* pb = named_pasteboard();
  DataItem item;
  const std::vector<uint8_t> payload = { 'd', 'r', 'o', 'p', 0, 0x42 };
  item.set_format(kCustom, payload.data(), (uint32_t)payload.size());
  pb_write_item_macos(pb, item);
  check(pb_collect_mimes_macos(pb).size() == 1, "(the stand-in drag carries only the custom type)");

  const CGFloat h = [view bounds].size.height;
  MimeFakeDragInfo* info = [[MimeFakeDragInfo alloc] init];
  info->pb   = pb;
  info->mask = NSDragOperationCopy;
  info->loc  = NSMakePoint(50, h - 50);
  g_target = t1.id;
  g_enters = 0;
  g_enter_formats.clear();
  const NSDragOperation op = [(id)view draggingEntered:(id)info];
  check(g_enters == 1, "DND_ENTER reaches the widget that accepts the MIME");
  check(g_enter_formats.size() == 1 && g_enter_formats[0] == kCustom,
        "...with `formats` naming the MIME, not the pasteboard type");
  check(op == NSDragOperationCopy, "...and the answer the client gave reaches AppKit");

  g_dropped.clear();
  const BOOL dropped = [(id)view performDragOperation:(id)info];
  check(dropped == YES, "the drop is performed");
  check(g_dropped == payload, "DND_DROP's data item holds the custom bytes");
  [(id)view draggingExited:(id)info];

  // The same drag over the second target, which accepts another MIME.
  info->loc = NSMakePoint(200, h - 50);
  g_target = t2.id;
  g_enters = 0;
  const NSDragOperation op2 = [(id)view draggingEntered:(id)info];
  check(g_enters == 0 && op2 == NSDragOperationNone,
        "a widget that accepts another MIME is not offered it");
  [(id)view draggingExited:(id)info];

  w->destroy(g_sess, win);
  pump_briefly();
}

} // namespace

int main()
{
  @autoreleasepool {
    [NSApplication sharedApplication];
    neui_init();

    check_mapping();
    check_clipboard();
    check_drag_source();
    check_drop("neui.host.crossplatform", "mime pasteboard xpl");
    check_drop("neui.host.macos",         "mime pasteboard native");

    std::printf("\n%s\n", g_failures ? "MIME PASTEBOARD FAILED"
                                     : "mime pasteboard: all checks passed");
  }
  return g_failures ? 1 : 0;
}

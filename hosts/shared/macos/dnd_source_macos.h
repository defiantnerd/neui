#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../clipboard_item.h"
#include "../../../include/neui/d/dnd.h"
#include "dnd_helpers_macos.h"   // dnd_nsop_from_action

// Drag-source side of NEUI_API_DND on macOS. Mirror of the
// NSDraggingDestination plumbing in NEUIView / NEUINativeContentView.
//
// AppKit drives drag sessions asynchronously via the runloop. To match
// the synchronous Win32 contract this helper spins the existing nested
// pump (`platform_run_modal_until`) until the dragging-source completion
// callback fires.

namespace neui_detail
{
  // <NSDraggingSource> implementation. Captures the negotiated operation
  // when AppKit calls draggingSession:endedAtPoint:operation: and flips
  // `done` so the modal pump can exit.
}

@interface NEUIDragSource : NSObject<NSDraggingSource>
{
@public
  bool             done;        // pump flag
  bool             began;       // session observed entering (watchdog probe)
  NSDragOperation  finalOp;     // result reported by AppKit
  NSDragOperation  allowedOps;  // mask from the client
}
@end

// Provider delegate retained by NSPasteboardItem.setDataProvider:. Holds a
// snapshot of (mime, provider, userdata) tuples taken at registration
// time. AppKit keeps the delegate alive for the lifetime of the
// pasteboard / drag session and fires
// pasteboard:item:provideDataForType: when something asks for one of the
// registered types. The snapshot model avoids retaining a pointer to the
// source DataItem - safe even when the DataItem is released right after
// the registration call returns (the clipboard case).
@interface NEUIDataProviderDelegate : NSObject<NSPasteboardItemDataProvider>
- (void)addMapping:(NSString*)type
              mime:(const std::string&)mime
          provider:(neui_detail::DataProviderFn)provider
          userdata:(void*)userdata;
@end
#ifdef NEUI_DND_SOURCE_MACOS_IMPLEMENTATION
@implementation NEUIDataProviderDelegate
{
  // ivar: heap-allocated map mime -> (provider, userdata). The pasteboard
  // type the OS hands back to us in provideDataForType: is the same string
  // we registered with setDataProvider: (built via stringWithUTF8String
  // from the MIME), so its UTF8String is a key into this map.
  std::unordered_map<std::string,
                      std::pair<neui_detail::DataProviderFn, void*>>* _mappings;
}
- (instancetype)init
{
  if ((self = [super init])) {
    _mappings = new std::unordered_map<std::string,
                                         std::pair<neui_detail::DataProviderFn, void*>>();
  }
  return self;
}
- (void)addMapping:(NSString*)type
              mime:(const std::string&)mime
          provider:(neui_detail::DataProviderFn)provider
          userdata:(void*)userdata
{
  (void)type;
  if (_mappings) (*_mappings)[mime] = std::make_pair(provider, userdata);
}
- (void)dealloc
{
  delete _mappings;
}
- (void)pasteboard:(NSPasteboard*)pb
              item:(NSPasteboardItem*)item
provideDataForType:(NSPasteboardType)type
{
  (void)pb;
  if (!_mappings || !type) return;
  const char* c = [type UTF8String];
  if (!c) return;
  auto it = _mappings->find(std::string(c));
  if (it == _mappings->end()) return;
  auto  provider = it->second.first;
  void* userdata = it->second.second;
  uint32_t size = 0;
  const uint8_t* p = provider ? provider(userdata, c, &size) : nullptr;
  if (p && size > 0) {
    NSData* d = [NSData dataWithBytes:p length:size];
    if (d) [item setData:d forType:type];
  }
}
@end
#endif // NEUI_DND_SOURCE_MACOS_IMPLEMENTATION

// This header is included by both the native (hosts/macos) and xpl
// (hosts/crossplatform) macOS hosts, which co-link in every example binary.
// An Obj-C @implementation is not inline/weak, so it would clash. Exactly one
// TU - the xpl platform_macos.mm - defines NEUI_DND_SOURCE_MACOS_IMPLEMENTATION
// to emit the class body; the other TUs see only the @interface + inline
// helpers below and resolve the class symbol at link.
#ifdef NEUI_DND_SOURCE_MACOS_IMPLEMENTATION
@implementation NEUIDragSource
- (instancetype)init
{
  if ((self = [super init])) {
    done       = false;
    began      = false;
    finalOp    = NSDragOperationNone;
    allowedOps = NSDragOperationNone;
  }
  return self;
}
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
   sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
  (void)session; (void)context;
  return allowedOps;
}
- (void)draggingSession:(NSDraggingSession*)session
        willBeginAtPoint:(NSPoint)screenPoint
{
  (void)session; (void)screenPoint;
  began = true;
}
- (void)draggingSession:(NSDraggingSession*)session
            endedAtPoint:(NSPoint)screenPoint
               operation:(NSDragOperation)op
{
  (void)session; (void)screenPoint;
  finalOp = op;
  done    = true;
}
@end
#endif // NEUI_DND_SOURCE_MACOS_IMPLEMENTATION

namespace neui_detail
{
  // action -> NSDragOperation mask lives in dnd_helpers_macos.h
  // (dnd_nsop_from_action), shared with the drop-target side.

  inline uint32_t nsop_to_dnd_action(NSDragOperation op)
  {
    // The OS reports one bit. Probe in the order COPY > MOVE > LINK to
    // match the same priority the Win32 path uses on DROPEFFECT.
    if (op & NSDragOperationCopy) return NEUI_DND_ACTION_COPY;
    if (op & NSDragOperationMove) return NEUI_DND_ACTION_MOVE;
    if (op & NSDragOperationLink) return NEUI_DND_ACTION_LINK;
    return 0;
  }

  // Synchronous drag requires spinning the runloop until AppKit reports the
  // session ended. Self-contained here (rather than the xpl-only
  // platform_run_modal_until seam) so the native macOS host - which doesn't
  // implement that seam - links against the same shared header.
  //
  // Safety: AppKit drives the drag in NSEventTrackingRunLoopMode, so pump
  // in that mode. Never block on distantFuture - teardown paths that skip
  // draggingSession:endedAtPoint:operation: (window destroyed mid-drag,
  // exception in a pasteboard writer) would otherwise freeze the app.
  // A short timeout keeps the loop responsive even when no events arrive,
  // and a watchdog bails out if the session was never observed beginning.
  inline void dnd_pump_until(NEUIDragSource* src)
  {
    if (!src) return;
    NSDate* watchdog = [NSDate dateWithTimeIntervalSinceNow:3.0];
    while (!src->done) {
      NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]
                                          inMode:NSEventTrackingRunLoopMode
                                         dequeue:YES];
      if (ev) [NSApp sendEvent:ev];
      // Watchdog: if AppKit never reported the session beginning within a
      // few seconds, the drag was stillborn - exit rather than hang.
      if (!src->began && [watchdog timeIntervalSinceNow] < 0) {
        src->finalOp = NSDragOperationNone;
        break;
      }
    }
  }

  // Build the array of NSDraggingItems from the DataItem's formats.
  // Format mapping mirrors clipboard_write_item_macos / pb_read_item_macos
  // but writes onto NSPasteboardItem rather than the general pasteboard.
  // text/uri-list expands to one NSDraggingItem per URL (AppKit's
  // standard multi-file drag shape); every other MIME goes on one shared
  // pasteboard item.
  //
  // `preview` is the optional drag-preview image. When nil, the function
  // synthesizes the historic 24x24 rounded-rect placeholder. `hot_x` /
  // `hot_y` are the hot-spot on the preview image (logical px from the
  // image top-left); -1 on either axis defaults to the image centre.
  inline NSArray<NSDraggingItem*>*
  build_dragging_items(const DataItem& item, NSPoint anchor_view_pt,
                        NSImage* preview, int hot_x, int hot_y)
  {
    NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];

    NSImage* image = preview;
    if (!image) {
      // Synthesize the historic 24x24 rounded-rect placeholder so the no-
      // preview path keeps its previous look.
      image = [[NSImage alloc] initWithSize:NSMakeSize(24, 24)];
      [image lockFocus];
      NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:
                            NSMakeRect(2, 2, 20, 20) xRadius:4 yRadius:4];
      [[NSColor colorWithCalibratedWhite:0.8 alpha:0.8] setFill];
      [p fill];
      [[NSColor colorWithCalibratedWhite:0.2 alpha:0.9] setStroke];
      [p setLineWidth:1.0];
      [p stroke];
      [image unlockFocus];
    }

    NSSize image_size = [image size];
    CGFloat eff_hot_x = (hot_x < 0) ? (image_size.width  * 0.5) : (CGFloat)hot_x;
    CGFloat eff_hot_y = (hot_y < 0) ? (image_size.height * 0.5) : (CGFloat)hot_y;

    auto frame_for = [&](void) -> NSRect {
      // NSView is isFlipped=YES in our hosts, so hot_y measured from the
      // image top-left maps directly to subtraction in view space.
      return NSMakeRect(anchor_view_pt.x - eff_hot_x,
                         anchor_view_pt.y - eff_hot_y,
                         image_size.width, image_size.height);
    };

    NSImage* placeholder = image;  // alias kept so call sites below stay terse

    // Collect text/html/MIME payloads once, keyed by NSPasteboard type.
    // Win32's IDataObject serves every format from one composite object;
    // mirror that by stamping these shared payloads onto *every*
    // NSPasteboardItem we emit, so each NSDraggingItem carries the full
    // context regardless of which one the receiver reads.
    NSMutableDictionary<NSPasteboardType, id>* shared_payloads =
      [NSMutableDictionary dictionary];

    // Lazy-MIME registrations (arbitrary MIMEs only - built-in MIMEs always
    // materialise eagerly because the OS-side transformations need the
    // bytes). Held off shared_payloads so AppKit gets a setDataProvider:
    // call instead of setData:. The pasteboard type is built fresh from
    // the MIME string at stamp time so the C++ vector stays free of
    // Obj-C-pointer members (ARC + std::vector aggregate semantics are a
    // known footgun area).
    struct LazyMime { std::string mime;
                       DataProviderFn provider;
                       void* userdata; };
    std::vector<LazyMime> lazy_mimes;

    // text/uri-list: emit one NSDraggingItem per URL.
    std::vector<NSURL*> uri_urls;

    auto fetch_bytes = [&](const std::string& mime) {
      std::vector<uint8_t> v;
      int n = item.get_format(mime, nullptr, 0);
      if (n > 0) {
        v.resize(static_cast<size_t>(n));
        item.get_format(mime, v.data(), n);
      }
      return v;
    };

    item.for_each_mime([&](const std::string& mime) {
      if (mime == "text/plain;charset=utf-8" || mime == "text/plain") {
        auto bytes = fetch_bytes(mime);
        uint32_t n = static_cast<uint32_t>(bytes.size());
        if (n > 0 && bytes[n - 1] == 0) --n;
        NSString* s = [[NSString alloc] initWithBytes:bytes.data()
                                                length:n
                                              encoding:NSUTF8StringEncoding];
        if (s) shared_payloads[NSPasteboardTypeString] = s;
        return;
      }
      if (mime == "text/html") {
        auto bytes = fetch_bytes(mime);
        NSData* d = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        if (d) shared_payloads[NSPasteboardTypeHTML] = d;
        return;
      }
      if (mime == "text/uri-list") {
        auto bytes = fetch_bytes(mime);
        const char* p = reinterpret_cast<const char*>(bytes.data());
        size_t len = bytes.size();
        size_t i = 0;
        while (i < len) {
          size_t end = i;
          while (end < len && p[end] != '\r' && p[end] != '\n') ++end;
          // Trim trailing whitespace, matching the Win32 urilist_parse.
          while (end > i && (p[end - 1] == ' ' || p[end - 1] == '\t')) --end;
          if (end > i) {
            NSString* line = [[NSString alloc] initWithBytes:p + i
                                                      length:end - i
                                                    encoding:NSUTF8StringEncoding];
            if (line && ![line hasPrefix:@"#"]) {
              NSURL* u = [NSURL URLWithString:line];
              if (u) uri_urls.push_back(u);
            }
          }
          while (end < len && (p[end] != '\r' && p[end] != '\n')) ++end;
          while (end < len && (p[end] == '\r' || p[end] == '\n')) ++end;
          i = end;
        }
        return;
      }
      // Arbitrary MIME: eager-pass-through unless registered as a lazy
      // provider. Lazy entries land in lazy_mimes and ride
      // setDataProvider: on every emitted NSPasteboardItem.
      DataProviderFn provider = nullptr;
      void*          userdata = nullptr;
      if (item.get_lazy_provider(mime, &provider, &userdata)) {
        lazy_mimes.push_back({ mime, provider, userdata });
        return;
      }
      NSString* t = [NSString stringWithUTF8String:mime.c_str()];
      if (!t) return;
      auto bytes = fetch_bytes(mime);
      NSData* d = [NSData dataWithBytes:bytes.data() length:bytes.size()];
      if (!d) return;
      shared_payloads[t] = d;
      // image/png: also stamp under NSPasteboardTypePNG so native macOS
      // receivers (Preview, Photos, Mail) see the image. Same bytes -
      // PNG is the native macOS bitmap format.
      if (mime == "image/png") {
        shared_payloads[NSPasteboardTypePNG] = d;
      }
    });

    // Build a single shared delegate + the parallel NSString* types array.
    // The types array is an NSArray (ARC-owned), keeping it disjoint from
    // the C++ lazy_mimes vector. AppKit retains the delegate via
    // setDataProvider:.
    NEUIDataProviderDelegate* lazy_delegate = nil;
    NSMutableArray<NSPasteboardType>* lazy_types = nil;
    if (!lazy_mimes.empty()) {
      lazy_delegate = [[NEUIDataProviderDelegate alloc] init];
      lazy_types    = [NSMutableArray array];
      for (auto& lm : lazy_mimes) {
        NSString* t = [NSString stringWithUTF8String:lm.mime.c_str()];
        if (!t) continue;
        [lazy_delegate addMapping:t
                              mime:lm.mime
                          provider:lm.provider
                          userdata:lm.userdata];
        [lazy_types addObject:t];
      }
    }

    auto stamp_shared = [&](NSPasteboardItem* pbitem) {
      for (NSPasteboardType type in shared_payloads) {
        id val = shared_payloads[type];
        if ([val isKindOfClass:[NSString class]])
          [pbitem setString:(NSString*)val forType:type];
        else
          [pbitem setData:(NSData*)val forType:type];
      }
      if (lazy_delegate && lazy_types && [lazy_types count] > 0) {
        [pbitem setDataProvider:lazy_delegate forTypes:lazy_types];
      }
    };

    // Per-URL items, each carrying the full shared payload alongside its
    // own URL type. (If the absoluteString form doesn't survive Finder's
    // round-trip we'd switch to setPropertyList:forType: instead.)
    for (NSURL* u : uri_urls) {
      NSPasteboardItem* pbitem = [[NSPasteboardItem alloc] init];
      stamp_shared(pbitem);
      [pbitem setString:[u absoluteString]
                forType:[u isFileURL] ? NSPasteboardTypeFileURL
                                      : NSPasteboardTypeURL];
      NSDraggingItem* di = [[NSDraggingItem alloc] initWithPasteboardWriter:pbitem];
      [di setDraggingFrame:frame_for() contents:placeholder];
      [items addObject:di];
    }

    // No URLs: the shared payloads alone are the drag.
    if (uri_urls.empty() && [shared_payloads count] > 0) {
      NSPasteboardItem* pbitem = [[NSPasteboardItem alloc] init];
      stamp_shared(pbitem);
      NSDraggingItem* di = [[NSDraggingItem alloc] initWithPasteboardWriter:pbitem];
      [di setDraggingFrame:frame_for() contents:placeholder];
      [items addObject:di];
    }
    return items;
  }

  // Synthesize an NSEvent fit for beginDraggingSessionWithItems:event:source:
  // when NSApp.currentEvent isn't a mouse event we can reuse.
  inline NSEvent* synthesize_drag_trigger(NSView* anchor_view, NSWindow* win,
                                           NSPoint pt_in_view)
  {
    NSEvent* cur = [NSApp currentEvent];
    if (cur) {
      NSEventType t = [cur type];
      if (t == NSEventTypeLeftMouseDown ||
          t == NSEventTypeLeftMouseDragged ||
          t == NSEventTypeRightMouseDown ||
          t == NSEventTypeRightMouseDragged)
        return cur;
    }
    NSPoint pt_in_win = [anchor_view convertPoint:pt_in_view toView:nil];
    return [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                               location:pt_in_win
                          modifierFlags:0
                              timestamp:[[NSProcessInfo processInfo] systemUptime]
                           windowNumber:[win windowNumber]
                                context:nil
                            eventNumber:0
                             clickCount:1
                               pressure:1.0f];
  }

  // Build an NSImage from a raw BGRA8 premultiplied pixel buffer (the
  // layout used by every neui asset manager). `scale` lets the OS render
  // an @2x asset at half its pixel dimensions (logical size = pixel size
  // / scale). Returns nil on bad args.
  inline NSImage* macos_make_drag_nsimage(const uint8_t* bgra_premul,
                                            uint32_t w_px, uint32_t h_px,
                                            float scale)
  {
    if (!bgra_premul || w_px == 0 || h_px == 0) return nil;
    if (scale <= 0.0f) scale = 1.0f;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) return nil;
    // kCGImageAlphaPremultipliedFirst + Little32 = BGRA premultiplied
    // (the byte order matches our asset_manager pixel layout).
    CGBitmapInfo info = (CGBitmapInfo)kCGImageAlphaPremultipliedFirst
                       | kCGBitmapByteOrder32Little;
    CGContextRef ctx = CGBitmapContextCreate(nullptr, w_px, h_px,
                                              8, w_px * 4, cs, info);
    CGColorSpaceRelease(cs);
    if (!ctx) return nil;
    std::memcpy(CGBitmapContextGetData(ctx), bgra_premul,
                 static_cast<size_t>(w_px) * h_px * 4);
    CGImageRef cg = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!cg) return nil;
    NSSize logical = NSMakeSize((CGFloat)w_px / scale,
                                 (CGFloat)h_px / scale);
    NSImage* img = [[NSImage alloc] initWithCGImage:cg size:logical];
    CGImageRelease(cg);
    return img;
  }

  // Entry point used by platform_macos.mm and hosts/macos/widgets.mm.
  // `preview` may be nil (no custom drag image, falls back to the historic
  // placeholder). `hot_x`/`hot_y` -1 = image centre on that axis.
  inline uint32_t macos_run_drag_source(NSView* anchor_view,
                                         const DataItem& item,
                                         uint32_t allowed_actions,
                                         NSImage* preview = nil,
                                         int hot_x = -1, int hot_y = -1)
  {
    if (!anchor_view) return 0;
    if (!allowed_actions) return 0;

    // A detached view can't host a drag session - AppKit would receive an
    // NSEvent with windowNumber:0 and misbehave. Bail early.
    NSWindow* win = [anchor_view window];
    if (!win) return 0;

    // Anchor the drag at the current cursor (converted into view coords).
    NSPoint mouse_screen = [NSEvent mouseLocation];
    NSPoint mouse_win    = [win convertPointFromScreen:mouse_screen];
    NSPoint mouse_view   = [anchor_view convertPoint:mouse_win fromView:nil];

    NSArray<NSDraggingItem*>* items =
      build_dragging_items(item, mouse_view, preview, hot_x, hot_y);
    if ([items count] == 0) return 0;

    NEUIDragSource* src = [[NEUIDragSource alloc] init];
    src->allowedOps = dnd_nsop_from_action(allowed_actions);

    NSEvent* evt = synthesize_drag_trigger(anchor_view, win, mouse_view);
    NSDraggingSession* session =
      [anchor_view beginDraggingSessionWithItems:items event:evt source:src];
    if (!session) return 0;

    dnd_pump_until(src);
    return nsop_to_dnd_action(src->finalOp);
  }
}

#endif // __APPLE__

#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#include <cstdint>
#include <string>
#include <vector>

#include "../clipboard_item.h"
#include "../../../include/neui/d/dnd.h"

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
  inline NSDragOperation dnd_action_to_nsop(uint32_t action)
  {
    NSDragOperation mask = NSDragOperationNone;
    if (action & NEUI_DND_ACTION_COPY) mask |= NSDragOperationCopy;
    if (action & NEUI_DND_ACTION_MOVE) mask |= NSDragOperationMove;
    if (action & NEUI_DND_ACTION_LINK) mask |= NSDragOperationLink;
    return mask;
  }

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
  inline NSArray<NSDraggingItem*>*
  build_dragging_items(const DataItem& item, NSPoint anchor_view_pt)
  {
    NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];

    // Shared placeholder image: a small rounded rect drawn once.
    NSImage* placeholder = [[NSImage alloc] initWithSize:NSMakeSize(24, 24)];
    [placeholder lockFocus];
    NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:
                          NSMakeRect(2, 2, 20, 20) xRadius:4 yRadius:4];
    [[NSColor colorWithCalibratedWhite:0.8 alpha:0.8] setFill];
    [p fill];
    [[NSColor colorWithCalibratedWhite:0.2 alpha:0.9] setStroke];
    [p setLineWidth:1.0];
    [p stroke];
    [placeholder unlockFocus];

    auto frame_for = [&](void) -> NSRect {
      return NSMakeRect(anchor_view_pt.x - 12, anchor_view_pt.y - 12, 24, 24);
    };

    // Collect text/html/MIME payloads once, keyed by NSPasteboard type.
    // Win32's IDataObject serves every format from one composite object;
    // mirror that by stamping these shared payloads onto *every*
    // NSPasteboardItem we emit, so each NSDraggingItem carries the full
    // context regardless of which one the receiver reads.
    NSMutableDictionary<NSPasteboardType, id>* shared_payloads =
      [NSMutableDictionary dictionary];

    // text/uri-list: emit one NSDraggingItem per URL.
    std::vector<NSURL*> uri_urls;

    item.for_each_format([&](const std::string& mime,
                              const std::vector<uint8_t>& bytes) {
      if (mime == "text/plain;charset=utf-8" || mime == "text/plain") {
        uint32_t n = static_cast<uint32_t>(bytes.size());
        if (n > 0 && bytes[n - 1] == 0) --n;
        NSString* s = [[NSString alloc] initWithBytes:bytes.data()
                                                length:n
                                              encoding:NSUTF8StringEncoding];
        if (s) shared_payloads[NSPasteboardTypeString] = s;
        return;
      }
      if (mime == "text/html") {
        NSData* d = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        if (d) shared_payloads[NSPasteboardTypeHTML] = d;
        return;
      }
      if (mime == "text/uri-list") {
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
      // Arbitrary MIME passthrough.
      NSString* t = [NSString stringWithUTF8String:mime.c_str()];
      if (t) {
        NSData* d = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        if (d) shared_payloads[t] = d;
      }
    });

    auto stamp_shared = [&](NSPasteboardItem* pbitem) {
      for (NSPasteboardType type in shared_payloads) {
        id val = shared_payloads[type];
        if ([val isKindOfClass:[NSString class]])
          [pbitem setString:(NSString*)val forType:type];
        else
          [pbitem setData:(NSData*)val forType:type];
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

  // Entry point used by platform_macos.mm and hosts/macos/widgets.mm.
  inline uint32_t macos_run_drag_source(NSView* anchor_view,
                                         const DataItem& item,
                                         uint32_t allowed_actions)
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

    NSArray<NSDraggingItem*>* items = build_dragging_items(item, mouse_view);
    if ([items count] == 0) return 0;

    NEUIDragSource* src = [[NEUIDragSource alloc] init];
    src->allowedOps = dnd_action_to_nsop(allowed_actions);

    NSEvent* evt = synthesize_drag_trigger(anchor_view, win, mouse_view);
    NSDraggingSession* session =
      [anchor_view beginDraggingSessionWithItems:items event:evt source:src];
    if (!session) return 0;

    dnd_pump_until(src);
    return nsop_to_dnd_action(src->finalOp);
  }
}

#endif // __APPLE__

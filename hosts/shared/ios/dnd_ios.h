#pragma once

#if defined(__APPLE__)
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <UIKit/UIKit.h>
#import <MobileCoreServices/MobileCoreServices.h>  // kUTType* fallbacks

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../clipboard_item.h"
#include "clipboard_ios.h"  // uti_for_mime_ios (MIME -> UTI)

// iOS drag&drop helpers shared by the native (neui.host.ios) and the
// crossplatform iOS host. The drop-target / drag-source plumbing differs from
// macOS (UIDragInteraction / UIDropInteraction + NSItemProvider instead of
// NSDragging + NSPasteboard), but the neui-side machinery is identical: the
// shared dnd_dispatch state machine, the DataItem payload primitive, and the
// MIME<->UTI mapping already used by clipboard_ios.h. This header only adds the
// UIKit<->neui glue:
//
//   - dnd_uti_to_mime_ios          : reverse of uti_for_mime_ios (UTI -> MIME).
//   - dnd_collect_mimes_ios        : the MIME list advertised by a drop session.
//   - dnd_read_drop_item_ios       : synchronously load every NSItemProvider's
//                                    bytes into a DataItem (for performDrop).
//   - dnd_item_provider_for_item   : build an NSItemProvider that vends each of
//                                    a DataItem's formats (for the drag source).
//
// The MIME<->UTI passthrough mirrors clipboard_ios.h:
//   public.utf8-plain-text / public.text / public.plain-text -> text/plain
//   public.html                                              -> text/html
//   public.url / public.file-url                             -> text/uri-list
//   public.png                                               -> image/png
//   anything else containing '/'                             -> opaque MIME

namespace neui_detail
{
  // UTI -> MIME. The inverse of uti_for_mime_ios, with a couple of extra
  // aliases UIKit hands us for system drags (e.g. Safari URLs arrive as
  // public.url, Notes text as public.plain-text). Returns empty for UTIs that
  // don't look like a MIME we can carry (e.g. dynamic com.apple.* types we
  // never registered) so the caller can skip them.
  inline std::string dnd_uti_to_mime_ios(NSString* uti)
  {
    if (!uti) return std::string();
    const char* c = [uti UTF8String];
    if (!c) return std::string();
    std::string u(c);

    if (u == "public.utf8-plain-text" || u == "public.text" ||
        u == "public.plain-text")
      return "text/plain;charset=utf-8";
    if (u == "public.html")  return "text/html";
    if (u == "public.url" || u == "public.file-url")
      return "text/uri-list";
    if (u == "public.png")   return "image/png";
    // Opaque passthrough: our own neui<->neui MIMEs are registered under the
    // literal MIME string (see uti_for_mime_ios), so a '/' identifies them.
    if (u.find('/') != std::string::npos) return u;
    return std::string();
  }

  // Collect the MIME list advertised across all items of a drag/drop session.
  // `item_providers` is the array of NSItemProvider* (UIDropSession.items map to
  // .itemProvider; a UIDragSession likewise). De-duplicated, dispatch-ready
  // (keep the struct alive for the dispatch call - `ptrs` points into
  // `strings`, matching the macOS PbMimeList shape Session::dispatch_dnd_*
  // expects).
  struct IosMimeList {
    std::vector<std::string> strings;
    std::vector<const char*> ptrs;
  };

  inline IosMimeList dnd_collect_mimes_ios(NSArray<NSItemProvider*>* providers)
  {
    IosMimeList ml;
    auto note = [&](const std::string& mime) {
      if (mime.empty()) return;
      for (auto& s : ml.strings) if (s == mime) return;
      ml.strings.push_back(mime);
    };
    for (NSItemProvider* p in providers) {
      for (NSString* uti in p.registeredTypeIdentifiers)
        note(dnd_uti_to_mime_ios(uti));
    }
    ml.ptrs.reserve(ml.strings.size());
    for (auto& s : ml.strings) ml.ptrs.push_back(s.c_str());
    return ml;
  }

  // True if any item in the session carries a type we can map to a MIME (i.e.
  // the drop interaction should claim the session at all). Cheap: checks
  // registeredTypeIdentifiers only, no byte load.
  inline bool dnd_session_has_known_type_ios(NSArray<NSItemProvider*>* providers)
  {
    for (NSItemProvider* p in providers) {
      for (NSString* uti in p.registeredTypeIdentifiers) {
        if (!dnd_uti_to_mime_ios(uti).empty()) return true;
      }
    }
    return false;
  }

  // Synchronously load one provider's representation for `uti` into `out`.
  // NSItemProvider loads are async; we block on a semaphore with a bounded
  // timeout. Safe to call on the main thread: the completion handler runs on a
  // private NSItemProvider queue, not the main queue, so the wait can't
  // deadlock. Returns true if bytes were produced. `timeout_s` guards a hung /
  // remote provider so performDrop can't wedge the UI.
  inline bool dnd_load_uti_bytes_ios(NSItemProvider* p, NSString* uti,
                                      std::vector<uint8_t>& out,
                                      double timeout_s = 3.0)
  {
    if (!p || !uti) return false;
    __block NSData* result = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [p loadDataRepresentationForTypeIdentifier:uti
                            completionHandler:^(NSData* data, NSError* /*err*/) {
      if (data) result = [data copy];
      dispatch_semaphore_signal(sem);
    }];
    dispatch_time_t deadline =
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout_s * NSEC_PER_SEC));
    if (dispatch_semaphore_wait(sem, deadline) != 0) return false;  // timed out
    if (!result || [result length] == 0) return false;
    out.assign((const uint8_t*)[result bytes],
               (const uint8_t*)[result bytes] + [result length]);
    return true;
  }

  // Load every mappable representation across all of a drop session's item
  // providers into `item`. text/plain is normalised to a NUL-terminated buffer
  // to match the DataItem convention the rest of the framework uses (mirrors
  // clipboard_read_item_ios). Returns true if at least one format was read.
  inline bool dnd_read_drop_item_ios(NSArray<NSItemProvider*>* providers,
                                      DataItem& item)
  {
    bool any = false;
    for (NSItemProvider* p in providers) {
      for (NSString* uti in p.registeredTypeIdentifiers) {
        std::string mime = dnd_uti_to_mime_ios(uti);
        if (mime.empty() || item.has_format(mime)) continue;  // first wins

        std::vector<uint8_t> bytes;
        if (!dnd_load_uti_bytes_ios(p, uti, bytes)) continue;

        if (mime == "text/plain;charset=utf-8") {
          // Ensure a trailing NUL so clients can treat it as a C string.
          if (bytes.empty() || bytes.back() != 0) bytes.push_back(0);
        }
        item.set_format(mime, bytes.data(),
                        static_cast<uint32_t>(bytes.size()));
        any = true;
      }
    }
    return any;
  }

  // Build an NSItemProvider that vends each of `item`'s formats under its UTI.
  // Used by the drag source: every UIDragItem wraps one of these. Bytes are
  // snapshotted up-front (we copy them into the block), so the source DataItem
  // may be released the moment the drag begins - matching the desktop
  // begin_drag snapshot contract. Returns nil if the item carries nothing.
  inline NSItemProvider* dnd_item_provider_for_item_ios(const DataItem& item)
  {
    NSItemProvider* provider = [[NSItemProvider alloc] init];
    bool any = false;

    // for_each_format invokes a synchronous C++ closure (not an Obj-C block),
    // so `any` is an ordinary by-reference capture - no __block needed.
    item.for_each_format([&](const std::string& mime,
                             const std::vector<uint8_t>& bytes) {
      NSString* uti = uti_for_mime_ios(mime);
      if (!uti || bytes.empty()) return;
      // Snapshot the bytes into the registration block. NSItemProvider keeps
      // the block until the receiver requests the data (deferred-render), so
      // the captured NSData must own a copy of the payload.
      NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
      [provider registerDataRepresentationForTypeIdentifier:uti
                                                visibility:NSItemProviderRepresentationVisibilityAll
                                               loadHandler:^NSProgress*(
          void (^completion)(NSData*, NSError*)) {
        completion(data, nil);
        return nil;
      }];
      any = true;
    });

    return any ? provider : nil;
  }

} // namespace neui_detail

#endif // TARGET_OS_IPHONE
#endif // __APPLE__

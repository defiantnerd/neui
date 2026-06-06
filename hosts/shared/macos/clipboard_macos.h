#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../clipboard_item.h"

// macOS system-clipboard helpers. Header-only / `inline` so the platform
// layer (and any future macOS-specific code that needs clipboard access)
// can include without ODR violations. Mirror of clipboard_win32.h.
//
// Same byte-count contract as clipboard_get_text_win32:
//   buf = NULL -> query bytes needed (incl. null terminator)
//   no clipboard text -> 0
//   error -> -1

namespace neui_detail
{
  // True if the clipboard currently advertises plain text.
  inline bool clipboard_has_text_macos()
  {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    return [pb availableTypeFromArray:@[NSPasteboardTypeString]] != nil;
  }

  // Replace pasteboard contents with UTF-8 text. Returns true on success.
  inline bool clipboard_set_text_macos(const char* utf8, uint32_t length)
  {
    if (!utf8) return false;
    NSString* s = [[NSString alloc] initWithBytes:utf8
                                            length:length
                                          encoding:NSUTF8StringEncoding];
    if (!s) return false;
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    return [pb setString:s forType:NSPasteboardTypeString] == YES;
  }

  // Read pasteboard text into buf as UTF-8.
  inline int clipboard_get_text_macos(char* buf, int buflen)
  {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    NSString* s = [pb stringForType:NSPasteboardTypeString];
    if (!s) return 0;

    // -[NSString UTF8String] returns an autoreleased C string owned by the
    // NSString. Snapshot the bytes immediately so we don't depend on its
    // lifetime past this call.
    const char* src = [s UTF8String];
    if (!src) return -1;

    int needed = static_cast<int>(strlen(src)) + 1;  // include null terminator
    if (buf && buflen > 0) {
      int n = (buflen < needed) ? buflen : needed;
      memcpy(buf, src, static_cast<size_t>(n));
      buf[n - 1] = '\0';
    }
    return needed;
  }

  // -------------------------------------------------------------------------
  // Multi-format item read / write.

  // Build text/uri-list bytes from a pasteboard's list of file URLs.
  inline std::vector<uint8_t> urilist_from_nsurls_macos(NSArray<NSURL*>* urls)
  {
    std::string text;
    for (NSURL* u in urls) {
      NSString* abs = [u absoluteString];
      if (!abs) continue;
      const char* c = [abs UTF8String];
      if (!c) continue;
      text.append(c);
      text.append("\r\n");
    }
    return std::vector<uint8_t>(text.begin(), text.end());
  }

  // Write a NSData payload onto the given pasteboard under the supplied
  // pasteboard type. Returns true on success.
  inline bool pb_set_data_macos(NSPasteboard* pb, NSString* type,
                                 const void* data, uint32_t length)
  {
    NSData* d = [NSData dataWithBytes:data length:length];
    return [pb setData:d forType:type] == YES;
  }

  // Place every format on `item` onto the system pasteboard. Known MIMEs
  // map to native pasteboard types; everything else passes through as
  // its MIME string (UTI passthrough).
  inline bool clipboard_write_item_macos(const DataItem& item)
  {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];

    bool any = false;
    item.for_each_format([&](const std::string& mime,
                              const std::vector<uint8_t>& bytes) {
      // ---- text/plain -> NSPasteboardTypeString ----
      if (mime == "text/plain;charset=utf-8" || mime == "text/plain") {
        uint32_t n = static_cast<uint32_t>(bytes.size());
        if (n > 0 && bytes[n - 1] == 0) --n;
        NSString* s = [[NSString alloc] initWithBytes:bytes.data()
                                                length:n
                                              encoding:NSUTF8StringEncoding];
        if (s && [pb setString:s forType:NSPasteboardTypeString]) any = true;
        return;
      }

      // ---- text/html -> NSPasteboardTypeHTML ----
      if (mime == "text/html") {
        if (pb_set_data_macos(pb, NSPasteboardTypeHTML,
                               bytes.data(),
                               static_cast<uint32_t>(bytes.size())))
          any = true;
        return;
      }

      // ---- text/uri-list -> NSPasteboardTypeFileURL per URI ----
      if (mime == "text/uri-list") {
        // Parse the RFC 2483 byte buffer into individual URIs, then write
        // each as an NSURL pasteboard item. NSPasteboard with multiple
        // file URLs uses one writer per URL via writeObjects:.
        NSMutableArray* urls = [NSMutableArray array];
        const char* p = reinterpret_cast<const char*>(bytes.data());
        size_t i = 0, length = bytes.size();
        while (i < length) {
          size_t end = i;
          while (end < length && p[end] != '\r' && p[end] != '\n') ++end;
          if (end > i) {
            NSString* line = [[NSString alloc]
              initWithBytes:p + i
                     length:end - i
                   encoding:NSUTF8StringEncoding];
            if (line && ![line hasPrefix:@"#"]) {
              NSURL* u = [NSURL URLWithString:line];
              if (u) [urls addObject:u];
            }
          }
          while (end < length && (p[end] == '\r' || p[end] == '\n')) ++end;
          i = end;
        }
        if ([urls count] > 0) {
          if ([pb writeObjects:urls]) any = true;
        }
        return;
      }

      // ---- arbitrary MIME -> UTI passthrough using MIME as the type ----
      NSString* t = [NSString stringWithUTF8String:mime.c_str()];
      if (t && pb_set_data_macos(pb, t,
                                   bytes.data(),
                                   static_cast<uint32_t>(bytes.size())))
        any = true;
    });

    return any;
  }

  // Snapshot every known representation on `pb` into `item`. Used by
  // both the clipboard reader (passes the general pasteboard) and the
  // DnD drop dispatcher (passes [sender draggingPasteboard]).
  inline bool pb_read_item_macos(NSPasteboard* pb, DataItem& item)
  {
    bool any = false;

    // ---- NSPasteboardTypeString -> text/plain ----
    if (NSString* s = [pb stringForType:NSPasteboardTypeString]) {
      const char* c = [s UTF8String];
      if (c) {
        uint32_t n = static_cast<uint32_t>(strlen(c)) + 1;  // include null
        item.set_format("text/plain;charset=utf-8", c, n);
        any = true;
      }
    }

    // ---- NSPasteboardTypeHTML -> text/html ----
    if (NSData* d = [pb dataForType:NSPasteboardTypeHTML]) {
      item.set_format("text/html",
                      [d bytes], static_cast<uint32_t>([d length]));
      any = true;
    }

    // ---- NSPasteboardTypeFileURL (per-URL) -> text/uri-list ----
    NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[[NSURL class]]
                                                options:nil];
    if (urls && [urls count] > 0) {
      // Only collect actual file URLs (NSURL also reads http: etc.).
      NSMutableArray<NSURL*>* fileUrls = [NSMutableArray array];
      for (NSURL* u in urls) if ([u isFileURL]) [fileUrls addObject:u];
      if ([fileUrls count] > 0) {
        auto bytes = urilist_from_nsurls_macos(fileUrls);
        if (!bytes.empty()) {
          item.set_format("text/uri-list", bytes.data(),
                          static_cast<uint32_t>(bytes.size()));
          any = true;
        }
      }
    }

    // ---- Other types whose name looks like a MIME (contains '/') ----
    NSArray<NSPasteboardType>* types = [pb types];
    for (NSPasteboardType t in types) {
      const char* c = [t UTF8String];
      if (!c) continue;
      std::string mime(c);
      if (mime.find('/') == std::string::npos) continue;
      if (item.has_format(mime)) continue;
      if (NSData* d = [pb dataForType:t]) {
        item.set_format(mime, [d bytes],
                        static_cast<uint32_t>([d length]));
        any = true;
      }
    }

    return any;
  }

  // List the MIME strings advertised on `pb` (deduped). Used by the DnD
  // dispatcher to drive client format_match decisions before pulling
  // bytes.
  inline std::vector<std::string> pb_collect_mimes_macos(NSPasteboard* pb)
  {
    std::vector<std::string> out;
    auto note = [&](const char* mime) {
      for (auto& s : out) if (s == mime) return;
      out.emplace_back(mime);
    };
    if ([pb availableTypeFromArray:@[NSPasteboardTypeString]])
      note("text/plain;charset=utf-8");
    if ([pb availableTypeFromArray:@[NSPasteboardTypeHTML]])
      note("text/html");
    NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[[NSURL class]]
                                                options:nil];
    if (urls) {
      for (NSURL* u in urls) {
        if ([u isFileURL]) { note("text/uri-list"); break; }
      }
    }
    for (NSPasteboardType t in [pb types]) {
      const char* c = [t UTF8String];
      if (!c) continue;
      std::string mime(c);
      if (mime.find('/') == std::string::npos) continue;
      note(c);
    }
    return out;
  }

  // MIME list + dispatch-ready const char* pointer view in one shot.
  // `ptrs` points into `strings`, so keep the struct alive for the
  // duration of the dispatch call. Shared by the NSDraggingDestination
  // methods of both macOS hosts, which all need the (data(), size())
  // shape for Session::dispatch_dnd_*.
  struct PbMimeList {
    std::vector<std::string>  strings;
    std::vector<const char*>  ptrs;
  };

  inline PbMimeList pb_collect_mime_list_macos(NSPasteboard* pb)
  {
    PbMimeList ml;
    ml.strings = pb_collect_mimes_macos(pb);
    ml.ptrs.reserve(ml.strings.size());
    for (auto& s : ml.strings) ml.ptrs.push_back(s.c_str());
    return ml;
  }

  inline bool clipboard_read_item_macos(DataItem& item)
  {
    return pb_read_item_macos([NSPasteboard generalPasteboard], item);
  }

} // namespace neui_detail

#endif // __APPLE__

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

// iOS system-clipboard helpers over UIPasteboard.generalPasteboard. Header-only
// / `inline`, same byte-count contract as clipboard_get_text_macos:
//   buf = NULL -> query bytes needed (incl. null terminator)
//   no clipboard text -> 0
//   error -> -1
//
// UIPasteboard, unlike NSPasteboard, has no rich per-type setData API at the
// top level; instead it carries an array of "items", each a dictionary keyed
// by UTI. We use a single item carrying all our representations. MIME<->UTI
// mapping mirrors the macOS pasteboard-UTI passthrough where reasonable:
//   text/plain;charset=utf-8 / text/plain -> "public.utf8-plain-text"
//   text/html                              -> "public.html"
//   text/uri-list                          -> "public.url" / NSURL items
//   image/png                              -> "public.png"
//   arbitrary MIME (contains '/')          -> the MIME string used as the UTI
//                                             (opaque passthrough, neui<->neui)

namespace neui_detail
{
  // Canonical UTIs for the built-in MIMEs. Use string constants rather than
  // UTType (iOS 14+) so the deployment target stays at 13; these public.*
  // identifiers are stable across OS versions.
  inline NSString* uti_for_mime_ios(const std::string& mime)
  {
    if (mime == "text/plain;charset=utf-8" || mime == "text/plain")
      return @"public.utf8-plain-text";
    if (mime == "text/html")     return @"public.html";
    if (mime == "text/uri-list") return @"public.url";
    if (mime == "image/png")     return @"public.png";
    // Opaque passthrough: the MIME string IS the type (matches macOS).
    return [NSString stringWithUTF8String:mime.c_str()];
  }

  // True if the clipboard currently advertises plain text.
  inline bool clipboard_has_text_ios()
  {
    return UIPasteboard.generalPasteboard.hasStrings;
  }

  // Replace pasteboard contents with UTF-8 text. Returns true on success.
  inline bool clipboard_set_text_ios(const char* utf8, uint32_t length)
  {
    if (!utf8) return false;
    NSString* s = [[NSString alloc] initWithBytes:utf8
                                            length:length
                                          encoding:NSUTF8StringEncoding];
    if (!s) return false;
    UIPasteboard.generalPasteboard.string = s;
    return true;
  }

  // Read pasteboard text into buf as UTF-8.
  inline int clipboard_get_text_ios(char* buf, int buflen)
  {
    NSString* s = UIPasteboard.generalPasteboard.string;
    if (!s) return 0;

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

  // Place every format on `item` onto the system pasteboard as a single
  // multi-representation item. Known MIMEs map to public.* UTIs; everything
  // else passes through under its MIME string. Returns true on success.
  inline bool clipboard_write_item_ios(const DataItem& item)
  {
    NSMutableDictionary<NSString*, id>* rep =
      [NSMutableDictionary dictionary];

    item.for_each_format([&](const std::string& mime,
                             const std::vector<uint8_t>& bytes) {
      NSString* uti = uti_for_mime_ios(mime);
      if (!uti) return;

      // text/plain -> NSString value so the system surfaces it as text.
      if (mime == "text/plain;charset=utf-8" || mime == "text/plain") {
        uint32_t n = static_cast<uint32_t>(bytes.size());
        if (n > 0 && bytes[n - 1] == 0) --n;  // drop trailing NUL
        NSString* s = [[NSString alloc] initWithBytes:bytes.data()
                                                length:n
                                              encoding:NSUTF8StringEncoding];
        if (s) rep[uti] = s;
        return;
      }

      // Everything else (html / uri-list / png / arbitrary) -> raw NSData.
      NSData* d = [NSData dataWithBytes:bytes.data() length:bytes.size()];
      if (d) rep[uti] = d;
    });

    if ([rep count] == 0) return false;
    UIPasteboard.generalPasteboard.items = @[ rep ];
    return true;
  }

  // Snapshot every known representation on the general pasteboard into `item`.
  inline bool clipboard_read_item_ios(DataItem& item)
  {
    UIPasteboard* pb = UIPasteboard.generalPasteboard;
    bool any = false;

    // ---- public.utf8-plain-text -> text/plain ----
    if (NSString* s = pb.string) {
      const char* c = [s UTF8String];
      if (c) {
        uint32_t n = static_cast<uint32_t>(strlen(c)) + 1;  // include null
        item.set_format("text/plain;charset=utf-8", c, n);
        any = true;
      }
    }

    // ---- public.html -> text/html ----
    if (NSData* d = [pb dataForPasteboardType:@"public.html"]) {
      item.set_format("text/html",
                      [d bytes], static_cast<uint32_t>([d length]));
      any = true;
    }

    // ---- URLs -> text/uri-list (RFC 2483, CRLF-joined) ----
    NSArray<NSURL*>* urls = pb.URLs;
    if (urls && [urls count] > 0) {
      std::string text;
      for (NSURL* u in urls) {
        NSString* abs = [u absoluteString];
        if (!abs) continue;
        const char* c = [abs UTF8String];
        if (!c) continue;
        text.append(c);
        text.append("\r\n");
      }
      if (!text.empty()) {
        item.set_format("text/uri-list", text.data(),
                        static_cast<uint32_t>(text.size()));
        any = true;
      }
    }

    // ---- public.png -> image/png ----
    if (!item.has_format("image/png")) {
      if (NSData* d = [pb dataForPasteboardType:@"public.png"]) {
        item.set_format("image/png",
                        [d bytes], static_cast<uint32_t>([d length]));
        any = true;
      }
    }

    // ---- Any remaining types whose name looks like a MIME (contains '/') ----
    for (NSString* t in pb.pasteboardTypes) {
      const char* c = [t UTF8String];
      if (!c) continue;
      std::string mime(c);
      if (mime.find('/') == std::string::npos) continue;
      if (item.has_format(mime)) continue;
      if (NSData* d = [pb dataForPasteboardType:t]) {
        item.set_format(mime, [d bytes],
                        static_cast<uint32_t>([d length]));
        any = true;
      }
    }

    return any;
  }

} // namespace neui_detail

#endif // TARGET_OS_IPHONE
#endif // __APPLE__

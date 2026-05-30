#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#include <cstdint>
#include <cstring>

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

} // namespace neui_detail

#endif // __APPLE__

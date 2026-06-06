#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

// Nested NSEvent pump used by blocking modal DIALOG show. Shared by both
// the xpl host (`hosts/crossplatform/platform_macos.mm::platform_run_modal_until`)
// and the native macOS host (`hosts/macos/window.mm::create_dialog`). The
// dispatch rules are identical between the two hosts (raw NSApp sendEvent:
// with no extra translation, unlike the win32 side), so the entire pump
// fits in this header.
//
// CONVENTION: include from `.mm` files only (this header imports AppKit).
//
// keep_running: caller-owned flag the destroy path clears so the pump
//   unwinds. Loops on [NSApp nextEventMatchingMask:] until the flag flips
//   to false. The runloop mode is NSDefaultRunLoopMode so timers and
//   sources still fire (panels, paint, etc.).

namespace neui_detail {

  inline void run_modal_pump_macos(bool* keep_running)
  {
    if (!keep_running) return;
    while (*keep_running) {
      @autoreleasepool {
        NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:[NSDate distantFuture]
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES];
        if (ev) [NSApp sendEvent:ev];
      }
    }
  }

} // namespace neui_detail

#endif // __APPLE__

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
//
//   "Caller-owned" is a REQUIREMENT, not a description: the flag is re-read
//   after every dispatched event, so it must outlive the pump independently of
//   any widget. Do NOT pass a pointer into a WidgetData - the events this pump
//   dispatches include the client callback that destroys the dialog, which is
//   the documented way out of a modal show, and that frees the slot while the
//   loop is still polling. Both hosts hold a std::shared_ptr<bool> and pass
//   .get() (xpl FrameWidget::modal_pump_flag, native WidgetData::modal_pump_flag);
//   an interior pointer was a real use-after-free in all four call sites.

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

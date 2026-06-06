#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

// Window geometry + style-mask helpers shared by the xpl macOS glue
// (hosts/crossplatform/platform_macos.mm) and the native macOS host
// (hosts/macos/window.mm). Both previously carried identical copies.

namespace neui_detail
{
  // neui logical coordinates: top-left origin, Y down. NSWindow frame uses
  // bottom-left screen origin, Y up. Convert against the main screen.
  inline NSRect logical_window_rect_macos(int x, int y, int w, int h)
  {
    CGFloat screen_h = NSScreen.mainScreen.frame.size.height;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    return NSMakeRect(x, screen_h - y - h, w, h);
  }

  inline NSWindowStyleMask styles_for_appwindow_macos()
  {
    return NSWindowStyleMaskTitled
         | NSWindowStyleMaskClosable
         | NSWindowStyleMaskMiniaturizable
         | NSWindowStyleMaskResizable;
  }

  // Titlebar + close button, no resize / minimize - mirrors WS_OVERLAPPED |
  // WS_CAPTION | WS_SYSMENU on win32.
  inline NSWindowStyleMask styles_for_dialog_macos()
  {
    return NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
  }
}

#endif // __APPLE__

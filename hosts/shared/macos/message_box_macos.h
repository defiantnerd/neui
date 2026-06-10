#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#include <cstdint>

#include "../../../include/neui/d/notify.h"

// Shared NSAlert-backed message box used by both the native macOS host
// and the xpl macOS platform layer. Mimics Win32 MessageBoxEx semantics:
// one NEUI_MB_* button set + icon + default button, blocking until the
// user picks, returning the NEUI_ID_* of the chosen button.
//
// Buttons are added in Win32 visual order (Yes No Cancel, ...). NSAlert
// lays first-added rightmost, which lands the affirmative on the right -
// the macOS idiom - while keeping the index -> NEUI_ID_* mapping trivial
// (NSAlertFirstButtonReturn + i = i-th button of the Win32 set). The
// default-button flag moves the return-key equivalent; NSAlert maps
// Escape to a button titled "Cancel" automatically.
//
// `caption` maps to messageText (the bold headline - NSAlert has no title
// bar), `text` to informativeText; with no caption the text becomes the
// headline. Button captions are literal English, matching what
// MessageBoxEx shows for the neutral language id on an English OS.
//
// ODR-safe: everything inline, no ObjC class definitions.

namespace neui_detail
{

  inline int message_box_macos(NSWindow* parent, const char* text,
                               const char* caption, uint32_t flags)
  {
    (void)parent;  // NSAlert runModal is app-modal; no owner wiring needed.

    // Button captions in Win32 visual order + their NEUI_ID_* returns.
    const char* titles[3] = { nullptr, nullptr, nullptr };
    int         ids[3]    = { 0, 0, 0 };
    int         count     = 0;
    switch (flags & 0x000Fu) {
      default:
      case NEUI_MB_OK:
        titles[0] = "OK"; ids[0] = NEUI_ID_OK; count = 1; break;
      case NEUI_MB_OKCANCEL:
        titles[0] = "OK";     ids[0] = NEUI_ID_OK;
        titles[1] = "Cancel"; ids[1] = NEUI_ID_CANCEL; count = 2; break;
      case NEUI_MB_ABORTRETRYIGNORE:
        titles[0] = "Abort";  ids[0] = NEUI_ID_ABORT;
        titles[1] = "Retry";  ids[1] = NEUI_ID_RETRY;
        titles[2] = "Ignore"; ids[2] = NEUI_ID_IGNORE; count = 3; break;
      case NEUI_MB_YESNOCANCEL:
        titles[0] = "Yes";    ids[0] = NEUI_ID_YES;
        titles[1] = "No";     ids[1] = NEUI_ID_NO;
        titles[2] = "Cancel"; ids[2] = NEUI_ID_CANCEL; count = 3; break;
      case NEUI_MB_YESNO:
        titles[0] = "Yes";    ids[0] = NEUI_ID_YES;
        titles[1] = "No";     ids[1] = NEUI_ID_NO; count = 2; break;
      case NEUI_MB_RETRYCANCEL:
        titles[0] = "Retry";  ids[0] = NEUI_ID_RETRY;
        titles[1] = "Cancel"; ids[1] = NEUI_ID_CANCEL; count = 2; break;
      case NEUI_MB_CANCELTRYCONTINUE:
        titles[0] = "Cancel";    ids[0] = NEUI_ID_CANCEL;
        titles[1] = "Try Again"; ids[1] = NEUI_ID_TRYAGAIN;
        titles[2] = "Continue";  ids[2] = NEUI_ID_CONTINUE; count = 3; break;
    }

    NSAlert* alert = [[NSAlert alloc] init];
    NSString* body = text ? [NSString stringWithUTF8String:text] : @"";
    if (caption && *caption) {
      [alert setMessageText:[NSString stringWithUTF8String:caption]];
      [alert setInformativeText:body];
    } else {
      [alert setMessageText:body];
    }

    switch (flags & 0x00F0u) {
      case NEUI_MB_ICONERROR:   [alert setAlertStyle:NSAlertStyleCritical];      break;
      case NEUI_MB_ICONWARNING: [alert setAlertStyle:NSAlertStyleWarning];       break;
      default:                  [alert setAlertStyle:NSAlertStyleInformational]; break;
    }

    for (int i = 0; i < count; ++i) {
      [alert addButtonWithTitle:[NSString stringWithUTF8String:titles[i]]];
    }

    // Default button: NSAlert gives the return-key equivalent to the
    // first-added button; move it when DEFBUTTON2/3 ask for another.
    int def = (int)((flags & 0x0F00u) >> 8);
    if (def > 0 && def < count) {
      [[alert buttons][0] setKeyEquivalent:@""];
      [[alert buttons][def] setKeyEquivalent:@"\r"];
    }

    NSModalResponse r = [alert runModal];
    int i = (int)(r - NSAlertFirstButtonReturn);
    if (i < 0 || i >= count) return 0;
    return ids[i];
  }

} // namespace neui_detail

#endif // __APPLE__

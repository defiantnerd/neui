#pragma once

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <UIKit/UIKit.h>
#include <cstdint>

#include "../../../include/neui/d/notify.h"

// Shared UIAlertController-backed message box for the iOS xpl platform layer
// (and, later, the native iOS host). Decodes the same NEUI_MB_* flag layout the
// macOS NSAlert helper (message_box_macos.h) decodes - one button set + icon +
// default button - and builds a UIAlertController with one UIAlertAction per
// button, in the same Win32 visual order.
//
// THE ASYNC DIVERGENCE (documented in notify.h + the plan's "Key divergences"):
//   UIAlertController is presented (presentViewController:animated:completion:)
//   and is inherently NON-BLOCKING - it cannot run modally and return the chosen
//   button id synchronously the way Win32 MessageBoxExW / macOS NSAlert runModal
//   do. iOS has no supported nested-runloop "spin until dismissed" primitive
//   (faking one is fragile and Apple-discouraged). So message_box on iOS:
//     - resolves the presenting view controller from the frame's UIWindow,
//     - presents the alert (buttons present + dismiss correctly on tap),
//     - returns IMMEDIATELY with a sentinel (NEUI_MB_IOS_PENDING below) rather
//       than the chosen NEUI_ID_*.
//   Delivering the chosen button back to the client (a completion callback /
//   event) is a deliberately deferred follow-up - this v1 only guarantees the
//   alert appears and dismisses. The UIAlertAction handlers are wired (so the
//   button-to-NEUI_ID_* mapping is ready for that future callback) but currently
//   just dismiss.
//
// ODR-safe: everything inline, no ObjC class definitions.

// Sentinel returned by the iOS message box, distinct from every NEUI_ID_* (all
// of which are small positive ints, 1..11) and from the 0 "failure" return, so a
// client can tell "presented, result pending" apart from "couldn't present".
#define NEUI_MB_IOS_PENDING (-1)

namespace neui_detail
{

  // Map the NEUI_MB_ICON* nibble + button set to UIAlertController content. iOS
  // alerts carry no icon well, so the icon class is surfaced as a leading glyph
  // on the title (mirroring the spirit of the macOS SF-Symbol icon) using the
  // same Unicode symbols across platforms. Returns nil for no icon bits.
  inline NSString* message_box_icon_prefix_ios(uint32_t flags)
  {
    switch (flags & 0x00F0u) {
      case NEUI_MB_ICONERROR:       return @"⛔ ";  // NO ENTRY
      case NEUI_MB_ICONQUESTION:    return @"❓ ";  // QUESTION MARK
      case NEUI_MB_ICONWARNING:     return @"⚠️ ";  // WARNING
      case NEUI_MB_ICONINFORMATION: return @"ℹ️ ";  // INFORMATION
      default:                      return nil;
    }
  }

  // Present a UIAlertController over `presenter`. Returns NEUI_MB_IOS_PENDING on
  // success (alert presented), 0 on failure (no presenter / nothing to present
  // on). See the async note above.
  inline int message_box_ios(UIViewController* presenter, const char* text,
                             const char* caption, uint32_t flags)
  {
    if (!presenter) return 0;

    // Button captions in Win32 visual order + their NEUI_ID_* returns - same
    // table as message_box_macos. The ids are retained for the future
    // result-callback path even though they are not returned synchronously.
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

    // caption -> alert title, text -> message (the UIKit idiom). With no caption
    // the body text becomes the title, matching the macOS no-caption shape.
    NSString* body    = text ? [NSString stringWithUTF8String:text] : @"";
    NSString* heading = (caption && *caption)
                          ? [NSString stringWithUTF8String:caption]
                          : body;
    if (NSString* prefix = message_box_icon_prefix_ios(flags))
      heading = [prefix stringByAppendingString:heading];

    UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:heading
                       message:((caption && *caption) ? body : nil)
                preferredStyle:UIAlertControllerStyleAlert];

    // Default button (DEFBUTTON1/2/3) -> the .default action style; the rest
    // are .cancel for "Cancel" / "No" / "Abort" and .default otherwise, so the
    // bold preferred button reads correctly. UIKit shows .cancel last + bold-less.
    int def = (int)((flags & 0x0F00u) >> 8);
    if (def < 0 || def >= count) def = 0;

    for (int i = 0; i < count; ++i) {
      const char* t  = titles[i];
      int         id_ = ids[i];
      UIAlertActionStyle style = UIAlertActionStyleDefault;
      if (id_ == NEUI_ID_CANCEL || id_ == NEUI_ID_NO || id_ == NEUI_ID_ABORT)
        style = UIAlertActionStyleCancel;
      UIAlertAction* act = [UIAlertAction
        actionWithTitle:[NSString stringWithUTF8String:t]
                  style:style
                handler:^(UIAlertAction* a) {
          (void)a;
          // Result-to-client delivery is deferred (see header note); the chosen
          // id_ is captured here ready for that future callback. For now the
          // tap simply dismisses (UIKit auto-dismisses an alert on any action).
          (void)id_;
        }];
      [alert addAction:act];
      if (i == def) {
        // preferredAction bolds the button + makes it the hardware-keyboard
        // default (iPad). Only one .cancel-styled action is allowed, and it
        // cannot also be preferred, so guard against bolding a .cancel button.
        if (@available(iOS 9.0, *)) {
          if (style != UIAlertActionStyleCancel) alert.preferredAction = act;
        }
      }
    }

    [presenter presentViewController:alert animated:YES completion:nil];
    return NEUI_MB_IOS_PENDING;
  }

} // namespace neui_detail

#endif // __APPLE__ && TARGET_OS_IPHONE

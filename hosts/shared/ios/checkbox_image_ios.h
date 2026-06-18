#pragma once

#if defined(__APPLE__)
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <UIKit/UIKit.h>

#include <neui/neui.h>

// SF-Symbol checkbox glyph for a neui check state - the iOS twin of
// hosts/macos/checkbox_image.h. The native iOS host renders CHECKBOX /
// CHECKBOX3 as a borderless UIButton carrying this image (leading) plus the
// widget's label text (trailing), and owns the state machine itself: UIButton
// does NOT auto-toggle (no UISwitch-style on/off), so the click handler reads
// the cached state, advances it by 2 (CHECKBOX) or 3 (CHECKBOX3), and swaps the
// image. This gives BOTH a visible label and a real three-state indeterminate
// glyph - neither of which a bare UISwitch can show.

namespace neui_detail
{
  inline UIImage* checkbox_image_for_state_ios(int neui_state)
  {
    NSString* name = @"square";
    if (neui_state == NEUI_CHECK_CHECKED)       name = @"checkmark.square.fill";
    if (neui_state == NEUI_CHECK_INDETERMINATE) name = @"minus.square.fill";

    if (@available(iOS 13.0, *)) {
      UIImage* img = [UIImage systemImageNamed:name];
      if (!img) return nil;
      // Tint: accent for checked / indeterminate, secondary for unchecked, so
      // the box reads as "set" vs "empty" the same way the macOS host does.
      UIColor* tint = (neui_state == NEUI_CHECK_UNCHECKED)
                        ? UIColor.secondaryLabelColor
                        : UIColor.systemBlueColor;
      return [img imageWithTintColor:tint renderingMode:UIImageRenderingModeAlwaysOriginal];
    }
    return nil;
  }
} // namespace neui_detail

#endif // TARGET_OS_IPHONE
#endif // __APPLE__

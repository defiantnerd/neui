#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#include <neui/neui.h>

namespace macos_host
{
  // SF Symbol image for a neui checkbox state. Used in place of the
  // +checkboxWithTitle: NSButton cell because, on macOS 26 / Sequoia,
  // setting NSControlStateValueMixed on a checkbox-style cell promotes it
  // to a bezeled pull-down with up/down chevrons - even when
  // allowsMixedState is NO. Driving an SF Symbol on a borderless NSButton
  // sidesteps the cell promotion entirely.
  inline NSImage* checkbox_image_for_state(int neui_state)
  {
    NSString* name = @"square";
    if (neui_state == NEUI_CHECK_CHECKED)       name = @"checkmark.square.fill";
    if (neui_state == NEUI_CHECK_INDETERMINATE) name = @"minus.square.fill";

    NSImage* img = [NSImage imageWithSystemSymbolName:name
                              accessibilityDescription:nil];
    if (!img) return nil;

    NSImageSymbolConfiguration* size =
      [NSImageSymbolConfiguration configurationWithPointSize:14.0
                                                        weight:NSFontWeightRegular
                                                         scale:NSImageSymbolScaleMedium];
    NSColor* tint = (neui_state == NEUI_CHECK_UNCHECKED)
                  ? [NSColor secondaryLabelColor]
                  : [NSColor controlAccentColor];
    NSImageSymbolConfiguration* color =
      [NSImageSymbolConfiguration configurationWithHierarchicalColor:tint];
    NSImageSymbolConfiguration* cfg =
      [size configurationByApplyingConfiguration:color];

    return [img imageWithSymbolConfiguration:cfg];
  }

} // namespace macos_host

#endif // __APPLE__

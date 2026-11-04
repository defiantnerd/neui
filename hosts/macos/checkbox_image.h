#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include "../shared/macos/symbol_image_macos.h"

namespace macos_host
{
  // SF Symbol image for a neui checkbox state. Used in place of the
  // +checkboxWithTitle: NSButton cell because, on macOS 26 / Sequoia,
  // setting NSControlStateValueMixed on a checkbox-style cell promotes it
  // to a bezeled pull-down with up/down chevrons - even when
  // allowsMixedState is NO. Driving an SF Symbol on a borderless NSButton
  // sidesteps the cell promotion entirely. SF Symbols need macOS 11: before
  // it there is no glyph (nil).
  inline NSImage* checkbox_image_for_state(int neui_state)
  {
    NSString* name = @"square";
    if (neui_state == NEUI_CHECK_CHECKED)       name = @"checkmark.square.fill";
    if (neui_state == NEUI_CHECK_INDETERMINATE) name = @"minus.square.fill";

    NSColor* tint = (neui_state == NEUI_CHECK_UNCHECKED)
                  ? [NSColor secondaryLabelColor]
                  : [NSColor controlAccentColor];
    if (@available(macOS 11.0, *))
      return neui_detail::tinted_symbol_image_macos(name, 14.0,
                                                    NSImageSymbolScaleMedium, tint);
    return nil;
  }

} // namespace macos_host

#endif // __APPLE__

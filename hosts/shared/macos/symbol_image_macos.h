#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

// A tinted SF Symbol, shared by the checkbox glyphs (hosts/macos/
// checkbox_image.h) and the message-box icons (message_box_macos.h).
//
// SF Symbols arrived in macOS 11, their hierarchical colour configuration in
// macOS 12. Built for an older deployment target the 12.0 calls compile to a
// selector that macOS 11 does not have - an "unrecognized selector" the first
// time a checkbox or an alert icon is drawn there. So: hierarchical colour
// where the OS has it; on macOS 11 the symbol (a template image) is drawn in
// the tint instead - one flat colour where macOS 12 shades the secondary
// layers, but the same glyph in the same hue. The caller guards macOS 11
// itself (`if (@available(macOS 11.0, *))`): before it there are no symbols.
//
// ODR-safe: everything inline, no ObjC class definitions.

namespace neui_detail
{

  API_AVAILABLE(macos(11.0))
  inline NSImage* tinted_symbol_image_macos(NSString* name, CGFloat point_size,
                                            NSImageSymbolScale scale,
                                            NSColor* tint)
  {
    NSImage* img = [NSImage imageWithSystemSymbolName:name
                              accessibilityDescription:nil];
    if (!img) return nil;

    NSImageSymbolConfiguration* size =
      [NSImageSymbolConfiguration configurationWithPointSize:point_size
                                                        weight:NSFontWeightRegular
                                                         scale:scale];
    if (@available(macOS 12.0, *)) {
      NSImageSymbolConfiguration* color =
        [NSImageSymbolConfiguration configurationWithHierarchicalColor:tint];
      return [img imageWithSymbolConfiguration:
                    [size configurationByApplyingConfiguration:color]];
    }

    // macOS 11: fill the template's shape with the tint. The handler runs at
    // draw time, so a dynamic colour (controlAccentColor, ...) resolves for
    // the appearance it is drawn in.
    NSImage* sized = [img imageWithSymbolConfiguration:size];
    if (!sized) return nil;
    return [NSImage imageWithSize:sized.size
                          flipped:NO
                   drawingHandler:^BOOL(NSRect rect) {
                     [sized drawInRect:rect];
                     [tint set];
                     NSRectFillUsingOperation(rect, NSCompositingOperationSourceAtop);
                     return YES;
                   }];
  }

} // namespace neui_detail

#endif // __APPLE__

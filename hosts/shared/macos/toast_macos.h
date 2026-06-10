#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <string>

// macOS toast overlay. Header-only helper used by both the native macOS
// host and the xpl macOS glue. Creates a borderless child NSWindow with
// rounded corners + soft shadow, animates fly-in (slide + fade) and
// fly-out (reverse), and self-destructs after the hold elapses.
//
// Per-host feel (macOS): ease-out slide + fade in, hold 3s, ease-in
// fade + small slide out. Matches the rough vibe of macOS Notification
// Center toasts (without trying to be pixel-identical - we don't depend
// on private APIs).
//
// Lifecycle:
//   neui_detail::toast_show_macos(parent_nswindow, "Hello\nWorld");
//
// The toast is owned by a strong reference attached to the parent window
// via an associated-object key; calling toast_show again replaces it.

// Per-toast strong reference + state. Held by the parent NSWindow via
// objc_setAssociatedObject so it lives until the parent dies OR until
// a replacement toast unhooks it. The @implementation body and the
// neui_detail::toast_show_macos entry point live behind
// NEUI_TOAST_MACOS_IMPLEMENTATION; exactly one TU defines that macro.

@interface NeuiToastMacOS : NSObject
@property (nonatomic, strong) NSWindow* panel;
@property (nonatomic, strong) NSTextField* label;
@property (nonatomic, strong) NSTimer* timer;
@property (nonatomic, assign) NSTimeInterval start_time;
@property (nonatomic, assign) NSTimeInterval fade_in;
@property (nonatomic, assign) NSTimeInterval hold;
@property (nonatomic, assign) NSTimeInterval fade_out;
@property (nonatomic, assign) CGFloat rest_y_above_top;   // toast top-edge offset in window-local "logical down" px
@property (nonatomic, assign) CGFloat width_px;
@property (nonatomic, assign) CGFloat height_px;
@property (nonatomic, weak)   NSWindow* parent_window;
- (void)stop;
- (void)tick:(NSTimer*)t;
// Jump to the start of the fade-out phase. Called from the panel's
// click handler so a click on the toast produces an immediate dismiss.
- (void)dismissNow;
@end

// Click sink for the toast panel. Lives inside the content view so a
// mouseDown on any part of the toast routes to the owning state object.
@interface NeuiToastClickView : NSView
@property (nonatomic, weak) NeuiToastMacOS* state;
@end

// Forward declaration of the entry point; the inline definition is gated
// by NEUI_TOAST_MACOS_IMPLEMENTATION so multiple TUs that include this
// header don't emit duplicate symbols.
namespace neui_detail {
  void toast_show_macos(NSWindow* parent, const char* utf8_text);
}

#ifdef NEUI_TOAST_MACOS_IMPLEMENTATION

// Stable key for objc_setAssociatedObject. One toast per parent NSWindow.
// Defined only in the impl TU so the address is unique process-wide.
static const void* kNeuiToastAssocKey = (const void*)"neui.toast";

@implementation NeuiToastClickView
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
// Claim every hit inside our bounds so the embedded NSTextField doesn't
// absorb mouseDown events meant for dismiss.
- (NSView*)hitTest:(NSPoint)point
{
  if ([self mouse:point inRect:self.bounds]) return self;
  return [super hitTest:point];
}
- (void)mouseDown:(NSEvent*)event
{
  (void)event;
  [self.state dismissNow];
}
@end

@implementation NeuiToastMacOS

- (void)dismissNow
{
  // Reproject start_time so elapsed lands at the start of the fade-out.
  NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
  NSTimeInterval hold_end = self.fade_in + self.hold;
  self.start_time = now - hold_end;
  [self tick:nil];
}

- (void)stop
{
  if (self.timer) { [self.timer invalidate]; self.timer = nil; }
  if (self.panel) {
    NSWindow* parent = self.parent_window;
    if (parent) [parent removeChildWindow:self.panel];
    [self.panel orderOut:nil];
    self.panel = nil;
  }
}

- (void)tick:(NSTimer*)t
{
  (void)t;
  if (!self.panel || !self.parent_window) { [self stop]; return; }
  NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
  NSTimeInterval elapsed = now - self.start_time;
  NSTimeInterval total   = self.fade_in + self.hold + self.fade_out;
  if (elapsed >= total) {
    // Detach the strong ref on the parent so we can be deallocated.
    NSWindow* p = self.parent_window;
    [self stop];
    if (p) objc_setAssociatedObject(p, kNeuiToastAssocKey, nil,
                                      OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return;
  }

  CGFloat alpha   = 1.0;
  CGFloat slide_t = 1.0;  // 0 = fully out of view above, 1 = rest position
  if (elapsed < self.fade_in) {
    CGFloat p  = (CGFloat)(elapsed / self.fade_in);
    CGFloat ip = 1.0 - p;
    CGFloat e  = 1.0 - ip * ip * ip;  // ease-out cubic
    alpha   = e;
    slide_t = e;
  } else if (elapsed < self.fade_in + self.hold) {
    alpha   = 1.0;
    slide_t = 1.0;
  } else {
    NSTimeInterval oe = elapsed - self.fade_in - self.hold;
    CGFloat p = (CGFloat)(oe / self.fade_out);
    CGFloat e = p * p * p;            // ease-in cubic
    alpha   = 1.0 - e;
    slide_t = 1.0 - e;
  }

  // Resting top in parent-window-local "screen-coord" terms:
  //   parent.contentLayoutRect spans the client area in screen Y-up;
  //   pull its top edge and place toast top one rest_y_above_top below.
  NSRect parent_rect    = [self.parent_window frame];
  NSRect content_rect   = [self.parent_window contentLayoutRect];
  CGFloat parent_top_y  = parent_rect.origin.y + content_rect.origin.y +
                            content_rect.size.height;
  CGFloat target_top_y  = parent_top_y - self.rest_y_above_top;
  CGFloat start_top_y   = parent_top_y + self.height_px;  // out of view above
  CGFloat top_y         = start_top_y + (target_top_y - start_top_y) * slide_t;

  CGFloat center_x      = parent_rect.origin.x +
                          (parent_rect.size.width - self.width_px) * 0.5;
  NSRect frame = NSMakeRect(center_x, top_y - self.height_px,
                              self.width_px, self.height_px);
  [self.panel setFrame:frame display:NO];
  [self.panel setAlphaValue:alpha];
}

@end

namespace neui_detail
{

  inline NSString* toast_split_to_nsstring_macos(const char* utf8)
  {
    if (!utf8) return @"";
    return [NSString stringWithUTF8String:utf8];
  }

  inline void toast_show_macos(NSWindow* parent, const char* utf8_text)
  {
    if (!parent) return;

    // Replace any in-flight toast attached to this parent.
    NeuiToastMacOS* prev = (NeuiToastMacOS*)objc_getAssociatedObject(
      parent, kNeuiToastAssocKey);
    if (prev) {
      [prev stop];
      objc_setAssociatedObject(parent, kNeuiToastAssocKey, nil,
                                OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }

    NSString* text = toast_split_to_nsstring_macos(utf8_text);

    // Build the label first so we can ask AppKit for its preferred size.
    NSTextField* lbl = [[NSTextField alloc] initWithFrame:NSZeroRect];
    [lbl setEditable:NO];
    [lbl setBezeled:NO];
    [lbl setDrawsBackground:NO];
    [lbl setSelectable:NO];
    [lbl setStringValue:text];
    [lbl setTextColor:[NSColor labelColor]];
    [lbl setFont:[NSFont systemFontOfSize:14.0]];
    [lbl setAlignment:NSTextAlignmentLeft];
    [lbl setLineBreakMode:NSLineBreakByWordWrapping];
    [[lbl cell] setWraps:YES];
    [[lbl cell] setScrollable:NO];
    [lbl setUsesSingleLineMode:NO];

    // Width clamp = 70% of parent client.
    NSRect parent_content = [parent contentLayoutRect];
    CGFloat max_w = parent_content.size.width * 0.7;
    if (max_w < 120) max_w = 120;

    NSSize fit = [[lbl cell] cellSizeForBounds:
                  NSMakeRect(0, 0, max_w - 36, CGFLOAT_MAX)];
    CGFloat pad_x = 18.0;
    CGFloat pad_y = 12.0;
    CGFloat w = fit.width + 2 * pad_x;
    CGFloat h = fit.height + 2 * pad_y;
    if (w > max_w) w = max_w;
    if (w < 120)   w = 120;
    if (h < 40)    h = 40;

    [lbl setFrame:NSMakeRect(pad_x, pad_y, w - 2 * pad_x, h - 2 * pad_y)];

    // Build the borderless child window.
    NSWindow* panel = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                                    styleMask:NSWindowStyleMaskBorderless
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
    [panel setOpaque:NO];
    [panel setBackgroundColor:[NSColor clearColor]];
    [panel setHasShadow:YES];
    [panel setLevel:NSFloatingWindowLevel];
    // Toast absorbs its own clicks so a click on it can trigger the
    // fade-out dismiss path via NeuiToastClickView.mouseDown:.
    [panel setIgnoresMouseEvents:NO];
    [panel setAlphaValue:0.0];

    NeuiToastClickView* root = [[NeuiToastClickView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
    [root setWantsLayer:YES];
    root.layer.cornerRadius  = 10.0;
    root.layer.masksToBounds = YES;
    // Honour system appearance: pull the dynamic control-background NSColor
    // and hand its CGColor to the layer. CALayer retains the CGColor itself.
    root.layer.backgroundColor = [[NSColor controlBackgroundColor] CGColor];
    [root addSubview:lbl];
    [panel setContentView:root];

    [parent addChildWindow:panel ordered:NSWindowAbove];

    // Animation state.
    NeuiToastMacOS* state = [[NeuiToastMacOS alloc] init];
    state.panel        = panel;
    state.label        = lbl;
    state.parent_window = parent;
    state.fade_in      = 0.22;
    state.hold         = 3.0;
    state.fade_out     = 0.35;
    state.width_px     = w;
    state.height_px    = h;
    // Rest position: top edge one line below parent's client-area top
    // (~22 px for default font - close enough for a single value).
    state.rest_y_above_top = 22.0;
    state.start_time   = [NSDate timeIntervalSinceReferenceDate];
    // Hook the click view to the state so mouseDown: can dismiss.
    root.state = state;
    state.timer = [NSTimer timerWithTimeInterval:1.0/60.0
                                            target:state
                                          selector:@selector(tick:)
                                          userInfo:nil
                                           repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:state.timer
                                  forMode:NSRunLoopCommonModes];
    // Strong-retain on the parent so the toast lives at least as long as
    // the parent window. Cleared by `tick:` when the lifetime expires.
    objc_setAssociatedObject(parent, kNeuiToastAssocKey, state,
                              OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    // First paint immediately so the toast appears before the 16 ms
    // tick lands.
    [state tick:nil];
  }

} // namespace neui_detail

#endif // NEUI_TOAST_MACOS_IMPLEMENTATION

#endif // __APPLE__

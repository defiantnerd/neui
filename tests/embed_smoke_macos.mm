// macOS acceptance harness for the DAW-embedding path (NEUI_API_EMBED).
//
// Plays the role of a DAW: creates a host NSWindow with a plain NSView as the
// "plugin slot", embeds a neui PLUGWINDOW (xpl host) into it via the public
// embed API, spins the main runloop briefly, then asserts the frame rooted as
// a subview, painted non-background pixels, and detaches cleanly on destroy.
// Uses cacheDisplayInRect: so the paint assertion works without a key window.
// Built but not ctest-registered (needs a GUI session); run
// ./tests/Debug/neui_embed_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdio>
#include <cstring>

// The "plugin slot" the fake DAW hands us. It paints its reference grey in
// drawRect: rather than via a CALayer background, because the paint check
// below renders the hierarchy with cacheDisplayInRect:, which composites
// drawRect: output but NOT layer background colours - with a layer-backed
// slot every sampled pixel reads as non-grey and the assertion is vacuous.
@interface NEUIFakeSlotView : NSView
@end

@implementation NEUIFakeSlotView
- (void)drawRect:(NSRect)dirtyRect
{
  [[NSColor colorWithSRGBRed:0.125 green:0.125 blue:0.125 alpha:1.0] set];
  NSRectFill(dirtyRect);
}
@end

static bool onevent(void*, neui_event_t*) { return false; }
static neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
static void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
static neui_client_t g_client = { NEUI_VERSION, iface };

int main()
{
  @autoreleasepool {
    [NSApplication sharedApplication];

    // Fake DAW: a titled window whose content view hosts a grey "plugin slot".
    NSWindow* host = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(120, 120, 400, 260)
                styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                  backing:NSBackingStoreBuffered
                    defer:NO];
    host.title = @"fake daw host";
    NSView* slot = [[NEUIFakeSlotView alloc] initWithFrame:NSMakeRect(40, 30, 320, 200)];
    [host.contentView addSubview:slot];
    [host makeKeyAndOrderFront:nil];

    // Build a neui PLUGWINDOW + a child button via the public API. The xpl
    // host is the plugin path - select it explicitly.
    neui_init();
    neui_api_t* api = neui_get_api("neui.host.crossplatform");
    if (!api) { std::printf("FAIL: no crossplatform host\n"); return 1; }
    neui_session_t sess = api->create_session(&g_client, nullptr);
    auto* w = (neui_widget_api_t*)api->get_interface(sess, NEUI_API_WIDGETS);
    auto* embed = (neui_embed_api_t*)api->get_interface(sess, NEUI_API_EMBED);
    if (!embed) { std::printf("FAIL: no NEUI_API_EMBED\n"); return 1; }

    neui_widget_t plug = w->create(sess, widget_none, NEUI_W_PLUGWINDOW,
                                   0, 0, 320, 200, nullptr);
    neui_widget_t btn = w->create(sess, plug, NEUI_W_BUTTON, 20, 20, 160, 32, nullptr);
    w->set_text(sess, btn, "Embedded!");

    int fail = 0;

    // Counts pixels in the slot that are NOT its reference grey - i.e. that
    // can only have come from an embedded neui frame. Sampled every other
    // pixel; the slot paints itself in drawRect: so cacheDisplayInRect:
    // captures it (see NEUIFakeSlotView).
    auto count_foreign_pixels = [&]() -> long {
      long n = 0;
      NSBitmapImageRep* rep =
        [slot bitmapImageRepForCachingDisplayInRect:slot.bounds];
      [slot cacheDisplayInRect:slot.bounds toBitmapImageRep:rep];
      for (NSInteger y = 0; y < rep.pixelsHigh; y += 2) {
        for (NSInteger x = 0; x < rep.pixelsWide; x += 2) {
          NSColor* c = [rep colorAtX:x y:y];
          if (!c) continue;
          CGFloat r = 0, g = 0, b = 0, a = 0;
          [[c colorUsingColorSpace:NSColorSpace.sRGBColorSpace]
              getRed:&r green:&g blue:&b alpha:&a];
          if (fabs(r - 0.125) > 0.06 || fabs(g - 0.125) > 0.06 ||
              fabs(b - 0.125) > 0.06)
            ++n;
        }
      }
      return n;
    };

    // Negative control: with nothing embedded the slot must read as pure
    // reference grey. Without this the paint assertion below can't fail -
    // it would pass just as happily against a capture that never contained
    // the frame at all.
    [[NSRunLoop mainRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.10]];
    long baseline = count_foreign_pixels();
    std::printf("baseline foreign pixels = %ld\n", baseline);
    if (baseline > 100) {
      std::printf("FAIL: slot is not a clean reference surface "
                  "(paint check would be vacuous)\n");
      ++fail;
    }

    // The DAW-provided parent travels as a void* (NSView* on macOS).
    if (!embed->set_parent(sess, plug, (__bridge void*)slot)) {
      std::printf("FAIL: set_parent rejected\n");
      ++fail;
    }
    // Guard: a second set_parent after show must be rejected (checked below).

    w->show(sess, plug);

    // event_fd is documented -1 on macOS (the DAW's runloop services the
    // view); pump_and_tick must be a callable no-op.
    int fd = embed->event_fd(sess, plug);
    std::printf("embed_event_fd = %d\n", fd);
    if (fd != -1) { std::printf("FAIL: expected fd -1\n"); ++fail; }
    if (!embed->pump_and_tick(sess, plug)) {
      std::printf("FAIL: pump_and_tick rejected a realized frame\n");
      ++fail;
    }
    if (embed->set_parent(sess, plug, (__bridge void*)slot)) {
      std::printf("FAIL: set_parent accepted a realized frame\n");
      ++fail;
    }

    // The frame must have rooted as a subview of the slot, visible.
    std::printf("slot subviews = %lu\n",
                (unsigned long)slot.subviews.count);
    if (slot.subviews.count != 1) {
      std::printf("FAIL: no embedded subview\n");
      ++fail;
    } else if (slot.subviews[0].hidden) {
      std::printf("FAIL: embedded subview still hidden\n");
      ++fail;
    }

    // Let the DAW's runloop deliver the initial display pass.
    [[NSRunLoop mainRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.25]];

    // Paint check: the embedded frame must now cover the slot's grey.
    long nonhost = count_foreign_pixels();
    std::printf("painted pixels = %ld (baseline %ld)\n", nonhost, baseline);
    if (nonhost < 500) { std::printf("FAIL: frame did not render\n"); ++fail; }

    // Destroy must detach the subview from the DAW's hierarchy.
    w->destroy(sess, plug);
    if (slot.subviews.count != 0) {
      std::printf("FAIL: subview leaked after destroy\n");
      ++fail;
    }

    [host close];
    std::printf(fail ? "\nEMBED FAILED (%d)\n" : "\nEMBED OK\n", fail);
    return fail ? 1 : 0;
  }
}

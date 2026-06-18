// NEUIApplication - the app-level menu-bar contribution point for the iPad
// hardware-keyboard system menu bar (and the press-and-hold-⌘ HUD).
//
// WHY THIS EXISTS (the bug it fixes): UIKit builds the MAIN menu from the
// responder chain by calling -buildMenuWithBuilder: on each responder. A
// UIVIEW's override is only consulted while that view is in the ACTIVE responder
// chain - which on a freshly-launched app with no first responder is unreliable,
// so the previous content-view contribution produced NOTHING on a real iPad (no
// menu bar in Stage Manager, no shortcuts HUD). UIApplication, by contrast, is
// ALWAYS in the responder chain, so overriding -buildMenuWithBuilder: HERE makes
// the contribution authoritative + deterministic.
//
// HOW A CLIENT OPTS IN: pass @"NEUIApplication" as the principalClassName (3rd
// arg) to UIApplicationMain so UIKit instantiates this subclass instead of the
// plain UIApplication. See examples/ios/main.m. A client that needs its own
// UIApplication subclass can either subclass NEUIApplication, or forward
// -buildMenuWithBuilder: / -handleMenuKeyCommand: to the neui hooks this file
// calls (neui_ios_native_build_menubar_menus / neui_ios_native_menu_key_command,
// and the parallel xpl pair). When no neui frame with a MENUBAR is frontmost -
// or the device isn't an iPad with a system menu bar - the hooks contribute
// nothing and the override is a no-op (the iPhone hamburger path is unaffected).
//
// HOST COVERAGE: the hooks are split per host so this one app class serves
// whichever host is linked. The native host (neui-ioshost) defines the
// neui_ios_native_* pair (window.mm); the crossplatform host (platform_ios.mm)
// defines the neui_ios_xpl_* pair. Both are declared weak here so the unlinked
// host's symbols resolve to nullptr at runtime rather than failing to link.

#import <UIKit/UIKit.h>

// Per-host contribution hooks. Weak so an app linking only one host still links
// (the other resolves to a null function pointer we guard before calling).
extern "C" unsigned long neui_ios_native_build_menubar_menus(
    id<UIMenuBuilder> builder, id key_cmd_target, SEL key_cmd_sel)
    __attribute__((weak_import));
extern "C" bool neui_ios_native_menu_key_command(UIKeyCommand* cmd)
    __attribute__((weak_import));
extern "C" unsigned long neui_ios_xpl_build_menubar_menus(
    id<UIMenuBuilder> builder, id key_cmd_target, SEL key_cmd_sel)
    __attribute__((weak_import));
extern "C" bool neui_ios_xpl_menu_key_command(UIKeyCommand* cmd)
    __attribute__((weak_import));

@interface NEUIApplication : UIApplication
@end

@implementation NEUIApplication

- (void)buildMenuWithBuilder:(id<UIMenuBuilder>)builder
{
  [super buildMenuWithBuilder:builder];
  if (@available(iOS 13.0, *)) {
    // Contribute the frontmost neui frame's menubar to the system menu bar. Each
    // host's hook gates internally on menu_ios_system_menubar_available() + a
    // frontmost MENUBAR-bearing frame, so calling both is safe regardless of
    // which is linked (the unlinked one's weak symbol is null). Shortcut leaves
    // become UIKeyCommands targeting self/-handleMenuKeyCommand: so the bar shows
    // + fires the equivalent.
    if (neui_ios_native_build_menubar_menus)
      neui_ios_native_build_menubar_menus(builder, self, @selector(handleMenuKeyCommand:));
    if (neui_ios_xpl_build_menubar_menus)
      neui_ios_xpl_build_menubar_menus(builder, self, @selector(handleMenuKeyCommand:));
  }
}

// Hardware-keyboard accelerator target for the UIKeyCommand-bearing menu leaves
// the build hook installs. Route through each host's accel dispatcher (built-in
// command first, else TREE_ITEM_ACTIVATED). First host to match wins.
- (void)handleMenuKeyCommand:(UIKeyCommand*)cmd
{
  if (neui_ios_native_menu_key_command && neui_ios_native_menu_key_command(cmd)) return;
  if (neui_ios_xpl_menu_key_command)    neui_ios_xpl_menu_key_command(cmd);
}

@end

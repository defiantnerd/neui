#pragma once

#if defined(__APPLE__)
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <UIKit/UIKit.h>

#include <cmath>   // std::fabs for the full-screen bounds comparison
#include <functional>
#include <string>
#include <vector>

#include "../../../include/neui/d/keys.h"

// iOS menu builder. There is no AppKit-style global menu bar on iPhone, so the
// xpl host surfaces a frame's MENUBAR child as a native UIMenu opened from a
// hamburger UIButton in the frame's top safe-area inset (the Linux-style
// host-drawn cascading-dropdown band - Session::paint_menubar - is gated off on
// iOS, so none of that code runs).
//
// This header is the UIKit twin of the macOS NSMenu builder in
// platform_macos.mm + hosts/shared/macos/menubar_macos.h. It owns:
//   - menu_ios_build_uimenu: recursively builds a UIMenu/UIAction tree from the
//     platform-neutral MenubarWidget model (popups -> submenu UIMenus, items ->
//     UIAction carrying the cmd_id, separators -> inline UIMenu sections,
//     shortcut text shown where available). Parameterised on the MenubarWidget
//     type so it compiles inside the platform_ios.mm TU (which has host.h)
//     without dragging host.h into shared code.
//   - menu_ios_build_top_level_menus: returns the array of top-level popup
//     UIMenus (File / Edit / View / ...) the system-menu-bar contribution
//     (-[UIResponder buildMenuWithBuilder:]) inserts next to the standard app
//     menus, and which menu_ios_build_uimenu wraps under a root for the
//     hamburger UIButton. One model -> both surfaces.
//   - menu_ios_system_menubar_available: runtime detection of whether the OS
//     provides a navigable system menu bar - iPadOS 26+ added a real,
//     touch-reachable menu bar (swipe-from-top) for the iPad idiom, surfaced
//     via -[UIResponder buildMenuWithBuilder:]. When true the content view
//     contributes the menubar tree to that system menu bar (top of screen /
//     menu bar) and the in-frame hamburger is hidden; when false (iPhone any
//     version, iPad < 26 - no menu bar) the hamburger is the only path. The
//     hamburger build itself always works regardless of this verdict.
//
// CONVENTION (matches theme_provider_ios.h / clipboard_ios.h): include from
// exactly one translation unit (hosts/crossplatform/platform_ios.mm). The
// functions are inline / templates so repeated includes within that single TU
// are harmless.

namespace neui_detail
{
  // -------------------------------------------------------------------------
  // Shortcut formatting (NEUI_KEY_* / NEUI_KMOD_* -> UIKey / UIKeyModifierFlags).
  // Used both for the UIKeyCommand-bearing UIMenu items (so a hardware keyboard
  // can trigger them) and for the inline shortcut label.

  inline UIKeyModifierFlags menu_ios_mods_to_uikit(uint32_t mods)
  {
    UIKeyModifierFlags f = 0;
    // NEUI_KMOD_CTRL is the platform-primary (Command on Apple platforms);
    // NEUI_KMOD_META is the secondary (Control). Mirrors macos_neui_mods_to_appkit.
    if (mods & NEUI_KMOD_SHIFT) f |= UIKeyModifierShift;
    if (mods & NEUI_KMOD_CTRL)  f |= UIKeyModifierCommand;
    if (mods & NEUI_KMOD_ALT)   f |= UIKeyModifierAlternate;
    if (mods & NEUI_KMOD_META)  f |= UIKeyModifierControl;
    return f;
  }

  // The "input" string a UIKeyCommand expects for a NEUI_KEY_* code, or nil if
  // the key has no usable UIKeyCommand input (so the item gets no key command).
  inline NSString* menu_ios_key_to_input(uint32_t key)
  {
    if (key >= NEUI_KEY_A && key <= NEUI_KEY_Z) {
      char c = (char)('a' + (key - NEUI_KEY_A));
      return [NSString stringWithFormat:@"%c", c];
    }
    if (key >= NEUI_KEY_0 && key <= NEUI_KEY_9) {
      char c = (char)('0' + (key - NEUI_KEY_0));
      return [NSString stringWithFormat:@"%c", c];
    }
    switch (key) {
      case NEUI_KEY_LEFT:   return UIKeyInputLeftArrow;
      case NEUI_KEY_RIGHT:  return UIKeyInputRightArrow;
      case NEUI_KEY_UP:     return UIKeyInputUpArrow;
      case NEUI_KEY_DOWN:   return UIKeyInputDownArrow;
      case NEUI_KEY_SPACE:  return @" ";
      default:              return nil;   // function / editing keys: no key cmd
    }
  }

  // -------------------------------------------------------------------------
  // Runtime system-menu-bar detection.
  //
  // HIG + iPadOS 26 reality: iPadOS 26 added a real, touch-reachable system menu
  // bar (swipe-from-top) for iPad apps - the surface
  // -[UIResponder buildMenuWithBuilder:] contributes to. iPadOS < 26 and iPhone
  // (any version) have NO navigable menu bar. So "the OS provides a navigable
  // system menu bar" is exactly: iPad idiom (UIUserInterfaceIdiomPad) AND
  // iPadOS >= 26 (iPadOS major == iOS major, so @available(iOS 26.0)). When true
  // we ALSO contribute the tree to the system menu bar and hide the hamburger;
  // when false (iPhone, or iPad < 26) the hamburger is the only path. The
  // hamburger build itself always works regardless of this verdict.
  inline bool menu_ios_system_menubar_available()
  {
    if (@available(iOS 26.0, *)) {
      return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
    }
    return false;
  }

  // Full-screen detection.
  //
  // WHY: the iPadOS 26 system menu bar (swipe-from-top) is only revealable in
  // WINDOWED / Stage-Manager mode - NOT in true full-screen, where there is no
  // top edge to swipe a menu bar down from. So "the menu bar is reachable" is
  // not just menu_ios_system_menubar_available() - it ALSO requires the window
  // to be windowed. A full-screen iPad-26 app has the menu-bar capability but no
  // way to reveal it, so it still needs the hamburger.
  //
  // HEURISTIC: a window is full-screen when its size matches the PHYSICAL display
  // size. We must NOT use screen.bounds: in Stage Manager that tracks the
  // (shrunken) workspace, so a windowed app's bounds ~= screen.bounds and every
  // window reads as full-screen (the bug). screen.nativeBounds is the physical
  // display in PIXELS, native (portrait) orientation, and is MODE-INDEPENDENT -
  // it stays the full panel in Stage Manager / Split View. Convert to points via
  // nativeScale and compare LONG/SHORT edges (orientation-agnostic, since
  // nativeBounds is portrait but the window may be landscape). A windowed app is
  // strictly smaller than the panel -> reads as windowed.
  // EDGE CASES (all acceptable - they only ever make the hamburger show
  // redundantly, never absent): a full-width Split View / maximized Stage-Manager
  // window may match the panel and read as full-screen; there the hamburger shows
  // even though the menu bar is technically reachable - a harmless extra
  // affordance. A nil window is treated as full-screen (safe default: show the
  // hamburger so the menu is never stranded).
  inline bool menu_ios_window_is_fullscreen(UIWindow* w)
  {
    if (!w) return true;   // unknown -> safe default (show hamburger)
    CGSize win = w.bounds.size;
    CGSize full;
    if (@available(iOS 13.0, *)) {
      UIScreen* screen = (w.windowScene != nil) ? w.windowScene.screen
                                                : UIScreen.mainScreen;
      CGFloat  ns      = screen.nativeScale > 0 ? screen.nativeScale : screen.scale;
      CGRect   nb      = screen.nativeBounds;   // physical pixels, mode-independent
      full = CGSizeMake(nb.size.width / ns, nb.size.height / ns);
    } else {
      full = UIScreen.mainScreen.bounds.size;
    }
    CGFloat winLong  = win.width  > win.height ? win.width  : win.height;
    CGFloat winShort = win.width  > win.height ? win.height : win.width;
    CGFloat fLong    = full.width > full.height ? full.width : full.height;
    CGFloat fShort   = full.width > full.height ? full.height : full.width;
    return (std::fabs(winLong  - fLong)  < 4.0) &&
           (std::fabs(winShort - fShort) < 4.0);
  }

  // The single hamburger-visibility rule, shared by both host hooks so native +
  // xpl stay in lock-step.
  //
  // DECISION: the hamburger ALWAYS shows when the frame carries a MENUBAR (so
  // this returns false). We tried to hide it on iPad-26 where the system menu
  // bar is reachable, but iPadOS 26's menu bar only reveals in WINDOWED / Stage-
  // Manager mode (not true full-screen), and there is no reliable public API to
  // distinguish windowed from full-screen: both screen.bounds AND
  // screen.nativeBounds track the (shrunken) Stage-Manager workspace, so every
  // window reads as full-screen and the hamburger never hides. Rather than ship
  // a detection that doesn't hold, the hamburger is the universal, always-
  // reachable menu on every device/mode; the system menu bar (when revealable in
  // windowed mode) coexists as an additive bonus - it's still contributed via
  // -buildMenuWithBuilder:. menu_ios_window_is_fullscreen() is kept (unused by
  // this rule) for reference / a future reliable signal. The `window` param is
  // accepted but ignored so callers don't churn.
  inline bool menu_ios_hamburger_should_hide(UIWindow* window)
  {
    (void)window;
    return false;
  }

  // -------------------------------------------------------------------------
  // Standard-menu merge mapping (iPad system menu bar).
  //
  // WHY: UIKit pre-populates the iPad menu bar with empty STANDARD menus
  // (File / Edit / View / Window / Help / Format, etc.) before any app
  // contribution runs. If neui INSERTS its own top-level File/Edit/View next to
  // them, the bar shows each title TWICE (the empty standard one + neui's
  // populated one). To avoid the duplicates we MERGE a neui popup whose title
  // matches a standard menu INTO that standard menu (filling the empty File,
  // adding our Edit items alongside the system Cut/Copy/Paste, ...) rather than
  // appending a second top-level menu.
  //
  // The mapping is a deliberately small, documented ENGLISH heuristic: we match
  // the popup's title case-insensitively (trimmed of surrounding whitespace)
  // against the well-known standard-menu names. A non-English or otherwise
  // non-matching title (e.g. a custom "Tools" menu) returns nil and is inserted
  // as a NEW top-level menu - no worse than the pre-merge behaviour, just no
  // duplicate of a standard menu. We intentionally only cover the standard menus
  // that UIKit pre-creates and that an app commonly re-titles; UIKit's other
  // defaults (Format / Window / Help) are left in place and only merged into
  // when a neui popup actually carries that name.
  //
  // Returns the UIMenuIdentifier constant to merge into, or nil for "no match,
  // insert as a new top-level menu". Shared by the native + xpl host hooks.
  inline UIMenuIdentifier menu_ios_standard_identifier_for_title(NSString* title)
  {
    if (@available(iOS 13.0, *)) {
      if (!title) return nil;
      NSString* t = [[title stringByTrimmingCharactersInSet:
                          [NSCharacterSet whitespaceAndNewlineCharacterSet]]
                        lowercaseString];
      if (t.length == 0) return nil;
      // English title -> standard UIKit menu identifier.
      if ([t isEqualToString:@"file"])   return UIMenuFile;
      if ([t isEqualToString:@"edit"])   return UIMenuEdit;
      if ([t isEqualToString:@"view"])   return UIMenuView;
      if ([t isEqualToString:@"window"]) return UIMenuWindow;
      if ([t isEqualToString:@"help"])   return UIMenuHelp;
      if ([t isEqualToString:@"format"]) return UIMenuFormat;
    }
    return nil;
  }

  // -------------------------------------------------------------------------
  // UIMenu builder. MenubarT is xpl_host::MenubarWidget (templated so this
  // header stays free of host.h). pick is invoked with the activated item's
  // cmd_id when the user selects a UIAction; the platform layer routes it
  // through Session::dispatch_menu_event.
  //
  // The MenubarWidget model: menu_items (neui_id -> MenuItemData), linked by
  // MenuItemData.parent_item_id (0 == top level), iterated in insertion order
  // via menu_item_ids_ordered. Leaf items carry a cmd_id; separators have
  // is_separator; an item with children is a submenu.

  template <class MenubarT>
  inline std::vector<uint32_t> menu_ios_children(const MenubarT& mb,
                                                 uint32_t parent_item_id)
  {
    std::vector<uint32_t> out;
    for (uint32_t id : mb.menu_item_ids_ordered) {
      auto it = mb.menu_items.find(id);
      if (it != mb.menu_items.end() && it->second.parent_item_id == parent_item_id)
        out.push_back(id);
    }
    return out;
  }

  template <class MenubarT>
  inline bool menu_ios_has_children(const MenubarT& mb, uint32_t item_id)
  {
    for (uint32_t id : mb.menu_item_ids_ordered) {
      auto it = mb.menu_items.find(id);
      if (it != mb.menu_items.end() && it->second.parent_item_id == item_id)
        return true;
    }
    return false;
  }

  // Build the UIMenu children for `parent_item_id`. Consecutive non-separator
  // items are gathered into an inline (display-inline) UIMenu so a separator
  // produces a visible divider, mirroring the desktop separator semantics.
  //
  // key_cmd_target selects how a leaf item with a bound shortcut is built:
  //   - nil  (hamburger path): plain UIAction routed via `pick`, with the
  //     shortcut shown as discoverabilityTitle only (UIButton menus can't show a
  //     trailing key-equivalent and the hamburger isn't a keyboard surface).
  //   - non-nil (system-menu-bar path): a UIKeyCommand targeting key_cmd_target
  //     via key_cmd_sel (handleMenuKeyCommand:) so the menu bar shows the
  //     trailing ⌘-equivalent AND fires the chord from a hardware keyboard. This
  //     is why -keyCommands yields the menubar accelerators when the system bar
  //     is active: the UIKeyCommand menu element is the single dispatch point.
  // Items WITHOUT a shortcut are always plain UIActions routed via `pick`
  // regardless of mode.
  template <class MenubarT>
  inline NSArray<UIMenuElement*>* menu_ios_build_elements(
      const MenubarT& mb, uint32_t parent_item_id,
      const std::function<void(uint32_t)>& pick,
      id key_cmd_target = nil, SEL key_cmd_sel = nullptr)
  {
    NSMutableArray<UIMenuElement*>* sections = [NSMutableArray array];
    NSMutableArray<UIMenuElement*>* run      = [NSMutableArray array];

    auto flush_run = [&]() {
      if (run.count == 0) return;
      // Wrap the accumulated run in an inline section so the surrounding
      // separators render as dividers between groups.
      UIMenu* sect = [UIMenu menuWithTitle:@""
                                     image:nil
                                identifier:nil
                                   options:UIMenuOptionsDisplayInline
                                  children:[run copy]];
      [sections addObject:sect];
      run = [NSMutableArray array];
    };

    for (uint32_t id : menu_ios_children(mb, parent_item_id)) {
      auto it = mb.menu_items.find(id);
      if (it == mb.menu_items.end()) continue;
      const auto& data = it->second;

      if (data.is_separator) {
        flush_run();
        continue;
      }

      NSString* title = [NSString stringWithUTF8String:data.text.c_str()];
      if (!title) title = @"";

      if (menu_ios_has_children(mb, id)) {
        // Submenu: recurse. A run before a submenu is flushed so the submenu
        // sits in its own (non-inline) group.
        NSArray<UIMenuElement*>* kids =
            menu_ios_build_elements(mb, id, pick, key_cmd_target, key_cmd_sel);
        UIMenu* sub = [UIMenu menuWithTitle:title children:kids];
        [run addObject:sub];
        continue;
      }

      uint32_t cmd_id = data.cmd_id;

      // System-menu-bar leaf WITH a bound shortcut -> UIKeyCommand. The menu bar
      // shows the trailing ⌘-equivalent and fires the chord from a hardware
      // keyboard; routing goes via the target's handleMenuKeyCommand: (which
      // re-derives the item from key+mods, the same single dispatch path the
      // M6 -keyCommands used). NEUI_KEY_NONE / unmappable keys fall through to a
      // plain UIAction so the item is still pickable by pointer.
      NSString* input = (key_cmd_target && data.shortcut_key != NEUI_KEY_NONE)
                          ? menu_ios_key_to_input(data.shortcut_key) : nil;
      if (input) {
        UIKeyModifierFlags flags = menu_ios_mods_to_uikit(data.shortcut_mods);
        UIKeyCommand* kc = [UIKeyCommand commandWithTitle:title
                                                    image:nil
                                                   action:key_cmd_sel
                                                    input:input
                                            modifierFlags:flags
                                             propertyList:nil];
        if (!data.enabled) kc.attributes = UIMenuElementAttributesDisabled;
        if (data.checked)  kc.state      = UIMenuElementStateOn;
        [run addObject:kc];
        continue;
      }

      // Leaf command -> UIAction carrying the cmd_id.
      // Capture an OWNED copy of `pick`, not the `const std::function&`: that
      // reference is bound to a temporary/local that is destroyed once the
      // build call returns, but the UIAction handler fires later (on tap). A
      // by-value capture makes the heap-copied block own a live callable;
      // capturing the reference invokes a destroyed std::function -> a
      // std::bad_function_call abort when the menu item is selected.
      std::function<void(uint32_t)> cb = pick;
      UIAction* action = [UIAction actionWithTitle:title
                                             image:nil
                                        identifier:nil
                                           handler:^(__kindof UIAction* _Nonnull a) {
        (void)a;
        cb(cmd_id);
      }];
      if (!data.enabled) action.attributes = UIMenuElementAttributesDisabled;
      if (data.checked)  action.state      = UIMenuElementStateOn;
      // Surface the shortcut text in the item subtitle where one is bound (the
      // cached display label, e.g. "Cmd+S"). discoverabilityTitle shows under
      // the title in the popover.
      if (!data.shortcut.empty()) {
        NSString* sc = [NSString stringWithUTF8String:data.shortcut.c_str()];
        if (sc) action.discoverabilityTitle = sc;
      }
      [run addObject:action];
    }

    flush_run();
    return [sections copy];
  }

  // Build the array of top-level popup UIMenus: each top-level popup
  // (parent_item_id == 0) becomes a submenu UIMenu; their children fill in
  // recursively. Shared by the hamburger UIMenu (wrapped under a root) and the
  // system-menu-bar contribution (-buildMenuWithBuilder:, where each is inserted
  // as its own top-level menu next to File/Edit/View).
  //
  // Each popup is given a STABLE identifier
  // ("neui.menubar.popup.<scope_id>.<id>") so a -buildMenuWithBuilder: pass that
  // runs after [UIMenuSystem setNeedsRebuild] replaces the previous contribution
  // cleanly (UIKit de-dups by identifier) and the insertion site stays
  // addressable. scope_id is the owning MENUBAR's globally-unique widget id
  // (session<<16 | slot): item ids restart at 1 per menubar, so without the
  // scope two frames in the same build pass (iPad Stage Manager / multiple
  // scenes) would both emit "neui.menubar.popup.1" and UIKit would drop one
  // frame's menus on the identifier collision.
  template <class MenubarT>
  inline NSArray<UIMenu*>* menu_ios_build_top_level_menus(
      const MenubarT& mb, uint32_t scope_id,
      const std::function<void(uint32_t)>& pick,
      id key_cmd_target = nil, SEL key_cmd_sel = nullptr)
  {
    NSMutableArray<UIMenu*>* top = [NSMutableArray array];
    for (uint32_t id : menu_ios_children(mb, 0)) {
      auto it = mb.menu_items.find(id);
      if (it == mb.menu_items.end()) continue;
      const auto& data = it->second;
      NSString* title = [NSString stringWithUTF8String:data.text.c_str()];
      if (!title) title = @"";
      NSArray<UIMenuElement*>* kids =
          menu_ios_build_elements(mb, id, pick, key_cmd_target, key_cmd_sel);
      NSString* ident =
          [NSString stringWithFormat:@"neui.menubar.popup.%u.%u", scope_id, id];
      UIMenu* popup = [UIMenu menuWithTitle:title
                                      image:nil
                                 identifier:ident
                                    options:0
                                   children:kids];
      [top addObject:popup];
    }
    return [top copy];
  }

  // Contribute the menubar tree to the iPad system menu bar, MERGING popups
  // whose title matches a standard UIKit menu into that menu and inserting the
  // rest as new top-level menus. This is the shared core of both host hooks
  // (-buildMenuWithBuilder:) so native + xpl behave identically.
  //
  // For each top-level popup (parent_item_id == 0):
  //   - title maps to a standard identifier (File/Edit/View/Window/Help/Format)
  //     -> build the popup's CHILDREN and wrap them in a single inline
  //        (DisplayInline) UIMenu group so they read as a section, then insert
  //        that group at the END of the matching standard menu. This fills the
  //        empty standard File with neui's File items and slots neui's Edit
  //        items in alongside the system Cut/Copy/Paste - no duplicate
  //        top-level menu.
  //   - title does NOT map -> insert the whole popup as a new top-level menu
  //     under UIMenuRoot (the original behaviour, for non-standard names only).
  //
  // The inline group / popup carries the SAME stable identifier scheme
  // menu_ios_build_top_level_menus uses ("neui.menubar.popup.<scope>.<id>") so a
  // rebuild pass replaces the previous contribution cleanly (UIKit de-dups by
  // identifier). Pick routing + the UIKeyCommand shortcut leaves are produced by
  // the shared menu_ios_build_elements, so accelerators work inside standard
  // menus too. Returns the merged-vs-top-level split via the out params (the
  // host hooks log it in the [neui-menu] line).
  template <class MenubarT>
  inline void menu_ios_contribute_menubar(
      id<UIMenuBuilder> builder, const MenubarT& mb, uint32_t scope_id,
      const std::function<void(uint32_t)>& pick,
      id key_cmd_target, SEL key_cmd_sel,
      unsigned long* out_merged, unsigned long* out_top_level)
  {
    unsigned long merged = 0, top = 0;
    if (@available(iOS 13.0, *)) {
      for (uint32_t id : menu_ios_children(mb, 0)) {
        auto it = mb.menu_items.find(id);
        if (it == mb.menu_items.end()) continue;
        const auto& data = it->second;
        NSString* title = [NSString stringWithUTF8String:data.text.c_str()];
        if (!title) title = @"";
        NSArray<UIMenuElement*>* kids =
            menu_ios_build_elements(mb, id, pick, key_cmd_target, key_cmd_sel);
        NSString* ident =
            [NSString stringWithFormat:@"neui.menubar.popup.%u.%u", scope_id, id];

        UIMenuIdentifier std_id =
            menu_ios_standard_identifier_for_title(title);
        if (std_id) {
          // Merge: an inline group of this popup's children into the standard
          // menu so they read as a section (and so a later rebuild can replace
          // it by identifier). Empty popups still merge harmlessly (no-op band).
          UIMenu* group = [UIMenu menuWithTitle:@""
                                          image:nil
                                     identifier:ident
                                        options:UIMenuOptionsDisplayInline
                                       children:kids];
          [builder insertChildMenu:group atEndOfMenuForIdentifier:std_id];
          ++merged;
        } else {
          // No standard match -> a brand-new top-level menu (original behaviour).
          UIMenu* popup = [UIMenu menuWithTitle:title
                                          image:nil
                                     identifier:ident
                                        options:0
                                       children:kids];
          [builder insertChildMenu:popup atEndOfMenuForIdentifier:UIMenuRoot];
          ++top;
        }
      }
    }
    if (out_merged)    *out_merged    = merged;
    if (out_top_level) *out_top_level = top;
  }

  // Build the top-level UIMenu: wraps menu_ios_build_top_level_menus under a
  // single root. Suitable for UIButton.menu (showsMenuAsPrimaryAction). The
  // hamburger lives on one UIButton (not the shared system-menu builder), so a
  // scope_id of 0 is fine - identifiers only need to be unique within that one
  // button's menu, which the per-item id already guarantees.
  template <class MenubarT>
  inline UIMenu* menu_ios_build_uimenu(const MenubarT& mb, NSString* root_title,
                                       const std::function<void(uint32_t)>& pick)
  {
    NSArray<UIMenu*>* top = menu_ios_build_top_level_menus(mb, /*scope_id=*/0, pick);
    return [UIMenu menuWithTitle:(root_title ? root_title : @"")
                        children:top];
  }

} // namespace neui_detail

#endif // TARGET_OS_IPHONE
#endif // __APPLE__

#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdint>
#include <cstring>

// macOS NSMenu helpers shared by both the xpl host
// (`hosts/crossplatform/platform_macos.mm`) and the native macOS host
// (`hosts/macos/`). Both targets NSApp.mainMenu - only the per-host menu
// activation routing differs (each host has its own NEUIMenuTarget singleton
// reaching into its own session registry), so the helpers here are just the
// pure factories + translation tables.
//
// CONVENTION: include from `.mm` files only (this header imports AppKit).

namespace neui_detail
{
  // "Save\tCtrl+S" -> "Save". macOS shows the keyEquivalent separately rather
  // than appended to the title.
  inline NSString* macos_menu_title_only(const char* display_text)
  {
    if (!display_text) return @"";
    const char* tab = strchr(display_text, '\t');
    if (!tab) return [NSString stringWithUTF8String:display_text];
    NSData* d = [NSData dataWithBytes:display_text length:(NSUInteger)(tab - display_text)];
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
  }

  // Find an NSMenuItem in `menu` whose tag matches `cmd_id`. Includes
  // separators (which we tag with their cmd_id too).
  inline NSMenuItem* macos_find_item_with_tag(NSMenu* menu, uint32_t cmd_id)
  {
    if (!menu) return nil;
    for (NSMenuItem* it in menu.itemArray) {
      if ((uint32_t)it.tag == cmd_id) return it;
    }
    return nil;
  }

  inline NSMenuItem* macos_find_popup_item(NSMenu* menu, NSMenu* submenu)
  {
    if (!menu || !submenu) return nil;
    for (NSMenuItem* it in menu.itemArray) {
      if (it.submenu == submenu) return it;
    }
    return nil;
  }

  // Translate NEUI_KMOD_* to NSEventModifierFlags. Cmd is the platform-primary
  // (NEUI_KMOD_CTRL); Control is the secondary (NEUI_KMOD_META). Same mapping
  // as keyboard input translation in keys_macos.h.
  inline NSEventModifierFlags macos_neui_mods_to_appkit(uint32_t mods)
  {
    NSEventModifierFlags f = 0;
    if (mods & NEUI_KMOD_SHIFT) f |= NSEventModifierFlagShift;
    if (mods & NEUI_KMOD_CTRL)  f |= NSEventModifierFlagCommand;
    if (mods & NEUI_KMOD_ALT)   f |= NSEventModifierFlagOption;
    if (mods & NEUI_KMOD_META)  f |= NSEventModifierFlagControl;
    return f;
  }

  // Translate a NEUI_KEY_* code into the string AppKit expects in
  // NSMenuItem.keyEquivalent.
  inline NSString* macos_key_to_keyEquivalent(uint32_t key)
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
      case NEUI_KEY_RETURN: return [NSString stringWithFormat:@"%C", (unichar)NSCarriageReturnCharacter];
      case NEUI_KEY_TAB:    return [NSString stringWithFormat:@"%C", (unichar)NSTabCharacter];
      case NEUI_KEY_SPACE:  return @" ";
      case NEUI_KEY_ESCAPE: return [NSString stringWithFormat:@"%C", (unichar)0x1B];
      case NEUI_KEY_BACK:   return [NSString stringWithFormat:@"%C", (unichar)NSBackspaceCharacter];
      case NEUI_KEY_DELETE: return [NSString stringWithFormat:@"%C", (unichar)NSDeleteCharacter];
      case NEUI_KEY_LEFT:   return [NSString stringWithFormat:@"%C", (unichar)NSLeftArrowFunctionKey];
      case NEUI_KEY_RIGHT:  return [NSString stringWithFormat:@"%C", (unichar)NSRightArrowFunctionKey];
      case NEUI_KEY_UP:     return [NSString stringWithFormat:@"%C", (unichar)NSUpArrowFunctionKey];
      case NEUI_KEY_DOWN:   return [NSString stringWithFormat:@"%C", (unichar)NSDownArrowFunctionKey];
      case NEUI_KEY_HOME:   return [NSString stringWithFormat:@"%C", (unichar)NSHomeFunctionKey];
      case NEUI_KEY_END:    return [NSString stringWithFormat:@"%C", (unichar)NSEndFunctionKey];
      case NEUI_KEY_INSERT: return [NSString stringWithFormat:@"%C", (unichar)NSInsertFunctionKey];
      case NEUI_KEY_F1:     return [NSString stringWithFormat:@"%C", (unichar)NSF1FunctionKey];
      case NEUI_KEY_F2:     return [NSString stringWithFormat:@"%C", (unichar)NSF2FunctionKey];
      case NEUI_KEY_F3:     return [NSString stringWithFormat:@"%C", (unichar)NSF3FunctionKey];
      case NEUI_KEY_F4:     return [NSString stringWithFormat:@"%C", (unichar)NSF4FunctionKey];
      case NEUI_KEY_F5:     return [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
      case NEUI_KEY_F6:     return [NSString stringWithFormat:@"%C", (unichar)NSF6FunctionKey];
      case NEUI_KEY_F7:     return [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
      case NEUI_KEY_F8:     return [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
      case NEUI_KEY_F9:     return [NSString stringWithFormat:@"%C", (unichar)NSF9FunctionKey];
      case NEUI_KEY_F10:    return [NSString stringWithFormat:@"%C", (unichar)NSF10FunctionKey];
      case NEUI_KEY_F11:    return [NSString stringWithFormat:@"%C", (unichar)NSF11FunctionKey];
      case NEUI_KEY_F12:    return [NSString stringWithFormat:@"%C", (unichar)NSF12FunctionKey];
      default:              return @"";
    }
  }

  // Prepend a standard macOS App menu with Quit (Cmd+Q via AppKit's
  // terminate:) if not already present. Idempotent - checks the first
  // item's representedObject == "neui.app_menu" sentinel.
  inline void macos_install_app_menu(NSMenu* root)
  {
    if (!root) return;
    if (root.itemArray.count > 0) {
      NSMenuItem* first = root.itemArray[0];
      if ([first.representedObject isEqual:@"neui.app_menu"]) return;
    }

    NSMenu* app = [[NSMenu alloc] init];
    NSString* name = [NSProcessInfo.processInfo processName];
    NSMenuItem* quit = [[NSMenuItem alloc]
      initWithTitle:[NSString stringWithFormat:@"Quit %@", name]
             action:@selector(terminate:)
      keyEquivalent:@"q"];
    quit.target = NSApp;
    quit.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [app addItem:quit];

    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    app_item.title             = name;
    app_item.submenu           = app;
    app_item.representedObject = @"neui.app_menu";
    [root insertItem:app_item atIndex:0];
  }

} // namespace neui_detail

#endif // __APPLE__

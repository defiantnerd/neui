<!-- neui reference. Extracted from CLAUDE.md - read when working on these topics. -->

## Frame resize, window icon, focus

Resize: Win32 `WM_SIZE` (skip `SIZE_MINIMIZED`; physical→logical via `MulDiv(phys, 96, dpi)`); macOS `windowDidResize:`. Both emit `RESIZE { widget, width, height }` in logical px. Min/max attrs drive `WM_GETMINMAXINFO` / `NSWindow.min/maxSize`. `WM_DPICHANGED` triggers a follow-up `WM_SIZE`. Icon (`NEUI_ATTR_ICON_PATH`): Win32 `WM_SETICON`; macOS `NSApp.applicationIconImage`.

Focus: clients see **logical** focus only. Tab traversal hand-rolled on xpl (`Session::_focused_widget`, `focus_next`); Win32 uses `IsDialogMessage` + `WS_TABSTOP`; macOS builds the key-view loop in **widget-creation order** (`rebuild_key_view_loop_macos`: pre-order DFS over the tab-stop set - BUTTON / INPUTBOX / MULTILINE / CHECKBOX[3] / LISTBOX / COMBOBOX / TREEVIEW / SLIDER / CUSTOMDRAW / GRID, honouring `NEUI_ATTR_TAB_STOP=0`; scroll-hosted controls use their document view), chained via `setNextKeyView:` (`autorecalculatesKeyViewLoop` off). Frame focus → `WIDGET_FOCUS`. `Session::_os_focused` false → paint reports "no focus" (caret + outline hide; logical state preserved).

## Theme palette

Process-wide `neui_detail::Palette` (`theme_palette.h`) - flat array indexed by `ColorRole` (frame_bg, panel_bg, control_bg, accent, text_primary, border, scrollbar_*, ime_underline_*, …); Win32 + macOS providers populate from system sources and fire `Session::on_theme_changed` on flips.

**Per-session override** (`NEUI_ATTR_THEME_MODE`): Session computes `_effective_palette` per mode and points `active_palette_override_ptr()` at it; `current_palette()` consults it first. AUTO copies system; LIGHT/DARK = defaults + live system accent. **Per-frame opt-in** (`NEUI_ATTR_FOLLOW_SYSTEM_THEME = 1`): DWM dark title bar + dark HMENU + palette-driven brushes + invalidate on theme flip; without it OS-default chrome, no auto-invalidate.

**Win32 manifest** (`examples/neui_example.manifest`) declares Win10/11 `supportedOS` GUIDs + Per-monitor v2 DPI + UTF-8 ACP (without the GUIDs Windows gates off uxtheme dark mode). `NEUI_API_THEME_CLIENT` - optional client theme-change callback, fires after framework invalidation.


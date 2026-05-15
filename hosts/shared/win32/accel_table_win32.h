#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <vector>
#include <neui/d/keys.h>
#include <neui/d/commands.h>

// Win32 accelerator-table helpers. Builds an HACCEL from a list of
// (modifiers, key, cmd_id) entries collected from a menubar's items.
// Header-only inline so both hosts can include without ODR conflicts.

namespace neui_detail
{
  struct AccelEntry
  {
    uint32_t mods;    // NEUI_KMOD_* bits
    uint32_t key;     // NEUI_KEY_* (== Win32 VK_*)
    uint32_t cmd_id;  // WM_COMMAND id this entry should fire
  };

  // Map NEUI_KMOD_* bits to ACCEL.fVirt bits. NEUI_KMOD_META is dropped on
  // Win32 (no native ACCEL flag for the Windows key); items needing it
  // would have to be intercepted via WM_KEYDOWN, which is out of scope.
  inline BYTE accel_fvirt(uint32_t mods)
  {
    BYTE v = FVIRTKEY;
    if (mods & NEUI_KMOD_CTRL)  v |= FCONTROL;
    if (mods & NEUI_KMOD_SHIFT) v |= FSHIFT;
    if (mods & NEUI_KMOD_ALT)   v |= FALT;
    return v;
  }

  // Builds an HACCEL from the entries. Returns nullptr if there are no
  // valid entries. Caller owns the returned handle and must
  // DestroyAcceleratorTable when done.
  inline HACCEL build_accel_table(const std::vector<AccelEntry>& entries)
  {
    std::vector<ACCEL> table;
    table.reserve(entries.size());
    for (const auto& e : entries) {
      if (e.key == NEUI_KEY_NONE || e.cmd_id == 0) continue;
      ACCEL a = {};
      a.fVirt = accel_fvirt(e.mods);
      a.key   = static_cast<WORD>(e.key);
      a.cmd   = static_cast<WORD>(e.cmd_id);
      table.push_back(a);
    }
    if (table.empty()) return nullptr;
    return CreateAcceleratorTableW(table.data(), static_cast<int>(table.size()));
  }

  inline void destroy_accel_table(HACCEL& h)
  {
    if (h) {
      DestroyAcceleratorTable(h);
      h = nullptr;
    }
  }

  // Append platform-standard alias accelerators for the given built-in
  // command. The menu item's primary (mods, key) is shown in the menu and
  // is added separately by the caller; this function emits ADDITIONAL
  // entries that should also fire `cmd_id` on Win32. Example: NEUI_CMD_REDO
  // adds Ctrl+Shift+Z, since modern Windows apps accept it alongside Ctrl+Y.
  inline void append_builtin_command_aliases(uint32_t menu_cmd,
                                              uint32_t primary_mods,
                                              uint32_t primary_key,
                                              uint32_t cmd_id,
                                              std::vector<AccelEntry>& out)
  {
    if (menu_cmd == NEUI_CMD_NONE || cmd_id == 0) return;

    auto already_primary = [&](uint32_t m, uint32_t k) {
      return m == primary_mods && k == primary_key;
    };

    switch (menu_cmd) {
      case NEUI_CMD_REDO: {
        // Standard Win32 alias: Ctrl+Shift+Z. Skip if it's already the
        // primary binding (avoid duplicate ACCEL entries).
        uint32_t am = NEUI_KMOD_CTRL | NEUI_KMOD_SHIFT;
        uint32_t ak = NEUI_KEY_Z;
        if (!already_primary(am, ak))
          out.push_back({ am, ak, cmd_id });
        break;
      }
      default:
        break;
    }
  }

} // namespace neui_detail

#endif // _WIN32

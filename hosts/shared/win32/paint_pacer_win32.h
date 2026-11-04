#pragma once

// The Win32 side of paint pacing (../paint_pacer.h) for HWNDs drawn through a
// Direct2D swap chain: the xpl frame window and the native host's painted
// widgets. Per HWND:
//   WM_PAINT     first `if (pace_hold(p, hwnd)) return 0;`, after the paint
//                (which presented) `pace_presented(p)`, which stamps the time
//                pace_hold let the paint start
//   WM_TIMER     `if (pace_timer(p, hwnd, wParam)) return 0;`
//   WM_SIZE, WM_DPICHANGED, showing the window
//                `pace_force(p)`: that paint must not wait
//   WM_DESTROY   `pace_release(p, hwnd)`
// A held paint keeps the window's update region and validates it (an invalid
// window would get WM_PAINT again at once); a one-shot WM_TIMER puts the
// region back when the paint is due, so the paint that follows sees all the
// damage it would have seen.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "../paint_pacer.h"

namespace neui_detail
{
  // WM_TIMER id of a held paint, on the painting HWND.
  constexpr UINT_PTR k_paint_pace_timer_id = 0x6E707063;  // 'nppc'

  // Plain data (no destructor): it lives in structs that move; pace_release
  // frees it in WM_DESTROY.
  struct PaintPacerWin32
  {
    PaintPacer core;
    HRGN       held     = nullptr;  // damage of held paints, client px
    bool       timer    = false;
    HMONITOR   monitor  = nullptr;
    uint64_t   interval = 0;        // ns per refresh of `monitor`
    uint64_t   queried  = 0;        // when `interval` was read
    uint64_t   start    = 0;        // when pace_hold last let a paint run
  };

  inline uint64_t pace_now_ns()
  {
    using namespace std::chrono;
    return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
  }

  // ns per refresh of the monitor showing `hwnd`; read again when the window
  // moves to another monitor and every 2 s (a mode change).
  inline uint64_t pace_interval(PaintPacerWin32& p, HWND hwnd, uint64_t now)
  {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (mon == p.monitor && p.interval && now - p.queried < 2000000000ull) return p.interval;
    DWORD hz = 60;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi)) {
      DEVMODEW dm{};
      dm.dmSize = sizeof dm;
      if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;  // 0 and 1 mean "the hardware default"
    }
    hz = std::min<DWORD>(std::max<DWORD>(hz, 24), 1000);
    p.monitor  = mon;
    p.interval = 1000000000ull / hz;
    p.queried  = now;
    return p.interval;
  }

  // Puts held damage back into the update region and stops the timer.
  inline void pace_unhold(PaintPacerWin32& p, HWND hwnd)
  {
    if (p.timer) {
      KillTimer(hwnd, k_paint_pace_timer_id);
      p.timer = false;
    }
    if (p.held) {
      InvalidateRgn(hwnd, p.held, FALSE);
      DeleteObject(p.held);
      p.held = nullptr;
    }
  }

  // First thing in WM_PAINT. True: hold this paint (return 0 from WM_PAINT).
  // False: paint now; damage held earlier is back in the update region.
  inline bool pace_hold(PaintPacerWin32& p, HWND hwnd)
  {
    const uint64_t now = pace_now_ns();
    uint64_t wait = 0;
    if (p.core.hold(now, pace_interval(p, hwnd, now), wait)) {
      if (!p.timer) {
        const UINT ms = static_cast<UINT>((wait + 999999u) / 1000000u);
        p.timer = SetTimer(hwnd, k_paint_pace_timer_id, ms ? ms : 1u, nullptr) != 0;
      }
      HRGN rgn = p.timer ? CreateRectRgn(0, 0, 0, 0) : nullptr;
      if (rgn) {
        const int kind = GetUpdateRgn(hwnd, rgn, FALSE);
        if (kind == SIMPLEREGION || kind == COMPLEXREGION) {
          if (!p.held) {
            p.held = rgn;
            rgn = nullptr;
          } else {
            CombineRgn(p.held, p.held, rgn, RGN_OR);
          }
          if (rgn) DeleteObject(rgn);
          ValidateRgn(hwnd, nullptr);
          return true;
        }
        DeleteObject(rgn);
      }
      // No timer, no region object, or an empty update region (an internal
      // paint request, which has nothing to keep): paint now rather than
      // lose the frame.
    }
    pace_unhold(p, hwnd);
    p.start = now;
    return false;
  }

  // After a paint that presented.
  inline void pace_presented(PaintPacerWin32& p) { p.core.on_present(p.start); }

  // WM_TIMER: true when it was the pacing timer (the held damage is invalid
  // again, so WM_PAINT follows).
  inline bool pace_timer(PaintPacerWin32& p, HWND hwnd, WPARAM id)
  {
    if (id != k_paint_pace_timer_id) return false;
    pace_unhold(p, hwnd);
    return true;
  }

  inline void pace_force(PaintPacerWin32& p) { p.core.force(); }

  inline void pace_release(PaintPacerWin32& p, HWND hwnd)
  {
    if (p.timer) KillTimer(hwnd, k_paint_pace_timer_id);
    p.timer = false;
    if (p.held) DeleteObject(p.held);
    p.held = nullptr;
  }
}  // namespace neui_detail

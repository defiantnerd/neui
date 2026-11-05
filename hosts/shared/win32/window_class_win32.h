#pragma once

// Win32 window classes, and resources, belong to the module that holds
// neui's code - the exe for an application, the DLL for a plugin.
//
// A window class is registered for a module (WNDCLASSEXW::hInstance), and
// CreateWindowExW looks for the class of that name registered for the module
// it is given; several modules in one process may each register a class of
// the same name. Registered for the process's exe (GetModuleHandleW(nullptr))
// instead, the first neui binary in a process owned the class for every later
// one: a host that loads two plugins built on neui - or one plugin in two
// formats - ran every window on the first one's window procedure, with that
// binary's state, and once the first was unloaded the class pointed into
// freed memory. So neui registers, creates and looks up its classes with
// module_instance().
//
// NEUI_WIN32_CLASS_SUFFIX (the CMake option of that name; empty by default,
// so the names stay as they are) ends every class name neui registers, as
// NEUI_OBJC_SUFFIX ends the Objective-C ones: with
// -DNEUI_WIN32_CLASS_SUFFIX=_AcmeSynth_1_2 "NeUiXplHost" is
// "NeUiXplHost_AcmeSynth_1_2". The module already keeps the classes apart; the
// suffix tells a plugin's windows apart by name too, for anything that finds
// windows by their class name (FindWindowExW, a host, a test, Spy++).
//
// Window classes registered by a DLL outlive it: Windows unregisters them when
// the process ends, not when the DLL is unloaded. register_window_class()
// therefore replaces a class it finds left over from an earlier image of the
// module (a plugin unloaded and loaded again at the same address).
//
// Embedded resources (the "PNG" and icon resources image_loader_win32.h and
// icon_win32.h load) are looked for in the same module first: resource_modules().

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <array>

// A class name: NEUI_WIN32_CLASS(L"NeUiXplHost"). The suffix is a narrow
// string literal (CMake passes it quoted); next to a wide one it is wide.
#if defined(NEUI_WIN32_CLASS_SUFFIX)
#define NEUI_WIN32_CLASS(name) name NEUI_WIN32_CLASS_SUFFIX
#else
#define NEUI_WIN32_CLASS(name) name
#endif

namespace neui_win32
{
  // The module that holds this code. In a static neui linked into a DLL
  // that is the DLL; in an application, the exe.
  inline HINSTANCE module_instance()
  {
    static const HINSTANCE s_module = [] {
      HMODULE m = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&module_instance), &m))
        m = GetModuleHandleW(nullptr);
      return static_cast<HINSTANCE>(m);
    }();
    return s_module;
  }

  // Registers `wc` for module_instance() (whatever wc.hInstance says). A
  // class of that name already registered for the module with another window
  // procedure is a leftover of an earlier image of it (see above) and is
  // replaced; one with the same procedure is this image's and is kept.
  // False if the class cannot be registered.
  inline bool register_window_class(WNDCLASSEXW wc)
  {
    wc.cbSize    = sizeof(wc);
    wc.hInstance = module_instance();
    if (RegisterClassExW(&wc)) return true;
    if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW have = {};
    have.cbSize = sizeof(have);
    if (GetClassInfoExW(wc.hInstance, wc.lpszClassName, &have) &&
        have.lpfnWndProc == wc.lpfnWndProc)
      return true;
    if (!UnregisterClassW(wc.lpszClassName, wc.hInstance)) return false;
    return RegisterClassExW(&wc) != 0;
  }

  // Whether `hwnd` is a window of this module's class `name` - not of
  // another module's class of the same name, nor another process's.
  inline bool is_own_window_class(HWND hwnd, const wchar_t* name)
  {
    wchar_t cls[257] = {};   // a class name has at most 256 characters
    if (!hwnd || !GetClassNameW(hwnd, cls, 257) || wcscmp(cls, name) != 0)
      return false;
    return reinterpret_cast<HINSTANCE>(GetClassLongPtrW(hwnd, GCLP_HMODULE)) ==
           module_instance();
  }

  // The modules an embedded resource is looked for in, in this order:
  // neui's (a plugin's resources are in its DLL), then the exe (an
  // application that links neui as a DLL keeps them there). The second is
  // null where both are the same module.
  inline std::array<HMODULE, 2> resource_modules()
  {
    const HMODULE own = static_cast<HMODULE>(module_instance());
    const HMODULE exe = GetModuleHandleW(nullptr);
    return { own, exe != own ? exe : nullptr };
  }
}  // namespace neui_win32

#endif  // _WIN32

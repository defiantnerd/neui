#pragma once

#include "host.h"
#include <CommCtrl.h>

#pragma comment(lib, "Comctl32")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace win32_host
{
  bool run();
  bool pump_once();
  void set_hinstance(HINSTANCE h);
  HINSTANCE get_hinstance();
  void register_classes();
  LRESULT CALLBACK AppWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT CALLBACK ImageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT CALLBACK PaintedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT CALLBACK ChildSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

  // Apply the dark / light visual style to a native control. Called after
  // CreateWindowExW for opted-in frames, and from Session::on_theme_changed
  // when the system theme flips. Uses SetWindowTheme(hwnd,
  // L"DarkMode_Explorer", NULL) for dark and L"Explorer" for light -
  // these theme classes ship with Windows since Win10 1809 and are what
  // File Explorer / Notepad / Settings use. For TreeView additionally
  // sets bg / text / line colours from the palette.
  void apply_native_theme_w32(WidgetData& wd);
}

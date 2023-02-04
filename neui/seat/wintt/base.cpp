#include "base.h"

#pragma comment(lib, "uxtheme")

namespace wintt
{
  HMODULE GetCurrentModule()
  { // NB: XP+ solution!
    HMODULE hModule = NULL;
    GetModuleHandleEx(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
      (LPCTSTR)GetCurrentModule,
      &hModule);

    return hModule;
  }  

  // info structure to pass a lambda through lParam of enumChildProc
  // see https://docs.microsoft.com/en-us/previous-versions/windows/desktop/legacy/ms633493(v=vs.85)

  struct enumCallBackInfo
  {
    HWND parent;
    std::function<void(HWND window)> function;
  };

  static BOOL CALLBACK enumChildProc(
    _In_ HWND   hwnd,
    _In_ LPARAM lParam
  )
  {
    // restore the cbi from the lParam pointer
    auto cbi = reinterpret_cast<enumCallBackInfo*>(lParam);
    // call the lambda
    cbi->function(hwnd);
    return TRUE;
  }

  void enumWin32ChildWindows(HWND parent, std::function<void(HWND window)> callback)
  {
    enumCallBackInfo cbi{ parent, callback };
    // enumerate child windows via the Win32 API
    EnumChildWindows(parent, enumChildProc, (LPARAM) &cbi);
  }
}
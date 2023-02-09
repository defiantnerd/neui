#pragma once

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#include <Windows.h>
#include <windowsx.h>
#include <d2d1_2.h>
#include <d2d1_1helper.h>
#include <d2d1_2helper.h>
#include <wrl.h>
#include <comdef.h>

#include <system_error>

#include <functional>
#include <tchar.h>
#include <Uxtheme.h>
// #include "fmt/format.h"

#define UWM_DPICHANGED WM_USER+WM_DPICHANGED

// notify child windows about new parent resize
#define WMTT_PARENT_WM_SIZE (WM_USER+1000)
#define UWM_UPDATE_STATE (WM_USER+1001)
#define UWM_BN_CLICKED (WM_USER+1002)

namespace neui
{
  namespace win
  {

    

    // default run loop
    LRESULT runLoop(HWND basehwnd);

    void enumWin32ChildWindows(HWND parent, std::function<void(HWND window)> callback);

    // tools for windows programming ----------------------------------------------------------------------------------------------------

    HMODULE GetCurrentModule();

    // convert UTF-8 string to wstring
    std::wstring utf8_to_wstring(const std::string_view str);

    // convert wstring to UTF-8 string
    std::string wstring_to_utf8(const std::wstring& wstr);

    DWORD GetWindowDPI(HWND hwnd);

#ifdef _UNICODE
    typedef std::wstring tstring;
#else
    typedef std::string tstring;
#endif

    // error handling  -----------------------------------------------------------------------------------------------------------------
    inline void require(bool condition) {
      if (!condition) throw std::runtime_error(std::system_category().message(::GetLastError()));
    }

    inline void require(bool condition, errno_t error) {
      if (!condition) throw std::runtime_error(std::system_category().message(error));
    }

    inline void com_require(int result) {

      if (S_OK != result)
      {
        _com_error err(result);
        LPCTSTR errMsg = err.ErrorMessage();

        throw std::runtime_error(wstring_to_utf8(errMsg).c_str());
      }
    }

    // resource tools  -----------------------------------------------------------------------------------------------------------------
    tstring LoadString(HINSTANCE hInstance, UINT uID);

    using D2D1BitmapRef = Microsoft::WRL::ComPtr<ID2D1Bitmap>;
   
    void enumWin32ChildWindows(HWND parent, std::function<void(HWND window)> callback);
  }
}



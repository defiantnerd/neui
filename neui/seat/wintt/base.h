#pragma once

#include <system_error>
#include <comdef.h>

#include <functional>

#include "../framework.h"
#include "fmt/format.h"

namespace wintt
{
  HMODULE GetCurrentModule();
  LRESULT runLoop(HWND basehwnd);

  // convert UTF-8 string to wstring
  std::wstring utf8_to_wstring(const std::string& str);

  // convert wstring to UTF-8 string
  std::string wstring_to_utf8(const std::wstring& wstr);

#ifdef _UNICODE
  typedef std::wstring tstring;
#else
  typedef std::string tstring;
#endif
  tstring LoadString(HINSTANCE hInstance, UINT uID);

  inline void require(bool condition) {
    if (!condition) throw std::runtime_error(std::system_category().message(::GetLastError()));
  }

  inline void com_require(int result) {

    if (S_OK != result)
    {
      _com_error err(result);
      LPCTSTR errMsg = err.ErrorMessage();
      
      throw std::runtime_error(wstring_to_utf8(errMsg).c_str());
    }
  }


  inline void require(bool condition, errno_t error) {
    if (!condition) throw std::runtime_error(std::system_category().message(error));
  }

  void enumWin32ChildWindows(HWND parent, std::function<void(HWND window)> callback);
}

namespace fm = fmt;

namespace Log
{
  template<class A, class...B>
  void write(const A a, B...b)
  {
    auto result = fmt::format(a, b...);
    OutputDebugString(result.c_str());
  }

  template<class A, class...B>
  void writeln(const A a, B...b)
  {
    auto result = fmt::format(a, b...);
    result.append(_T("\n"));
    OutputDebugString(result.c_str());
  }

}

// user messages for wintt windows


// notify child windows about new parent resize
#define WMTT_PARENT_WM_SIZE (WM_USER+1000)

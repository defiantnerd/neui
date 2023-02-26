#include "base.h"

#pragma comment(lib, "uxtheme")

namespace neui
{
  namespace win
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
      // enumerate child windows
      EnumChildWindows(parent, enumChildProc, (LPARAM)&cbi);
    }

    // convert UTF-8 string to wstring
    std::wstring utf8_to_wstring(const std::string_view str)
    {
      if (str.empty())
      {
        return std::wstring();
      }
      int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
      std::wstring wstrTo(size_needed, 0);
      MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
      return wstrTo;
    }

    // convert wstring to UTF-8 string
    std::string wstring_to_utf8(const std::wstring& wstr)
    {

      BOOL usedDefault = false;
      int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, " ", &usedDefault);
      std::string strTo(size_needed, 0);
      WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, " ", &usedDefault);
      return strTo;
    }

    tstring LoadString(HINSTANCE hInstance, UINT uID)
    {
      PCTSTR pws;
      int cch = LoadString(hInstance, uID, reinterpret_cast<LPTSTR>(&pws), 0);
      return tstring(pws, cch);
    }

    class BaseGlobal
    {
      typedef HRESULT(WINAPI* PGetDpiForMonitor)(HMONITOR hmonitor, int dpiType, UINT* dpiX, UINT* dpiY);
    public:
      BaseGlobal()
      {
        shcoreDll = LoadLibrary(_T("shcore"));
        if (shcoreDll)
        {
          pGetDpiForMonitor =
            reinterpret_cast<PGetDpiForMonitor>(GetProcAddress(shcoreDll, "GetDpiForMonitor"));
        }
      }
      ~BaseGlobal()
      {
        pGetDpiForMonitor = nullptr;
        FreeLibrary(shcoreDll);
      }
      WORD GetWindowDPI(HWND hWnd)
      {
        if (pGetDpiForMonitor)
        {
          HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY);
          UINT uiDpiX;
          UINT uiDpiY;
          HRESULT hr = pGetDpiForMonitor(hMonitor, 0, &uiDpiX, &uiDpiY);
          if (SUCCEEDED(hr))
          {
            return static_cast<WORD>(uiDpiX);
          }
        }
        // We couldn't get the window's DPI above, so get the DPI of the primary monitor
// using an API that is available in all Windows versions.
        HDC hScreenDC = GetDC(0);
        int iDpiX = GetDeviceCaps(hScreenDC, LOGPIXELSX);
        ReleaseDC(0, hScreenDC);

        return static_cast<WORD>(iDpiX);
      }

    private:
      HMODULE shcoreDll = 0;
      PGetDpiForMonitor pGetDpiForMonitor = nullptr;
    } gGlobal;

    DWORD GetWindowDPI(HWND hwnd)
    {
      return gGlobal.GetWindowDPI(hwnd);
    }
  }
}
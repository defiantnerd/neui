#include "example.h"

namespace wintt
{  

  Example::Example(HWND parent)
    : BaseWindow()
  {
    WNDCLASSEX wcex;
    memset(&wcex, 0, sizeof(wcex));
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.cbWndExtra = 0;
    wcex.hbrBackground = (HBRUSH)CreateSolidBrush(0xFFA100); 
    wcex.hCursor = LoadCursor(0, IDC_ARROW);
    wcex.lpfnWndProc = &BaseWindow::basicWndProc;
    wcex.lpszClassName = _T("Example");
    wcex.hInstance = gInstance;
    wcex.style = CS_HREDRAW | CS_VREDRAW;

    ATOM atom = RegisterClassEx(&wcex);
    // WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr

    auto h = CreateWindowEx(WS_EX_LAYERED, /* MAKEINTATOM(classAtom) */_T("Example"), _T(""), WS_VISIBLE | WS_CHILDWINDOW | WS_CLIPCHILDREN, 10, 10, 40, 40, parent, 0, gInstance, (BaseWindow*)this);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE| SWP_NOMOVE|SWP_NOREDRAW|SWP_NOSIZE);
    assert(h == hwnd);
    if (hwnd > 0)
    {
      // ::SetParent(hwnd, parent);
      ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);
    }
    else
    {
      auto out = ::GetLastError();
      fprintf(stderr,"err: %d\n", out);
    }
    // this->setAlpha(70);
  }

  Example::~Example()
  {
    if (hwnd)
    {
      ::DestroyWindow(hwnd);
      hwnd = 0;
    }
    UnregisterClass(_T("Example"), gInstance);
  }


  LRESULT Example::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
  {
    switch (message)
    {
    //case WM_RBUTTONDOWN:
    //  ::MessageBox(hwnd, _T("Hoho"), _T("Captain Caption"), MB_OK);
    //  break;
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      // TODO: Add any drawing code that uses hdc here...
      EndPaint(hwnd, &ps);
    }
    break;
    default:
      return BaseWindow::handleWindowMessage(message, wParam, lParam);
    }
    return 0;
  }


}
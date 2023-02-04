#include "Frame.h"

namespace wintt
{  

  Frame::Frame(HWND parent)
    : BaseWindow()
  {
    WNDCLASSEX wcex;
    memset(&wcex, 0, sizeof(wcex));
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.cbWndExtra = 0;
    wcex.hbrBackground = (HBRUSH)CreateSolidBrush(0xFFA100); 
    wcex.hCursor = LoadCursor(0, IDC_ARROW);
    wcex.lpfnWndProc = &BaseWindow::basicWndProc;
    wcex.lpszClassName = _T("Frame");
    wcex.hInstance = gInstance;
    wcex.style = 0; //  CS_HREDRAW | CS_VREDRAW;

    ATOM atom = RegisterClassEx(&wcex);
    // WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr

    auto h = CreateWindowEx(WS_EX_LAYERED | WS_EX_CONTROLPARENT, /* MAKEINTATOM(classAtom) */_T("Frame"), _T(""),
      WS_VISIBLE | WS_CHILDWINDOW 
      , 40, 40, 1000, 700, parent, 0, gInstance, (BaseWindow*)this);
    // SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE| SWP_NOMOVE|SWP_NOREDRAW|SWP_NOSIZE);
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

  Frame::~Frame()
  {
    if (hwnd)
    {
      ::DestroyWindow(hwnd);
      hwnd = 0;
    }
    UnregisterClass(_T("Frame"), gInstance);
  }


  LRESULT Frame::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
  {
    switch (message)
    {
    case WM_RBUTTONDOWN:
      ::MessageBox(hwnd, _T("Hoho"), _T("Captain Caption"), MB_OK);
      break;
    case WM_LBUTTONDOWN:
    { 
      RECT c;
      auto ks = GetKeyState(VK_SHIFT);
      bool shifted = (ks< 0);
      ScrollWindow(hwnd, 0, shifted ? 10 : -10, NULL, &c);
      // InvalidateRect(hwnd,NULL,true);
    }
      break;
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
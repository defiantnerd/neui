#include "canvas.h"
#include "direct2d.h"
#include "apihelper.h"

namespace wintt
{  

  static WindowClass Direct2dWindowClass;

  Direct2DBaseWindow::Direct2DBaseWindow(HWND parent)
    : BaseWindow()
  {
    
    WNDCLASSEX wcex;
    memset(&wcex, 0, sizeof(wcex));
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.cbWndExtra = 0;
    wcex.hbrBackground = (HBRUSH)CreateSolidBrush(0xFFA100);
    wcex.hCursor = LoadCursor(0, IDC_ARROW);
    wcex.lpfnWndProc = &BaseWindow::basicWndProc;
    wcex.lpszClassName = _T("Direct2DBaseWindow");
    wcex.hInstance = gInstance;
    wcex.style = 0; //  CS_HREDRAW | CS_VREDRAW;

    classHandle = std::move(Direct2dWindowClass.Register(wcex));
    
    // WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr

    auto h = CreateWindowEx(
      WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED
      // WS_EX_LAYERED | WS_EX_COMPOSITED | WS_EX_CONTROLPARENT
      , classHandle,
      _T(""), WS_VISIBLE | WS_CHILDWINDOW, 250, 20, 400, 400, parent, 0, gInstance, (BaseWindow*)this);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOSIZE);
    assert(h == hwnd);

    // d2d_init(hwnd, &md2d);
    if (hwnd > 0)
    {
      // ::SetParent(hwnd, parent);
      ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);    // this is needed to be rendered at all
      // SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, /*ULW_COLORKEY |*/ LWA_ALPHA);
      //  this->context = std::make_unique<d2d::Context>(hwnd, 400, 400);
    }
    else
    {
      auto out = ::GetLastError();
      fprintf(stderr, "err: %d\n", out);
    }
    // this->setAlpha(70);
  }

  Direct2DBaseWindow::~Direct2DBaseWindow()
  {
    // d2d_shutdown(&md2d);
    if (hwnd)
    {
      ::DestroyWindow(hwnd);
      hwnd = 0;
    }
    UnregisterClass(_T("Direct2DBaseWindow"), gInstance);
  }


  LRESULT Direct2DBaseWindow::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
  {
    switch (message)
    {
    case WM_CTLCOLORSTATIC:
    {
      {
        static HBRUSH hbrBkgnd = NULL;
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, RGB(255, 255, 255));
        SetBkColor(hdcStatic, RGB(0, 0, 0));
        if (hbrBkgnd == NULL)
        {
          hbrBkgnd = CreateSolidBrush(RGB(0, 0, 0));
        }
        return (INT_PTR)hbrBkgnd;
      }

      //  return 1;
      //  //SetTextColor(hdcStatic, RGB(255, 255, 255));
      //  //SetBkColor(hdcStatic, RGB(0, 0, 0));

      //  //if (hbrBkgnd == NULL)
      //  //{
      //  //  hbrBkgnd = CreateSolidBrush(RGB(0, 0, 0));
      //  //}
      //  //return (INT_PTR)hbrBkgnd;
      //}
    }
    case WM_CREATE:
      // context = std::make_unique<d2d::Context>(hwnd, 400, 400);      
      break;
    case WM_PAINT:
    {
#if 0
      PAINTSTRUCT ps;
      // HDC hdc = BeginPaint(hwnd, &ps);
      // TODO: Add any drawing code that uses hdc here...
      d2d::Context(hwnd, 400, 400).render();
      // context->render();
      // EndPaint(hwnd, &ps);
#else
      this->context->render();
      // d2d_render(&md2d);
      ValidateRect(hwnd, NULL);
#endif
    }
    break;
    case WM_ERASEBKGND:
      return 0;
    case WM_DISPLAYCHANGE:
    {
      InvalidateRect(hwnd, NULL, TRUE);
    }
    break;
    case WMTT_PARENT_WM_SIZE:
    {
      UINT width = LOWORD(lParam);
      UINT height = HIWORD(lParam);

      if (width > 50) width -= 40;
      if (height > 50) height -= 40;

      SetWindowPos(hwnd, 0, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOCOPYBITS);
    }
    break;
    case WM_SIZE:
    {
      auto result = BaseWindow::handleWindowMessage(message, wParam, lParam);
      UINT width = LOWORD(lParam);
      UINT height = HIWORD(lParam);
#if 1
      if (!context)
      {
        context = std::make_unique<d2d::Context>(hwnd, width, height);
      }
      else
      {
        context->resize(width, height);
      }
#endif
      return result;
    }
    break;
    case WM_RBUTTONUP:
    case WM_RBUTTONDOWN:
    {
#if 1
      WNDPROC wp = 0;
      HWND hParent = 0;
      POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      //swprintf_s(buf, sizeof(buf), _T("Right-Click in control!\nx: %i\ny: %i\n\nNow, we'll convert this to the coordinates of the frame and pass the message up-stream!"),
      //  p.x, p.y);
      if (p.x >= 50 || p.y >= 50)
      {
        hParent = GetParent(hwnd);
        MessageBox(hParent, _T("Right Click Direct2DBaseWindow, will bubble up..."), _T("Message"), MB_ICONINFORMATION);
        ClientToScreen(hwnd, &p);
        ScreenToClient(hParent, &p);
        SendMessage(hParent, message, wParam, MAKELPARAM(p.x, p.y));
      }
      else
      {
        InvalidateRect(hwnd, NULL, TRUE);
      }
#else
      InvalidateRect(hwnd, NULL, TRUE);
#endif
    }
    break;
    default:
      return BaseWindow::handleWindowMessage(message, wParam, lParam);
    }
    return 0;
  }

  static WindowClass canvasClass;

  Canvas::Canvas(HWND parent)
    : BaseWindow()
  {
    WNDCLASSEX wcex;
    memset(&wcex, 0, sizeof(WNDCLASSEX));
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.cbWndExtra = 0;
    wcex.hbrBackground = (HBRUSH)CreateSolidBrush(0xFFA100); 
    wcex.hCursor = LoadCursor(0, IDC_ARROW);
    wcex.lpfnWndProc = &BaseWindow::basicWndProc;
    wcex.lpszClassName = _T("Canvas");
    wcex.hInstance = gInstance;
    wcex.style = 0; //  CS_HREDRAW | CS_VREDRAW;

    classHandle = canvasClass.Register(wcex);

    // WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr

    auto h = CreateWindowEx( 
      WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED
      // WS_EX_LAYERED | WS_EX_COMPOSITED | WS_EX_CONTROLPARENT
      ,classHandle,
      _T(""), WS_VISIBLE | WS_CHILDWINDOW, 250, 20, 400, 400, parent, 0, gInstance, (BaseWindow*)this);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE| SWP_NOMOVE|SWP_NOREDRAW|SWP_NOSIZE);
    assert(h == hwnd);
    
    // d2d_init(hwnd, &md2d);
    if (hwnd > 0)
    {
      // ::SetParent(hwnd, parent);
      ::SetLayeredWindowAttributes(hwnd, 0, 0, 0);    // this is needed to be rendered at all
      // SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, /*ULW_COLORKEY |*/ LWA_ALPHA);
      //  this->context = std::make_unique<d2d::Context>(hwnd, 400, 400);
    }
    else
    {
      auto out = ::GetLastError();
      fprintf(stderr,"err: %d\n", out);
    }
    // this->setAlpha(70);
  }

  Canvas::~Canvas()
  {
    // d2d_shutdown(&md2d);
    if (hwnd)
    {
      ::DestroyWindow(hwnd);
      hwnd = 0;
    }
  }


  LRESULT Canvas::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
  {
    switch (message)
    {
    case WM_CTLCOLORSTATIC:
    {
      {
        static HBRUSH hbrBkgnd = NULL;
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, RGB(255, 255, 255));
        SetBkColor(hdcStatic, RGB(0, 0, 0));
        if (hbrBkgnd == NULL)
        {
          hbrBkgnd = CreateSolidBrush(RGB(0, 0, 0));
        }
        return (INT_PTR)hbrBkgnd;
      }

      //  return 1;
      //  //SetTextColor(hdcStatic, RGB(255, 255, 255));
      //  //SetBkColor(hdcStatic, RGB(0, 0, 0));

      //  //if (hbrBkgnd == NULL)
      //  //{
      //  //  hbrBkgnd = CreateSolidBrush(RGB(0, 0, 0));
      //  //}
      //  //return (INT_PTR)hbrBkgnd;
      //}
    }
    case WM_CREATE:
      // context = std::make_unique<d2d::Context>(hwnd, 400, 400);      
      break;
    case WM_PAINT:
    {
#if 0
      PAINTSTRUCT ps;
      // HDC hdc = BeginPaint(hwnd, &ps);
      // TODO: Add any drawing code that uses hdc here...
      d2d::Context(hwnd, 400, 400).render();
      // context->render();
      // EndPaint(hwnd, &ps);
#else
      this->context->render();
      // d2d_render(&md2d);
      ValidateRect(hwnd, NULL);
#endif
    }
    break;
    case WM_ERASEBKGND:
      return 0;
    case WM_DISPLAYCHANGE:
    {
      InvalidateRect(hwnd, NULL, TRUE);
    }
    break;
    case WMTT_PARENT_WM_SIZE:
    {
      UINT width = LOWORD(lParam);
      UINT height = HIWORD(lParam);

      if (width > 50 ) width -= 40; 
      if ( height > 50) height -= 40;

      SetWindowPos(hwnd, 0, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOCOPYBITS);
    }
      break;
    case WM_SIZE:
    {
      auto result = BaseWindow::handleWindowMessage(message, wParam, lParam);
      UINT width = LOWORD(lParam);
      UINT height = HIWORD(lParam);
#if 1
      if (!context)
      {
        context = std::make_unique<d2d::Context>(hwnd, width, height);
      }
      else
      {
        context->resize(width, height);
      }      
#endif
      return result;
    }
      break;
    case WM_RBUTTONUP:      
    case WM_RBUTTONDOWN:
      {
  #if 1
        WNDPROC wp = 0;
        HWND hParent = 0;
        POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        //swprintf_s(buf, sizeof(buf), _T("Right-Click in control!\nx: %i\ny: %i\n\nNow, we'll convert this to the coordinates of the frame and pass the message up-stream!"),
        //  p.x, p.y);
        if (p.x >= 50 || p.y >= 50)
        {
          hParent = GetParent(hwnd);
          MessageBox(hParent, _T("Right Click Canvas, will bubble up..."), _T("Message"), MB_ICONINFORMATION);
          ClientToScreen(hwnd, &p);
          ScreenToClient(hParent, &p);
          SendMessage(hParent, message, wParam, MAKELPARAM(p.x, p.y));
        }
        else
        {
          InvalidateRect(hwnd, NULL, TRUE);
        }
  #else
        InvalidateRect(hwnd, NULL, TRUE);
  #endif
      }
      break;
    default:
      return BaseWindow::handleWindowMessage(message, wParam, lParam);
    }
    return 0;
  }


}
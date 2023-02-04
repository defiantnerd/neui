#include "apihelper.h"
#include <cassert>
#include <debugapi.h>

extern HINSTANCE gInstance;

namespace wintt
{
  /*
  * WindowClassHandle is used to hold a reference WindowClass object (with a WNDCLASSEX structure)
  * and can be directly used in the CreateWindowEx call as argument.
  * 
  * It can be safely copied and transfered. If the last instance of an
  * WindowClassHandle object is deleted, the WNDCLASSEX structure is automatically
  * unregistered via UnregisterClassEx call.
  */
  WindowClassHandle::WindowClassHandle()
  {
  }
  WindowClassHandle::WindowClassHandle(const WindowClassHandle& other)
  {
    mParent = other.mParent;
    if ( mParent)
      mParent->addInstance();
  }
  WindowClassHandle::WindowClassHandle(WindowClassHandle&& other) noexcept
  {
    mParent = other.mParent;
    other.mParent = nullptr;
  }
  WindowClassHandle& WindowClassHandle::operator=(const WindowClassHandle& other)
  {
    mParent = other.mParent;
    if (mParent)
      mParent->addInstance();
    return *this;
  }
  WindowClassHandle& WindowClassHandle::operator=(WindowClassHandle&& other) noexcept
  {
    mParent = other.mParent;
    other.mParent = nullptr;
    return *this;
  }
  WindowClassHandle::WindowClassHandle(WindowClass* parent)
    : mParent(parent)
  {
    mParent->addInstance();
  }

  WindowClassHandle::operator LPTSTR()
  {
    if ( mParent)
      return MAKEINTATOM(mParent->getAtom());
    return nullptr;
  }

  WindowClassHandle::~WindowClassHandle()
  {
    if ( mParent )
      mParent->removeInstance();
  }

  WindowClassHandle WindowClass::Register(WNDCLASSEX& wcex)
  {
    if (wcx == nullptr)
    {
      auto size = wcex.cbSize;
      wcx = (PWNDCLASSEX) new uint8_t[size];
      std::memcpy(wcx, &wcex, size);
    }
    else
    {
      if ((wcx->cbSize != wcex.cbSize)
#ifdef _DEBUG
        && (std::memcmp(wcx, &wcex, wcx->cbSize) != 0)
#endif
        )
      {
        throw std::runtime_error("window class {} definition does not match");
      }
    }
    return WindowClassHandle(this);
  }

  /*
  * WindowClass holds a WNDCLASSEX structure and provides WindowClassHandle
  * instances for objects that create window objects.
  */

  void WindowClass::addInstance()
  {
    if (counter == 0)
    {
      atom = RegisterClassEx(wcx);
    }
    ++counter;
  }
  void WindowClass::removeInstance()
  {
    counter--;
    if (wcx && counter == 0)
    {
      UnregisterClass(wcx->lpszClassName, gInstance);
      atom = 0;
      delete[] wcx;
      wcx = nullptr;
    }
  }


  WindowClass::~WindowClass()
  {
    assert(counter == 0);
  }
}
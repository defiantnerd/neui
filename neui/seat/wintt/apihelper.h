#pragma once

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <memory>
#include "../framework.h"

namespace wintt
{
  class WindowClass;

  class WindowClassHandle
  {
  public:
    friend class WindowClass;
    WindowClassHandle();
    WindowClassHandle(const WindowClassHandle& other);
    WindowClassHandle(WindowClassHandle&& other) noexcept;
    WindowClassHandle& operator=(const WindowClassHandle& other);
    WindowClassHandle& operator=(WindowClassHandle&& other) noexcept;
  protected:
    WindowClassHandle(WindowClass* parent);
  public:
    operator LPTSTR();
    ~WindowClassHandle();
  private:
    WindowClass* mParent = nullptr;

  };
  class WindowClass
  {
  public:
    friend class WindowClassHandle;
    WindowClassHandle Register(WNDCLASSEX& wcex);
    ~WindowClass();

    ATOM getAtom() {
      return atom;
    }

  protected:
    void addInstance();
    void removeInstance();
   
  private:
    int counter = 0;
    ATOM atom = 0;
    PWNDCLASSEX wcx = nullptr;
  };
}
#pragma once

#include "boxmodel.h"

namespace neui
{
  class Layout
  {
  public:
    BoxModel boxmodel;
    Rect position;
    const Rect& getContentRect() const { return content; }
    const Rect& getOuterRect() const { return outerbound; }
    void calculate() {}
  protected:
    Rect outerbound;
    Rect content;
  };

  class ILayoutable
  {
  public:
    virtual ~ILayoutable() = default;
    virtual const Size getSize() const = 0;
    virtual void setPosition(const Rect& rect) = 0;
    // virtual const Size& getMinimalSize() = 0;

  };

}
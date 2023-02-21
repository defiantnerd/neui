#pragma once

/*
    common.h
    templates and definitions to be used all over the project

*/

#include "geometry.h"
#include "color.h"
#include <string>

namespace neui
{

  /*
      IRenderer is an interface class returned by a graphical context
      Internally, it is connected to the specific context as well as the resource management
      to obtain bitmaps.

      probable targets are:
      - window contexts
      - bitmaps/surfaces
      - printer output
  */
  class IRenderer
  {
  public:
    using uint = unsigned int;

    virtual ~IRenderer() = default;

    // switch a context to native DPI
    virtual int getDpi() const = 0;
    virtual bool isScaled() const = 0;
    virtual void doScale(const bool scale) = 0;
    // 
    virtual IRenderer& begin() = 0;
    virtual IRenderer& end() = 0;
    // 
    virtual IRenderer& line(const Point from, const Point to) = 0;    
    // polyline
    virtual IRenderer& rect(const Rect rect) = 0;
    virtual IRenderer& rect(const Rect rect, uint distance) = 0;

    virtual IRenderer& circle(const Point center, const uint r) = 0;
    virtual IRenderer& ellipse(const Point center, const uint rx, const uint ry) = 0;
    // polygon
    // path (like svg path) with bezier/quadbezier
    // solid colors
    // gradients (linear/radial)
    // patterns
    // clipping
    virtual IRenderer& pushclip(const Rect rect) = 0;
    virtual IRenderer& popclip() = 0;
    // masking
    // transformations (transpose/scale/rotate/skew?)
    virtual IRenderer& transpose(const Size offset) = 0;
    virtual IRenderer& rotate(const Point center, float normalized_angle) = 0;
    // text rendering
    virtual IRenderer& text(const std::string_view text, const Rect rect, uint ninealign) = 0;
    // fonts
    // svg: g, use, symbol?
  };

  enum class Feature : int
  {
    bitmap,
    rotation,
    skewing,
    utf8,
  };

  class IContext
  {
  public:
    virtual ~IContext() = default;
    virtual bool supports(const Feature) const = 0;
    virtual IRenderer& getRenderer() = 0;
  };

}
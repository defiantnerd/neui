#pragma once

/*
    common.h
    templates and definitions to be used all over the project

*/

#include "geometry.h"

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
    virtual ~IRenderer() = default;
    // 
    // line
    // polyline
    // rect
    // circle
    // elipse
    // polygon
    // path (like svg path) with bezier/quadbezier
    // solid colors
    // gradients (linear/radial)
    // patterns
    // clipping
    // masking
    // transformations (transpose/scale/rotate/skew?)
    // text rendering
    // fonts

    // svg: g, use, symbol?
  };

}
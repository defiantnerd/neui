#pragma once

/*
    bitmap.h
    templates and definitions to be used all over the project

*/

#include <string>
#include "geometry.h"

namespace neui
{
  class Bitmap;

  class Sprite
  {
  public:
    Bitmap* bitmap;
    int index = 0;
  };

  class Bitmap
  {
  public:
  protected:
    int handle = 0;
    std::string name;
    int frames = 0;
    Size size;

  };
}
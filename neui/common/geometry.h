#pragma once
#include "common.h"
#include <algorithm>

namespace neui
{
  // ---------------------------------------------------------
/* Box is a data struct for BoxModel. It's not meant to be used as a rect (use Rect instead),
   so its members may not have negative values and its calculateXXX methods do not calculate the distance between
   its borders. */
  struct Box
  {
    constexpr Box() {}
    constexpr explicit Box(unsigned int p) : left(p), top(p), right(p), bottom(p) {}

    constexpr Box(unsigned int left, unsigned int top, unsigned int right, unsigned int bottom) :
      left(left), right(top), top(right), bottom(bottom) {}

    constexpr void operator+=(const Box& other)
    {
      left += other.left; top += other.top; right += other.right; bottom += other.bottom;
    }

    constexpr Box operator+(const Box& other)
    {
      return Box{ left + other.left, top + other.top, right + other.right, bottom + other.bottom };
    }

    constexpr Box operator-(const Box& other)
    {
      return Box{ left - other.left, top - other.top, right - other.right, bottom - other.bottom };
    }

    constexpr bool operator!=(const Box& other) const
    {
      return (left != other.left || right != other.right || top != other.top || bottom != other.bottom);
    }

    constexpr bool operator==(const Box& other) const
    {
      return (left == other.left && top == other.top && right == other.right && bottom == other.bottom);
    }

    constexpr unsigned int calculateWidth() const { return left + right; }
    constexpr unsigned int calculateHeight() const { return top + bottom; }

    constexpr void clear() { left = right = top = bottom = 0; }
    constexpr bool empty() const { return (left == 0 && top == 0 && right == 0 && bottom == 0); }
    constexpr operator bool() const { return !empty(); }

    unsigned int left = 0;
    unsigned int top = 0;
    unsigned int right = 0;
    unsigned int bottom = 0;
  };

  // non-class member operators --------------------------------
  inline Box operator +(const Box& d1, const Box& d2) { Box temp(d1); temp += d2; return temp; }


  struct Padding : Box 
  {
    Padding() = default;
    constexpr explicit Padding(unsigned int p) : Box(p) {}

    constexpr Padding(unsigned int left, unsigned int top, unsigned int right, unsigned int bottom)
     : Box(left,top,right,left) {}

  };
  struct Margin : Box {
    Margin() = default;
    constexpr explicit Margin(unsigned int p) : Box(p) {}

    constexpr Margin(unsigned int left, unsigned int top, unsigned int right, unsigned int bottom)
      : Box(left, top, right, left) {}
  };
  struct Border : Box 
  {
    Border() = default;
    constexpr explicit Border(unsigned int p) : Box(p) {}

    constexpr Border(unsigned int left, unsigned int top, unsigned int right, unsigned int bottom)
      : Box(left, top, right, left) {}

  };

  // Point is a coordinate
  struct Point
  {
    constexpr Point(int x, int y) : x(x), y(y) {}

    //constexpr Point(const Box& box) : x(static_cast<int>(box.left)),
    //  y(static_cast<int>(box.top)) {}

    constexpr Point() {}

    constexpr bool operator==(const Point& other) const
    {
      return (x == other.x) && (y == other.y);
    }

    constexpr bool operator!=(const Point& other) const
    {
      return (x != other.x) || (y != other.y);
    }

    constexpr const Point operator+(const Point & other) const
    {
      return Point(x + other.x, y + other.y);
    }
    constexpr const Point operator-(const Point & other) const
    {
      return Point(x - other.x, y - other.y);
    }

    constexpr Point& operator+=(const Point& other) 
    {
      x += other.x;
      y += other.y;
      return *this;
    }

    constexpr Point& operator-=(const Point& other)
    {
      x -= other.x;
      y -= other.y;
      return *this;
    }

    int x = 0;
    int y = 0;
  };

  struct Size
  {
    constexpr bool operator==(const Size& other) const
    {
      return (w == other.w) && (h == other.h);
    }

    constexpr bool operator!=(const Size& other) const
    {
      return (w != other.w) || (h != other.h);
    }

    constexpr const Point operator+(const Size& other) const
    {
      return Point(w + other.w, h + other.h);
    }
    constexpr const Point operator-(const Size& other) const
    {
      return Point(w - other.w, h - other.h);
    }

    constexpr Size& operator+=(const Size& other)
    {
      w += other.w;
      h += other.h;
      return *this;
    }

    constexpr Size& operator-=(const Size& other)
    {
      w -= other.w;
      h -= other.h;
      return *this;
    }

    int w = 0;
    int h = 0;
  };

  struct Rect
  {
    constexpr Rect() {}
    constexpr Rect(int x, int y, int w, int h)
    : x(x)
    , y(y)
    , w(w)
    , h(h)
    {}

    constexpr Rect(const Point point, const Size size)
      : x(point.x)
      , y(point.y)
      , w(size.w)
      , h(size.h) {}

    constexpr Rect(const Point& point, const Size& size)
      : x(point.x)
      , y(point.y)
      , w(size.w)
      , h(size.h) {}

    Point getLeftTop() const
    {
      return Point{ x,y };
    }

    Size getSize() const
    {
      return Size{ w, h };
    }

    constexpr Rect operator+(const Point delta)
    {
      return Rect{ x + delta.x, y + delta.y, w, h };
    }

    constexpr Rect operator-(const Point delta)
    {
      return Rect{ x - delta.x, y - delta.y, w, h };
    }

    constexpr Rect& unite(const Rect& other)
    {
      auto r = x + w - 1;                         
      auto b = y + h - 1;                         
      auto orr = other.x + other.w - 1;            
      auto orb = other.y + other.y - 1;            
      auto nx = std::min(x, other.x);             
      auto ny = std::min(y, other.y);             
      auto nw = std::max(r, orr ) + 1 - nx;        
      auto nh = std::max(b, orb) + 1 - ny;
      x = nx;
      y = ny;
      w = nw;
      h = nh;
      return *this;
    }

    constexpr Rect& intersect(const Rect& other)
    {
      auto r = x + w - 1;
      auto b = y + h - 1;
      auto orr = other.x + other.w - 1;
      auto orb = other.y + other.y - 1;
      auto nx = std::max(x, other.x);
      auto ny = std::max(y, other.y);
      auto nr = std::min(r, orr );
      auto nb = std::min(b, orb);
      if (nx > nr) std::swap(nx, nr);
      if (ny > nb) std::swap(ny, nb);
      auto nw = nr - nx + 1;
      auto nh = nb - ny + 1;
      x = nx;
      y = ny;
      w = nw;
      h = nh;
      return *this;
    }

    Rect& operator=(const Point& other)
    {
      x = other.x;
      y = other.y;
      return *this;
    }

    Rect& operator=(const Size& other)
    {
      w = other.w;
      h = other.h;
      return *this;
    }

    Rect& setPos(int xpos, int ypos)
    {
      this->x = xpos;
      this->y = ypos;
      return *this;
    }

    Rect& setPos(const Point& other)
    {
      this->x = other.x;
      this->y = other.y;
      return *this;
    }

    Rect& setSize(int width, int height)
    {
      this->w = width;
      this->h = height;
      return *this;
    }

    Rect& setSize(const Size& size)
    {
      this->w = size.w;
      this->h = size.h;
      return *this;
    }

    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
  };
}
#pragma once

#include <cstdint>
#include <vector>
#include <memory>

namespace tvg
{
  using Unit = float;
  struct Point
  {
    Unit x;
    Unit y;
  };

  struct Lines
  {
    Point start, end;
  };

  struct Rectangle
  {
    Unit x, y, w, h;
  };

  struct Style
  {
    bool flat = true;
    bool linear = true;
    union
    {
      struct {
        uint32_t color_index;
      } flat;
      struct {
        Point point_0;
        Point point_1;
        uint32_t color_index_0;
        uint32_t color_index_1;
      } gradient;
    } type;
  };
  

  typedef struct Segment {
    typedef struct Line {
      Point p2;
    } Line;
    typedef struct Horz {
      Unit pos;
    } Horz;
    typedef struct Vert {
      Unit pos;
    } Vert;
    typedef struct Bezier {
      Point control0;
      Point control1;
      Point point_1;
    } Bezier;
    typedef struct ArcCircle {
      bool large_arc;
      bool sweep;
      uint32_t radius;
      Point target;
    } ArcCircle;
    typedef struct ArcEllipse {
      bool large_arc;
      bool sweep;
      Unit radius_x;
      Unit radius_y;
      Unit rotation;
      Point target;
    } ArcEllipse;
    typedef struct Quad
    {
      Point control;
      Point point_1;
    } Quad;
    typedef union Data
    {
      Line line;
      Horz horz;
      Vert vert;
      Bezier bezier;
      ArcCircle arc;
      ArcEllipse ellipse;
      Quad quad;
    } Data;
    typedef struct Node
    {
      uint8_t type = 6;
      Unit line_width = 1;      
      Data data;
    } Node;

    Point p0 = { 0,0 };
    std::vector<Node> node;
    bool closed = false;
  } Segment;

  struct Path {
    std::vector<Segment> segments;
  };

  class Command
  {
  public:
    virtual ~Command() {}
    uint8_t index;
    Style prim_style;
  };

  class FillPolygon : public Command
  {
  public:
    std::vector<Point> points;
  };

  class FillRectangles : public Command
  {
  public:
    std::vector<Rectangle> rectangles;
  };

  class FillPath: public Command
  {
  public:
    Path path;
  };

  class DrawLines: public Command
  {
  public:
    Unit linewidth;
    std::vector<Lines> lines;
  };

  class DrawLineLoop: public Command
  {
  public:
    Unit linewidth;
    std::vector<Point> points;
  };

  class DrawLineStrip: public Command
  {
  public:
    Unit linewidth;
    std::vector<Point> points;
  };

  class DrawLinePath: public Command
  {
  public:
    Unit linewidth;
    Path path;
  };

  class OutlineFillPolygon: public Command
  {
  public:
    Style sec_style; // line
    Unit linewdith;
    std::vector<Point> points;
  };

  class OutlineFillRectangles: public Command
  {
  public:
    Style sec_style; // line
    Unit linewdith;
    std::vector<Rectangle> rectangles;
  };

  class OutlineFillPath: public Command
  {
  public:
    Style sec_style; // line
    Unit linewdith;
    Path path;
  };

  class Asset
  {
  public:
    bool load(const uint8_t* stream);
    ~Asset();
  private:
    Unit readUnit(const uint8_t*& stream);
    Unit readUnitUnscaled(const uint8_t*& stream);
    uint32_t readVarUInt(const uint8_t*& stream);
    void readColortable(const uint8_t*& stream);
    uint32_t readColorEntry(const uint8_t*& stream);
    bool readCommand(const uint8_t*& stream);
    Style readStyle(uint8_t kind, const uint8_t*& stream);
    Point readPoint(const uint8_t*& stream);
    Rectangle readRectangle(const uint8_t*& stream);
    Path readPath(uint32_t segment_count, const uint8_t*& stream);
    uint16_t readu16(const uint8_t*& stream)
    {
      uint16_t res = *stream++;
      res += (*stream++) << 8;
      return res;
    }

    bool loaded = false;

    uint8_t scale;              // 3:4 (a)
    uint8_t color_encoding;     // 3:2 (b)  
    uint8_t coord_range;        // 3:2 (c)
                                // coord range -> 0=u16, 1=u8, 2=u32

    int32_t width;             // u8, u16 or u32
    int32_t height;            // u8, u16 or u32
    uint32_t color_count;       // VarUInt

    uint32_t* colors = nullptr;

    std::vector<std::unique_ptr<Command>> commands;


  };
}
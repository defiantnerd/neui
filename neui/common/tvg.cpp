#include "tvg.h"
#include <iostream>

/*
 
  .tvg  
  image/tinyvg
  unit is 1/96 dpi

  file is:
        HEADER
        Color Lookup Table
        Command
        Command
        ...
        End of File

*/

namespace neui
{
  namespace tvg
  {
    typedef struct header
    {
      uint8_t magic[2];           // 0,1: { 0x72, 0x56 }
      uint8_t version;            // 2: must be 1

                                  // -> ccbbaaaa
      uint8_t scale;              // 3:4 (a)
      uint8_t color_encoding;     // 3:2 (b)  
      uint8_t coord_range;        // 3:2 (c)
                                  // coord range -> 0=u16, 1=u8, 2=u32

      uint32_t width;             // u8, u16 or u32
      uint32_t height;            // u8, u16 or u32
      uint32_t color_count;       // VarUInt

    } header_t;

    typedef struct clut
    {

    } clut_t;

    typedef struct command
    {

    } command_t;

    typedef struct asset
    {
      header_t header;
      clut_t clut;
      std::vector<command_t*> command;
    } asset_t;


    bool Asset::load(const uint8_t* stream)
    {
      loaded = false;

      // TV version 1?
      if (!(stream[0] == 0x72 && stream[1] == 0x56 && stream[2] == 0x01))
        return false;

      scale = stream[3] & 0b00001111;     // number of fractional bits in Unit value
      color_encoding = (stream[3] & 0b00110000) >> 4;
      coord_range = (stream[3] & 0b11000000) >> 6;

      // now we can read Coords and VarUInts ---------------

      stream += 4;    // where the fun begins

      width = readUnitUnscaled(stream);
      height = readUnitUnscaled(stream);
      color_count = readVarUInt(stream);

      // read the color table
      readColortable(stream);

      // read the commands 
      while (readCommand(stream));

      loaded = true;
      auto n = this->commands.size();
      return true;
    }

    Asset::~Asset()
    {
      delete[] colors;
    }

    Unit Asset::readUnitUnscaled(const uint8_t*& stream)
    {
      int32_t result = 0;
      switch (coord_range)
      {
        case 0:
          result = *reinterpret_cast<const int16_t*>(stream);
          stream += sizeof(int16_t);
          break;
        case 1:
          // s8
          result = *reinterpret_cast<const int8_t*>(stream);
          stream++;
          break;
        case 2:
          // s32

          result = result = *reinterpret_cast<const int32_t*>(stream);
          stream += sizeof(int32_t);
          break;
        default:
          // throw
          break;
      }
      return float(result);
    }

    inline static uint8_t srgb_to_linear(uint8_t value)
    {
      return value;
#if 0
      auto linear = float(value) / 255.0f;
#if 0
      if (linear <= 0.04045f) {
        linear = linear / 12.92f;
      }
      else {
        linear = std::powf((linear + 0.055f) / 1.055f, 2.4f);
      }
      return uint8_t(linear * 255.0f);
#else
      float srgb = pow(linear, /* 1.0f / */ 2.2f);
      return (uint8_t)(srgb * 255.f);
#endif
#endif
    }


    uint32_t Asset::sRGBtoColorSpace(uint32_t argb) {
      uint32_t r = (argb & 0xff000000)
        + (srgb_to_linear((uint8_t)((argb >> 16) & 0xFF)) << 16)
        + (srgb_to_linear((uint8_t)((argb >> 8) & 0xFF)) << 8)
        + (srgb_to_linear((uint8_t)(argb & 0xFF)));
      return r;
    }

    Unit Asset::readUnit(const uint8_t*& stream)
    {
      int32_t result = 0;
      switch (coord_range)
      {
        case 0:
          result = *reinterpret_cast<const int16_t*>(stream);
          stream += sizeof(int16_t);
          break;
        case 1:
          // s8
          result = *reinterpret_cast<const int8_t*>(stream);
          stream++;
          break;
        case 2:
          // s32

          result = result = *reinterpret_cast<const int32_t*>(stream);
          stream += sizeof(int32_t);
          break;
        default:
          // throw
          break;
      }

      float f = float(result) / float(1 << this->scale);
      return f;
    }

    uint32_t Asset::readVarUInt(const uint8_t*& stream)
    {
      uint32_t count = 0;
      uint32_t result = 0;
      while (true) {
        uint8_t byte = *stream++;
        uint32_t val = (byte & 0x7F) << (7 * count);
        result |= val;
        if ((byte & 0x80) == 0)
          break;
        count++;
      }
      return result;
    }

    void Asset::readColortable(const uint8_t*& stream)
    {
      delete[] colors;
      colors = new uint32_t[color_count];

      for (decltype(color_count) i = 0; i < color_count; ++i)
      {
        colors[i] = sRGBtoColorSpace(readColorEntry(stream));
      }
    }

    uint32_t Asset::readColorEntry(const uint8_t*& stream)
    {
      // ARGB
      uint32_t result = 0;

      switch (color_encoding)
      {
        case 0:
          // spec defines entry as RGBA8888
          result |= (*stream++) << 16;
          result |= (*stream++) << 8;
          result |= (*stream++);
          result |= (*stream++) << 24;
          break;
        case 1:
          // spec defines entry as RGB565, find vImageConvert_RGB565toARGB8888 from the iOS Accelerate Framework
          {
            uint32_t w = readu16(stream); // expand to u32 to have room for calculations
            result |= ((((w >> 11) & 31) * 255 + 15) / 31) << 16; // red
            result |= ((((w >> 5) & 63) * 255 + 31) / 63) << 8; // green
            result |= (((w & 31) * 255 + 15) / 31); // blue;
          }
          break;
        case 2:
          {
            auto r = *reinterpret_cast<const float*>(stream); stream += sizeof(float);
            auto g = *reinterpret_cast<const float*>(stream); stream += sizeof(float);
            auto b = *reinterpret_cast<const float*>(stream); stream += sizeof(float);
            auto a = *reinterpret_cast<const float*>(stream); stream += sizeof(float);

            result |= ((int)(r * 255.f)) << 16;
            result |= ((int)(g * 255.f)) << 8;
            result |= ((int)(b * 255.f));
            result |= ((int)(a * 255.f)) << 24;
          }
          break;
        default:
          // throw?
          break;
      }

      return result;
    }

    bool Asset::readCommand(const uint8_t*& stream)
    {
      auto b = *stream++;
      uint8_t commandindex = b & 0x3F;
      uint8_t prim_style = (b >> 6) & 0x3;
      switch (commandindex)
      {
        case 0:
          // eod
          // prim_style should be 0
          // from the spec:
          // Every byte after this command is considered not part of the TinyVG data and can be used 
          // for other purposes like metadata or similar.
          return false; // returning false means: parsing ended, end of document reached
          break;
        case 1:
          // fill polygon
          {
            auto cmd = new FillPolygon();
            cmd->index = commandindex;
            auto pointcount = readVarUInt(stream) + 1;
            cmd->prim_style = readStyle(prim_style, stream);
            cmd->points.reserve(pointcount);
            for (decltype(pointcount) i = 0; i < pointcount; ++i)
            {
              cmd->points.push_back(readPoint(stream));
            }
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 2:
          // fill rectangles
          {
            auto cmd = new FillRectangles();
            cmd->index = commandindex;
            auto rectanglecount = readVarUInt(stream) + 1;
            cmd->prim_style = readStyle(prim_style, stream);
            cmd->rectangles.reserve(rectanglecount);
            for (decltype(rectanglecount) i = 0; i < rectanglecount; ++i)
            {
              cmd->rectangles.push_back(readRectangle(stream));
            }
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 3:
          // fill path
          {
            auto cmd = new FillPath();
            cmd->index = commandindex;
            auto seg_count = readVarUInt(stream) + 1;
            cmd->prim_style = readStyle(prim_style, stream);
            cmd->path = readPath(seg_count, stream);
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 4:
          // draw lines
          {
            auto cmd = new DrawLines();
            cmd->index = commandindex;
            auto line_count = readVarUInt(stream) + 1;
            cmd->prim_style = readStyle(prim_style, stream);
            cmd->linewidth = readUnit(stream);
            cmd->lines.reserve(line_count);
            for (decltype(line_count) i = 0; i < line_count; ++i)
            {
              auto start = readPoint(stream);
              auto end = readPoint(stream);
              cmd->lines.push_back({ start,end });
            }
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 5:
          // draw line loop
          {
            auto cmd = new DrawLineLoop();
            cmd->index = commandindex;
            auto point_count = readVarUInt(stream) + 1;
            cmd->points.reserve(point_count);
            cmd->prim_style = readStyle(prim_style, stream);
            cmd->linewidth = readUnit(stream);
            for (decltype(point_count) i = 0; i < point_count; ++i)
            {
              cmd->points.push_back(readPoint(stream));
            }
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 6:
          // draw line strip
          {
            auto cmd = new DrawLineStrip();
            cmd->index = commandindex;
            auto point_count = readVarUInt(stream) + 1;
            cmd->points.reserve(point_count);
            cmd->prim_style = readStyle(prim_style, stream);
            cmd->linewidth = readUnit(stream);
            for (decltype(point_count) i = 0; i < point_count; ++i)
            {
              cmd->points.push_back(readPoint(stream));
            }
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 7:
          // draw line path
          {
            auto cmd = new DrawLinePath();
            cmd->index = commandindex;
            auto seg_count = readVarUInt(stream) + 1;
            cmd->prim_style = readStyle(prim_style, stream);
            cmd->linewidth = readUnit(stream);
            cmd->path = readPath(seg_count, stream);
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 8:
          // outline fill polygon
          {
            auto cmd = new OutlineFillPolygon();
            cmd->index = commandindex;

            auto b = *stream++;
            uint8_t pointcount = (b & 0x3F) + 1;
            uint8_t sec_style = (b >> 6) & 0x3;

            cmd->prim_style = readStyle(prim_style, stream);
            cmd->sec_style = readStyle(sec_style, stream);
            cmd->linewdith = readUnit(stream);
            cmd->points.reserve(pointcount);
            for (decltype(pointcount) i = 0; i < pointcount; ++i)
            {
              cmd->points.push_back(readPoint(stream));
            }
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 9:
          // outline fill rectangles
          {
            auto cmd = new OutlineFillRectangles();
            cmd->index = commandindex;
            auto b = *stream++;
            uint8_t rectanglecount = (b & 0x3F) + 1;
            uint8_t sec_style = (b >> 6) & 0x3;
            cmd->prim_style = readStyle(prim_style, stream);
            cmd->sec_style = readStyle(sec_style, stream);
            cmd->linewdith = readUnit(stream);
            cmd->rectangles.reserve(rectanglecount);
            for (decltype(rectanglecount) i = 0; i < rectanglecount; ++i)
            {
              cmd->rectangles.push_back(readRectangle(stream));
            }
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 10:
          // outline fill path
                  // outline fill rectangles
          {
            auto cmd = new OutlineFillPath();
            cmd->index = commandindex;
            auto b = *stream++;
            uint8_t seg_count = (b & 0x3F) + 1;
            uint8_t sec_style = (b >> 6) & 0x3;
            cmd->prim_style = readStyle(prim_style, stream);
            cmd->sec_style = readStyle(sec_style, stream);
            cmd->linewdith = readUnit(stream);
            cmd->path = readPath(seg_count, stream);
            commands.push_back(std::unique_ptr<Command>(cmd));
          }
          break;
        case 11:
          // informal spec:
        /*
        * command-name:  "tag"
              command-index: 11
              layout:
                length: varuint
                data:   [length]u8
        */
          break;
        default:
          // unknown thing, but can't read ahead
          // throw?
          return false;
          break;
      }
      return true;
    }

    Style Asset::readStyle(uint8_t kind, const uint8_t*& stream)
    {
      Style result;
      switch (kind)
      {
        case 0:
          result.type.flat.color_index = readVarUInt(stream);
          break;
        case 2:
          result.linear = false;
          [[fallthrough]];
        case 1:
          result.flat = false;
          result.type.gradient.point_0 = readPoint(stream);
          result.type.gradient.point_1 = readPoint(stream);
          result.type.gradient.color_index_0 = readVarUInt(stream);
          result.type.gradient.color_index_1 = readVarUInt(stream);
          break;
        default:
          break;
      }
      return result;
    }

    Point Asset::readPoint(const uint8_t*& stream)
    {
      Point result;
      result.x = readUnit(stream);
      result.y = readUnit(stream);
      return result;
    }

    Rectangle Asset::readRectangle(const uint8_t*& stream)
    {
      Rectangle result;
      result.x = readUnit(stream);
      result.y = readUnit(stream);
      result.w = readUnit(stream);
      result.h = readUnit(stream);
      return result;
    }

    Path Asset::readPath(uint32_t segment_count, const uint8_t*& stream)
    {
      Path path;
      path.segments.resize(segment_count);

      uint32_t segment_lengths[1024];
      if (segment_count > 1024) {
        // throw?
        return Path();
      }

      uint32_t totalnodes = 0;
      for (uint32_t i = 0; i < segment_count; ++i) {
        auto l = readVarUInt(stream) + 1;
        segment_lengths[i] = l;
        totalnodes += l;
      }


      // read all segments and for each one the segment_lengts[segment] number of commands
      for (uint32_t i = 0; i < segment_count; ++i) {

        auto& s = path.segments[i];

        // first read the starting point
        s.p0 = readPoint(stream);

        auto num_nodes = segment_lengths[i];

        // read the segment dommands
        s.node.resize(num_nodes);

        for (uint32_t j = 0; j < num_nodes; ++j) {

          // command instruction and line width, then payload
          auto& node = s.node[j];

          uint8_t p = *stream++;

          node.type = p & 0x7; // u3  (followed by u1 padding)
          bool has_linewidth = (p & 0x10); // u1  (followed by u3 padding)
          if (has_linewidth)
          {
            node.line_width = readUnit(stream);
          }

          // read type specific data
          auto& n = node.data;

          switch (node.type)
          {
            case 0: // line
              n.line.p2 = readPoint(stream);
              break;
            case 1: // horizontal line
              n.horz.pos = readUnit(stream);
              break;
            case 2: // vertical line
              n.vert.pos = readUnit(stream);
              break;
            case 3: // cubic bezier
              n.bezier.control0 = readPoint(stream);
              n.bezier.control1 = readPoint(stream);
              n.bezier.point_1 = readPoint(stream);
              break;
            case 4: // arc circle
              {
                auto b = *stream++;
                n.arc.large_arc = b & 0x01;
                n.arc.sweep = b & 0x02;
                n.arc.radius = readUnit(stream);
                n.arc.target = readPoint(stream);
              }
              break;
            case 5: // arc ellipse
              {
                auto b = *stream++;
                n.ellipse.large_arc = b & 0x01;
                n.ellipse.sweep = b & 0x02;
                n.ellipse.radius_x = readUnit(stream);
                n.ellipse.radius_y = readUnit(stream);
                n.ellipse.rotation = readUnit(stream);
                n.ellipse.target = readPoint(stream);
              }
              break;
            case 6: // close path
              // this is the end.... no data encoded, but actually, this must be the last one, if present.
              // assert(j + 1 == num_nodes);
              s.closed = true;
              break;
            case 7: // quadratic bezier
              n.quad.control = readPoint(stream);
              n.quad.point_1 = readPoint(stream);
              break;
          }
        }
      }

      return path;
    }

  }
}

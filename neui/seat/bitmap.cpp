#include <memory>
#include <map>
#include "bitmap.h"
#include "../common/mujson.h"
#include "wind2d/wic.h"

namespace neui
{

  /*
    bitmap types

    - simple bitmap -> just refer to the bitmap resource name of the PNG/JPG stream
          "logoartwork.png"  which tries to lookup the bitmap via resource fork

    - stitched bitmap
           {bitmap:5bar.png,frames=9}

    - stretched
            {bitmap:background.png,inset:"5,5,5,5"}

    - autoanimated bitmap
            {bitmap:animation.png,frames:5,frametime:100}
            or
            {bitmap:animation?.png,frames:5,frametime:100}    // takes animation1.png to animation5.png and uses them to animate

    {"bitmap":"stuff.png","type":"9","border":"2,2,4,4"}
    {"bitmap":"animation.png","type":"stitch","frames":"5","frametime":"100"}
    */

  class BitmapManager
  {
  public:
    
  private:
    std::map<std::string, D2D1BitmapRef> bitmaps;
  };

  Bitmap::Bitmap()
    : IBitmap()
  {
  }

  void Bitmap::load(const std::string& s)
  {
    load(s.c_str());
  }

  void Bitmap::load(const char* s)
  {
    if (s)
    {
      if (s[0] == '{')
      {
        auto desc = mujson::parse(s);
      }
      else
      {
        // load simply the bitmap
      }
    }

  }

}
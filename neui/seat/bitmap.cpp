#include <memory>
#include <map>
#include "bitmap.h"
#include "../common/mujson.h"
#include "wind2d/base.h"
#include "wind2d/wic.h"
#include "wind2d/seatimpl.h"

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
    BitmapManager()
    {
      // imagePool = new ImagePool(d2dfactories);
    }
    ~BitmapManager()
    {
      delete imagePool;
      // bitmaps.clear();
    }
    void loadBitmap(const std::wstring& name, const std::wstring& filePath)
    {
      imagePool->loadPNGFromFile(name, filePath);
    }
    ID2D1Bitmap* getBitmap(ID2D1DeviceContext* deviceContext, const std::wstring& name)
    {
      ID2D1Bitmap* ref = nullptr;
      auto result = imagePool->ConvertAndCreateLockedBitmap(deviceContext, name, &ref);
      if (ref && result)
        return ref;
      return nullptr;
    }
  private:
    // std::map<std::string, ID2D1Bitmap*> bitmaps;
    win::ImagePool* imagePool = nullptr;
  };

  BitmapManager bitmapManager;

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
        // bitmapManager.
      }
    }

  }

}
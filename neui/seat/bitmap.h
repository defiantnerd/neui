#pragma once

// bitmap.h

#include <string>
#include "../common/geometry.h"

namespace neui
{
#if 0 // remove
  /*
  *   A ITextureRef interface is being used to access the native Texture Object
  */
  class ITextureRef
  {
  public:
    virtual const Size getSize() const = 0;
    virtual ~ITextureRef() = default;
  };

  class Texture
  {
  public:
    const Size getSize() const
    {
      if (impl)
        return impl->getSize();
      return Size();
    }
  protected:
    bool load(const uint8_t* mem, size_t len);
    std::unique_ptr<ITextureRef> impl;
  };

#endif

  class Image;

  class IBitmap
  {
  public:
    virtual bool isAnimated() const = 0;
    virtual bool isMultiFrame() const = 0;
    virtual bool isTiled() const = 0;
    virtual size_t numFrames() const = 0;
    virtual void getImageSource(Image& patch) = 0;
    virtual void getImageSource(Image& patch, size_t frame) = 0;
    virtual void getImageSource(Image& patch, float value) = 0;
    virtual const Size getSize() const = 0;
    virtual ~IBitmap() = default;
  };



  class Bitmap : public IBitmap
  {
  protected:
    Bitmap();
  public:
    ~Bitmap() override = default;
    void load(const std::string& s);
    void load(const char* s);
  };

  class IBitmapManager
  {
  public:
    virtual void setResolution(int dpi, int scale) = 0;
    virtual void keepCache() = 0;
    virtual void flushCache() = 0;
    virtual IBitmap* getBitmap(const char* resource) = 0;


  };

}

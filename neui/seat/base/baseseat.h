#pragma once

#include <memory>
#include <list>
#include "../bitmap.h"
#include "../resource.h"
#include "../seat.h"
#include "common/events.h"
#include "common/ptr.h"
#include "common/boxmodel.h"

namespace neui
{

  class IFrontEnd
  {
  public:
    // if functions are needed for the communication from widget to seat, use this interface
    // virtual void sendEvent(uint32_t widget, const Event& ev) = 0;
    virtual ~IFrontEnd() = default;
  };

  struct ViewHandle
  {
    IFrontEnd* seat = nullptr;
    ISeat::widget_index_t index = 0;
    IEventCallback* eventcallback = nullptr;
    bool wantsEvent(const event::type eventtype);
    void sendEvent(Event& ev);
    bool validateContent(std::string& text, int32_t& caretPos);
    void disconnect();
  };

  // IPlatformView is the interface that all widgets are derived from.
  // it is called from the Seat implementation
  class IPlatformView : public RefCounter
  {
  public:
    virtual void setViewHandle(const ViewHandle& handle) = 0;
    virtual void setParent(RefPtr<IPlatformView> parent) = 0;
    //
    virtual void setAlpha(int percent) = 0;
    virtual float getDpi() = 0;
    virtual void create() = 0;
    virtual void destroy() = 0;
    virtual void show(int show) = 0;
    virtual void hide() = 0;
    virtual void enable() = 0;
    virtual void disable() = 0;
    virtual bool setText(const std::string_view text, int32_t index) = 0;
    virtual bool setInteger(const int32_t value, int32_t index) = 0;
    virtual void focus() = 0;
    virtual void setRect(const Rect& rect) = 0;
    virtual void setBoxModel(const BoxModel& bm) = 0;
    virtual void* getNativeHandle() const = 0;
    virtual void animate() = 0;

    virtual ~IPlatformView() {}
    // virtual void print(std::ostream& out) = 0;
  };

  class ISeatImpl
  {
  public:    
    virtual std::unique_ptr<ITextureRef> loadTexture(const uint8_t* mem, size_t len) = 0;
    virtual ~ISeatImpl() = default;
  };

  class BaseSeat : public ISeat
  {
  public:
    void setResourceProvider(std::shared_ptr<IResourceProvider>& provider) override
    {
      resourceProviders.push_back(provider);
    }
  protected:
    std::list <std::weak_ptr<IResourceProvider>> resourceProviders;
  };
}
#pragma once

#include "common/common.h"
#include "common/boxmodel.h"
#include "common/geometry.h"
#include "common/events.h"
#include "resource.h"

namespace neui
{
 
  class  IEvent
  {
    uint32_t event;

  };
  class ISeatPlug
  {
  public:
    virtual void eventAppTerminate(bool& cancel) {}
    virtual void eventClick(uint32_t widget) {}
    virtual ~ISeatPlug() {}
  };

  class ISeatSession
  {
  public:
    virtual ~ISeatSession() = default;
  };

  class ISeat
  {
  public:
    using widget_type_t = uint32_t;
    using widget_index_t = uint32_t;

    virtual void setResourceProvider(std::shared_ptr<IResourceProvider>& provider) = 0; 
    virtual [[nodiscard]] uint32_t allocate(widget_type_t widgettype, widget_type_t parent, IEventCallback* cb) = 0;
    virtual uint32_t release(widget_index_t widget) = 0;
    virtual uint32_t createWidget(widget_index_t widget) = 0;
    virtual uint32_t destroyWidget(widget_index_t widget) = 0;
    virtual uint32_t show(widget_index_t widget) = 0;
    virtual uint32_t hide(widget_index_t widget) = 0;
    virtual uint32_t setParent(widget_index_t widget, widget_index_t parent) = 0;
    virtual uint32_t setRect(widget_index_t widget, const Rect& pos, int32_t index = 0) = 0;
    virtual uint32_t setBoxModel(widget_index_t widget, const BoxModel& pos) = 0;
    virtual uint32_t setColor(widget_index_t widget, uint32_t color, int32_t index = 0) = 0;
    virtual uint32_t setPos(widget_index_t widget, int size, int32_t index = 0) = 0;
    virtual uint32_t setSize(widget_index_t widget, int size, int32_t index = 0) = 0;
    virtual uint32_t setString(widget_index_t widget, const std::string_view string, int32_t index = 0) = 0;
    virtual uint32_t setInteger(widget_index_t widget, int32_t value, int32_t index = 0) = 0;
    virtual int32_t run() = 0;
    virtual ~ISeat() {}
  };

  class Seat
  {
  public:
    static ISeat& Instance();
  };
}
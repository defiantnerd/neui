#pragma once

#include "../base/baseseat.h"
#include "common/indexedwidgettree.h"
#include <vector>
#include <memory>
#include <deque>

namespace neui
{  
  class wind2dSeat : public BaseSeat, public IFrontEnd
  {
  public:
    wind2dSeat();
    ~wind2dSeat();

    [[nodiscard]] uint32_t allocate(widget_type_t widgettype, widget_type_t parent, IEventCallback* cb) override;
    uint32_t release(widget_index_t widget) override;
    uint32_t createWidget(widget_index_t widget) override;
    uint32_t destroyWidget(widget_index_t widget) override;
    uint32_t show(widget_index_t widget) override;
    uint32_t hide(widget_index_t widget) override;
    uint32_t enable(widget_index_t widget) override;
    uint32_t disable(widget_index_t widget) override;
    uint32_t setParent(widget_index_t widget, widget_index_t parent) override;
    uint32_t setRect(widget_index_t widget, const Rect& pos, int index) override;
    uint32_t setBoxModel(widget_index_t widget, const BoxModel & boxmodel) override;
    uint32_t setColor(widget_index_t widget, uint32_t color, int32_t index) override;
    uint32_t setPos(widget_index_t widget, int size, int32_t index = 0) override;
    uint32_t setSize(widget_index_t widget, int size, int32_t index = 0) override;
    uint32_t setString(widget_index_t widget, const std::string_view string, int32_t index = 0) override;
    uint32_t setInteger(widget_index_t widget, int32_t value, int32_t index = 0) override;
    int32_t run() override;
    int32_t quit(int32_t result = 0) override;


    void destroyChildren(widget_index_t widget);
    // IFrontEnd
    void atExit(std::function<void()> exitfunction)
    {
      atexit.push_back(exitfunction);
    }
  private:
    void initComCtrl32();
    WidgetIndex<IPlatformView> widgets;
    std::deque<std::function<void()>> atexit;
  };


}
#include "widgetbase.h"
#include "seat/seat.h"

namespace neui
{
  void WidgetReference::allocateOnSeat()
  {
    widgetid = Seat::Instance().allocate(type,getParent(),this);
    this->setParentOnChildren(widgetid);
  }

  void WidgetReference::releaseOnSeat()
  {
    Seat::Instance().release(widgetid);
    widgetid = 0;
  }

  void WidgetReference::createOnSeat()
  {
    Seat::Instance().createWidget(widgetid);
  }

  void WidgetReference::destroyOnSeat()
  {
    Seat::Instance().destroyWidget(widgetid);
  }

  void WidgetReference::showOnSeat()
  {
    Seat::Instance().show(widgetid);
  }

  void WidgetReference::hideOnSeat()
  {
    Seat::Instance().hide(widgetid);
  }

  void WidgetReference::selectLevel(SeatInstantiationLevel newlevel)
  {
    if (newlevel > level)
    {
      if (level < SeatInstantiationLevel::allocated)
      {
          allocateOnSeat();
          level = SeatInstantiationLevel::allocated;
          this->setLevelForChildren(SeatInstantiationLevel::allocated);
          if (newlevel == level) return;
      }
      if (level < SeatInstantiationLevel::created)
      {
        updateSeatProperties();
        createOnSeat();
        level = SeatInstantiationLevel::created;
        this->setLevelForChildren(SeatInstantiationLevel::created);
        if (newlevel == level) return;
      }
      if (newlevel == SeatInstantiationLevel::active)
      {
        if (visibleOnSeat) showOnSeat(); else hideOnSeat();
        level = newlevel;
      }
    }
    else
    {
      if (newlevel < level)
      {
        if (level >= SeatInstantiationLevel::created)
        {
          level = SeatInstantiationLevel::allocated;
          destroyOnSeat();
          if (newlevel == level) return;
        }
        if (level >= SeatInstantiationLevel::allocated)
        {
          level = SeatInstantiationLevel::none;
          releaseOnSeat();
          if (newlevel == level) return;
        }
      }
    }
  }
  
  bool WidgetReference::wantsEvent(const event::type eventtype)
  {
    (void)eventtype;
    return false;
  }

  bool WidgetReference::processEvent(const Event& ev)
  {
    // parse and pass to this
    (void)ev;
    return false;
  }

  void WidgetReference::setVisible(bool visible)
  {
    visibleOnSeat = visible;
  }

  void WidgetReference::setBoxModelOnSeat(const BoxModel& boxmodel)
  {
    if (widgetid)
    {
      Seat::Instance().setBoxModel(widgetid, boxmodel);
    }
  }

  void WidgetReference::setRectOnSeat(const Rect& rect)
  {
    if (widgetid)
    {
      Seat::Instance().setRect(widgetid,rect);
    }
  }

  void WidgetReference::setString(const std::string_view text, int32_t index)
  {
    if (widgetid)
    {
      Seat::Instance().setString(widgetid, text, index);
    }
  }

}

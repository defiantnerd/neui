#include "widgetbase.h"
#include "seat/seat.h"

namespace neui
{
  WidgetReference::~WidgetReference()
  {
    selectLevel(SeatInstantiationLevel::none);
  }

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
    if (this->level > SeatInstantiationLevel::none)
      Seat::Instance().show(widgetid);
  }

  void WidgetReference::hideOnSeat()
  {
    if (this->level > SeatInstantiationLevel::none)
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
        if (visibleOnSeat)
        {
          showOnSeat();
          if (!enabled)
          {
            this->setEnable(false);
          }
        }
        else 
          hideOnSeat();
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
  
  void WidgetReference::disconnect()
  {
    selectLevel(SeatInstantiationLevel::none);
  }

  void WidgetReference::invalidate()
  {
    if (visibleOnSeat)
    {
      Seat::Instance().invalidate(widgetid);
    }
  }

  void WidgetReference::setVisible(bool visible)
  {
    if (visibleOnSeat == visible)
      return;

    visibleOnSeat = visible;
    if (level > SeatInstantiationLevel::none)
    {
      if ( visible)
        Seat::Instance().show(widgetid);
      else
        Seat::Instance().hide(widgetid);
    }
  }

  void WidgetReference::setEnable(bool state)
  {
    if (enabled != state)
    {
      enabled = state;
      if (state)
        Seat::Instance().enable(widgetid);
      else
        Seat::Instance().disable(widgetid);
    }
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

  void WidgetReference::setInteger(int32_t value, int32_t index)
  {
    if (widgetid)
    {
      Seat::Instance().setInteger(widgetid, value, index);
    }
  }

}

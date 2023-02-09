#include "commonwidgets.h"

namespace neui
{
#if 0 
  void AppWindow::show()
  {
    this->setType(widgettype::appwindow);
    this->selectLevel(SeatInstantiationLevel::shown);
    //// create children
    //for (auto& c : children)
    //{
    //  if ( c->isVisible() )
    //    c->show();
    //}
  }
  void AppWindow::hide()
  {
    this->setType(widgettype::appwindow);
    this->selectLevel(SeatInstantiationLevel::hidden);
  }
#endif

  void AppWindow::updateSeatProperties()
  {
    WidgetBase::updateSeatProperties();
    setString(this->title);
  }

  void Label::updateSeatProperties()
  {
    WidgetBase::updateSeatProperties();
    setString(this->text);
  }

  void Button::updateSeatProperties()
  {
    WidgetBase::updateSeatProperties();
    setString(this->text);
  }

  void Checkbox::updateSeatProperties()
  {
    super::updateSeatProperties();
    setString(this->text);
    setInteger(this->checked ? 1 : 0, 0);
  }

  void Checkbox::setChecked(bool state)
  {
    _checked = state;
    setInteger(this->checked ? 1 : 0, 0);
  }

  void Checkbox::processEvent(Event& ev)
  {
    if (ev.getType() == event::Selected::type_v)
    {
      auto& e = static_cast<event::Selected&>(ev);
      _checked = (e.index != 0);
    }
    super::processEvent(ev);
  }

  void Text::updateSeatProperties()
  {
    WidgetBase::updateSeatProperties();
    setString(this->text);
  }

  void Droplist::updateSeatProperties()
  {
    WidgetBase::updateSeatProperties();
    // TODO: set droplist texts
    setString(_text, 0);
    int32_t i = 1;
    for (auto&& n : _items.texts())
    {
      setString(n, i++);
    }
    auto curindex = _items.getSelectedIndex();
    if (curindex >= 0)
    {
      this->setInteger(curindex,-1);
    }
  }

  void Droplist::updateItemList(Itemlist* sender)
  {
    updateSeatProperties();
  }

  void Droplist::updateItemIndex(Itemlist* sender)
  {
    auto curindex = _items.getSelectedIndex();
    if (curindex >= 0)
    {
      this->setInteger(curindex, 0);
    }
  }

}


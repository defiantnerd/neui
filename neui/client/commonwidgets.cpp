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
  void Button::processEvent(Event& ev)
  {
    if (ev.getType() == event::type::click)
    {
      this->setText("Button clicked");
    }
    return WidgetBase::processEvent(ev);
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
  }

  void Droplist::updateItemList(Itemlist* sender)
  {
    updateSeatProperties();
  }
}


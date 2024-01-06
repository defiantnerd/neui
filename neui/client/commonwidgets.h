#pragma once

#include "common/widget.h"
#include "common/events.h"
#include "common/contents.h"
#include "common/tvg.h"
#include <string>

namespace neui
{
  using Asset = tvg::Asset;

  //template<typename T, typename... Args>
  //RefPtr<T> make(Args&& ...args)
  //{
  //  // return std::make_shared<T>(std::forward<Args>(args)...);
  //  return RefPtr<T>::make(std::forward<Args>(args)...);    
  //}

  template<typename T, typename... Args>
  auto make(Args&& ...args) -> std::shared_ptr<T>
  {
    return std::make_shared<T>(std::forward<Args>(args)...);
    // return RefPtr<T>::make(std::forward<Args>(args)...);
  }

  class AppWindow : public WidgetBase<AppWindow>, WidgetContainer
  {
  public:
    using WidgetBase::addProperty;
    using WidgetContainer::addProperty;

    AppWindow() = default;
    template <typename... Args,
      typename = enable_if_not_similar<head_t<Args...>, AppWindow>>
      explicit AppWindow(Args&&... args)
      : WidgetBase()
      , WidgetContainer()
    {
      addProperties(std::forward<Args>(args)...);
      show();
    }

    template<class T>
    std::shared_ptr<T> getWidgetById(const std::string_view id)
    {
      return WidgetContainer::getWidgetById<T>(id);
    }
    
    void addProperty(const std::string_view& text) { this->title = text; }
    void setText(const char* text) { this->title = text; setString(text, 0); }

    void updateSeatProperties() override;

    void setParentOnChildren(uint32_t parent) override
    {
      distributeParentToChildren(parent);
    }

    void setLevelForChildren(SeatInstantiationLevel level) override
    {
      distributeLevelToChildren(level);
    }

    widgettype getWidgetType() override { return widgettype::appwindow; }

    //void show() override;
    //void hide() override;
  private:
    std::string title = "Application Window";
  };

  class Panel : public WidgetBase<Panel>, WidgetContainer
  {
  public:
    Panel() = default;
    template <typename... Args,
      typename = enable_if_not_similar<head_t<Args...>, Panel>>
      explicit Panel(Args&&... args)
      : WidgetBase()
      , WidgetContainer()
    {
      addProperties(std::forward<Args>(args)...);
    }

    using WidgetBase::addProperty;

    template <typename T, typename = enable_if_widget<T>>
    void addProperty(T&& w)
    {
      // children.push_back(std::make_shared<std::decay_t<T>>(std::forward<T>(w)));
    }

    template <typename T>
    void addProperty(std::shared_ptr<T>& w)
    {
      // children.push_back(w);
    }
  };

  class Label : public WidgetBase<Label>
  {
  public:
    Label() = default;
    template <typename... Args,
      typename = enable_if_not_similar<head_t<Args...>, Label>>
      explicit Label(Args&&... args)
      : WidgetBase()
    {
      addProperties(std::forward<Args>(args)...);
      this->setType(getWidgetType());
    }

    void setText(const std::string_view& text) { this->text = text; updateSeatProperties(); }

    using WidgetBase::addProperty;

    void addProperty(const std::string_view text) { this->text = text; setString(text, 0);}

    void updateSeatProperties() override;
    widgettype getWidgetType() override { return widgettype::label; }
  private:
    std::string text;
  };

  class Button : public WidgetBase<Button>
  {
  public:
    Button() = default;
    template <typename... Args,
      typename = enable_if_not_similar<head_t<Args...>, Label>>
      explicit Button(Args&&... args)
      : WidgetBase()
    {
      addProperties(std::forward<Args>(args)...);
      this->setType(getWidgetType());
    }

    void setText(const std::string_view& text) { this->text = text; setString(text,0); }

    using WidgetBase::addProperty;

    void addProperty(const std::string_view text) { this->text = text; }
    
    void updateSeatProperties() override;
    widgettype getWidgetType() override { return widgettype::button; }

  private:
    std::string text;
  };

  class Checkbox : public WidgetBase<Checkbox>
  {
  public:
    using super = WidgetBase<Checkbox>;
    Checkbox() = default;
    template <typename... Args,
      typename = enable_if_not_similar<head_t<Args...>, Label>>
      explicit Checkbox(Args&&... args)
      : WidgetBase()
    {
      addProperties(std::forward<Args>(args)...);
      this->setType(getWidgetType());
    }

    void setText(const std::string_view& text) { this->text = text; setString(text, 0); }

    using WidgetBase::addProperty;

    void addProperty(const std::string_view text) { this->text = text; }

    void updateSeatProperties() override;
    widgettype getWidgetType() override { return widgettype::checkbox; }

    const bool& checked = _checked;

    void processEvent(Event& ev) override;

    void setChecked(bool state);
  private:
    std::string text;
    bool _checked = false;
  };

  class Text : public WidgetBase<Text>
  {
  public:
    Text() = default;
    template <typename... Args,
      typename = enable_if_not_similar<head_t<Args...>, Label>>
      explicit Text(Args&&... args)
      : WidgetBase()
    {
      addProperties(std::forward<Args>(args)...);
      this->setType(getWidgetType());
    }

    void setText(const std::string_view& text) { this->text = text; setString(text, 0); }

    using WidgetBase::addProperty;

    void addProperty(const std::string_view text) { this->text = text; }

    void updateSeatProperties() override;
    widgettype getWidgetType() override { return widgettype::text; }

    // void processEvent(Event& ev) override { };
  private:
    std::string text;
  };

  class Droplist : public WidgetBase<Droplist>, public ItemlistOwner
  {
  public:
    Droplist() = default;
    template <typename... Args,
      typename = enable_if_not_similar<head_t<Args...>, Label>>
      explicit Droplist(Args&&... args)
      : WidgetBase()
      , _items(*_itemslistimpl)
    {
      addProperties(std::forward<Args>(args)...);
      this->setType(getWidgetType());
    }

    widgettype getWidgetType() override { return widgettype::droplist; }

    using WidgetBase::addProperty;

    void setText(const std::string_view& text) { _text = text; setString(_text, 0); }

    void addProperty(const std::string_view text) { _text = text; }

    void addProperty(const Itemlist& items)
    {
      this->_items = items;
      updateListOwner();
    }

    void updateSeatProperties() override;

    // void processEvent(Event& ev) override {  };

    Itemlist& items() { return _items; }

    void updateItemList(Itemlist* sender) override;
    void updateItemIndex(Itemlist* sender) override;
  private:
    Itemlist& _items;
    void setIntegerFromSeat(int32_t value, int32_t index) override;
    std::string _text;
  };


  class Slider : public WidgetBase<Slider>
  {
  public:
    Slider() = default;
    template <typename... Args,
      typename = enable_if_not_similar<head_t<Args...>, Label>>
      explicit Slider(Args&&... args)
      : WidgetBase()
    {
      addProperties(std::forward<Args>(args)...);
      this->setType(getWidgetType());
    }

    widgettype getWidgetType() override { return widgettype::slider; }

    using WidgetBase::addProperty;
  private:
    void setIntegerFromSeat(int32_t value, int32_t index) override;
  };

}
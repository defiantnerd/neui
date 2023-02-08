#pragma once

#include "common/widget.h"
#include "common/events.h"
#include "common/contents.h"
#include <string>

namespace neui
{

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

    void processEvent(Event& ev) override;
  private:
    std::string text;
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

    void processEvent(Event& ev) override { };
  private:
    std::string text;
  };

  class Droplist : public WidgetBase<Droplist> , public ItemlistOwner
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
      _items.add("Bla");
      _items.add("Blub");
    }

    widgettype getWidgetType() override { return widgettype::droplist; }

    using WidgetBase::addProperty;

    void setText(const std::string_view& text) { _text = text; setString(_text, 0); }


    void addProperty(const std::string_view text) { _text = text; }

    void updateSeatProperties() override;

    void processEvent(Event& ev) override {  };

    void updateItemList(Itemlist* sender) override;
    Itemlist& _items;
  private:
    std::string _text;
  };

}
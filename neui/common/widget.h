#pragma once

#include "common.h"
#include "widgetbase.h"

namespace neui
{

  // TODO: remove class Widget, it it not useful in the end
  class Widget : public WidgetBase<Widget>, public WidgetContainer
  {
  public:

    Widget() = default;
    template <typename... Args,
      typename = enable_if_not_similar<head_t<Args...>, Widget>>
      explicit Widget(Args&&... args)
      : WidgetBase()
      , WidgetContainer()
    {
      addProperties(std::forward<Args>(args)...);
    }

    using WidgetBase::addProperty;
    using WidgetContainer::addProperty;

#if 0
    template <typename T, typename = enable_if_widget<T>>
    void addProperty(T&& w)
    {
      children.push_back(std::make_shared<std::decay_t<T>>(std::forward<T>(w)));
    }

    template <typename T>
    void addProperty(std::shared_ptr<T>& w)
    {
      children.push_back(w);
    }

    template<class T>
    std::shared_ptr<T> getWidgetById(const std::string_view& id)
    {
      for (auto& c : children)
      {
        if (id == c->getID())
        {
          return std::dynamic_pointer_cast<T>(c);
        }
      }
      return std::shared_ptr<T>();
    }

    template <typename T, typename = enable_if_widget<T>>
    void removeProperty(std::shared_ptr<T> w)
    {
      children.erase(std::remove(children.begin(), children.end(), w));
    }
#endif

  };


}
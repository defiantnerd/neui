#pragma once

#include "common.h"
#include "layoutable.h"
#include <memory>
#include <string>
#include <vector>
#include "ptr.h"
#include <functional>
#include <common/events.h>

namespace neui
{

  struct Id { 
    std::string id;  
    operator const std::string_view() const { return id; }
  };  

  class ISeat;

  class IWidget;
  
  class IWidgetContainer
  {
  public:
    virtual void removeFromContainer(IWidget* widget) = 0;
    virtual ~IWidgetContainer() = default;
  };

  class IWidget
  {
  public:
    virtual bool hasChildren() const = 0;
//     virtual IWidgetContainer* getChildren() const = 0;
    virtual const std::string_view getID() const = 0;
    virtual const Layout& getLayout() const = 0;
    virtual void setParent(IWidgetContainer* container) = 0;

    virtual void show() = 0;
    virtual void hide() = 0;

    virtual bool isVisible() const = 0;

    virtual ~IWidget() = default;
  };

  enum class SeatInstantiationLevel : int
  {
    none = 0,       // not instantiated
    allocated,      // a widget index has been allocated
    created,        // a platform control for a widget index has been created
    active,         // active
  };

  class WidgetContainer;



  class WidgetReference : public IEventCallback
  {
  public:
    friend class WidgetContainer;
    void selectLevel(SeatInstantiationLevel level);

    bool wantsEvent(const event::type eventtype) override;
    void processEvent(Event& ev) override;
  protected:
    void setVisible(bool visible);
    uint32_t getParent() const { return parentwidget; }
    void setBoxModelOnSeat(const BoxModel& boxmodel);
    void setRectOnSeat(const Rect& rect);
    void setString(const std::string_view text, int32_t index = 0);
    void setInteger(int32_t value, int32_t index);

    void setType(widgettype type) { assert(level == SeatInstantiationLevel::none); this->type = type; }
    virtual void updateSeatProperties() = 0;
    void setParent(uint32_t parent)
    {
      parentwidget = parent;
    }
  protected:
    virtual void setParentOnChildren(uint32_t parent) = 0;
    virtual void setLevelForChildren(SeatInstantiationLevel level) = 0;
  private:
    void allocateOnSeat();
    void releaseOnSeat();
    void createOnSeat();
    void destroyOnSeat();
    void showOnSeat();
    void hideOnSeat();

    widgettype type;
    SeatInstantiationLevel level = SeatInstantiationLevel::none;
    bool visibleOnSeat = true;
    ISeat * seat = nullptr;
    uint32_t widgetid = 0;
    uint32_t parentwidget = 0;
  };

  class WidgetContainer : public IWidgetContainer
  {
  public:

    void removeFromContainer(IWidget* widget) override
    {
      for ( auto i = children.begin(); i != children.end(); ++i)      
      {
        if (i->get() == widget)
        {
          children.erase(i);
        }
      }
    }

    template <typename T, typename = enable_if_widget<T>>
    void addProperty(T&& w)
    {
      auto newnode = std::make_shared<std::decay_t<T>>(std::forward<T>(w));
      newnode->setParent(this);
      children.push_back(newnode);
    }

    template <typename T, typename = enable_if_widget<T>>
    void addProperty(const T& w)
    {
      auto newnode = std::make_shared<std::decay_t<T>>(std::forward<T>(w));
      newnode->setParent(this);
      children.push_back(newnode);
    }

    template <typename T, typename = enable_if_widget<T>>
    void addProperty(std::shared_ptr<T>& w)
    {
      w->setParent(this);
      children.push_back(w);
    }

    template <typename T, typename = enable_if_widget<T>>
    void removeProperty(std::shared_ptr<T> w)
    {
      children.erase(std::remove(children.begin(), children.end(), w));
    }

    template<class T>
    std::shared_ptr<T> getWidgetById(const std::string_view id)
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


  protected:
    std::vector<std::shared_ptr<IWidget>> children;
    void foreachChildren(std::function<void(std::shared_ptr<IWidget>& widget)> func)
    {
      for (auto&& c : children)
      {
        func(c);
      }
    }
    void distributeLevelToChildren(SeatInstantiationLevel level)
    {
      for (auto&& c : children)
      {
        auto && p = dynamic_cast<WidgetReference*>(c.get());
        if ( p)
          p->selectLevel(level);
      }
    }
    void distributeParentToChildren(uint32_t parent)
    {
      for (auto& c : children)
      {
        auto&& p = dynamic_cast<WidgetReference*>(c.get());
        if (p)
          p->setParent(parent);
      }
    }
  };


  template <typename T>
  using enable_if_widget = std::enable_if_t<std::is_base_of_v<IWidget, std::decay_t<T>>>;

  template <typename T>
  using enable_if_no_widget = std::enable_if_t<!std::is_base_of_v<IWidget, std::decay_t<T>>>;

  template <typename T>
  using enable_if_widget_has_children = std::enable_if_t<std::is_base_of_v<IWidget, std::decay_t<T>> && std::is_base_of_v<IWidgetContainer, std::decay_t<T>>>;

  template <typename T>
  using enable_if_widget_has_no_children = std::enable_if_t<std::is_base_of_v<IWidget, std::decay_t<T>> && !std::is_base_of_v<IWidgetContainer, std::decay_t<T>>>;


  template <typename S, typename T>
  using enable_if_not_similar = std::enable_if_t<not is_similar_v<S, T>>;

  /*
  *   WidgetBase takes properties from the box model etc.
  */
  template <typename Derived>
  class WidgetBase : public WidgetReference, public IWidget
  {
  public:

    bool hasChildren() const override { return std::is_base_of_v<IWidgetContainer, std::decay_t<Derived>>; }

    void setLevelForChildren(SeatInstantiationLevel level) override 
    {
      assert(!hasChildren()); // if this fails, a derived class with children did not override this function, which is necessary
    }

    void setParentOnChildren(uint32_t parent) override
    {
      assert(!hasChildren()); // if this fails, a derived class with children did not override this function, which is necessary
    }

    void setParent(IWidgetContainer* parent_container) override
    { 
      if (parent)
      {
        parent->removeFromContainer(this);
      }
      parent = parent_container;
    }

    template <typename... Args>
    void addProperties(Args&&... args)
    {
      auto self = static_cast<Derived*>(this);
      (self->addProperty(std::forward<Args>(args)), ...);
    }

    void addProperty(const IHandlerBase& r)
    {
      _event_handler.push_back(r.make_shared());
    }

    void addProperty(std::shared_ptr<IHandlerBase> r)
    {
      _event_handler.push_back(r);
    }

    void addProperty(const Id& newId)
    {
      id = newId;
    }

    auto operator+=(std::shared_ptr<IWidget> t) -> Derived&
    {
      auto self = static_cast<Derived*>(this);
      self->addProperty(t);
      return *self;
    }

    auto operator-=(std::shared_ptr<IWidget> t) -> Derived&
    {
      auto self = static_cast<Derived*>(this);
      self->removeProperty(t);
      return *self;
    }

    void addProperty(Margin const& margin) { layout.boxmodel.margin = margin; }
    void addProperty(Border const& border) { layout.boxmodel.border = border; }
    void addProperty(Padding const& padding) { layout.boxmodel.padding = padding; }
    void addProperty(Rect const &rect) { layout.position = rect; }

    const std::string_view getID() const override { return id; }
    const Layout& getLayout() const override { return layout; }
    bool isVisible() const override { return visible; }

    void show() override
    {
      this->setType(getWidgetType());
      this->setVisible(true);
      this->selectLevel(SeatInstantiationLevel::active);
    }
    void hide() override
    {
      this->selectLevel(SeatInstantiationLevel::none);
      this->setVisible(false);
      this->setType(getWidgetType());
    }    

    void processEvent(Event& ev) override
    {
      ev._sender = this;
      executeEvents(ev);
    }
  protected:
    void executeEvents(event::Base& ev)
    {
      for (auto&& handler : _event_handler)
      {
        handler->execute(ev);
      }
    }
    virtual widgettype getWidgetType() = 0;
    void updateSeatProperties() override
    {
      setBoxModelOnSeat(layout.boxmodel);
      setRectOnSeat(layout.position);
    }
    Id id;
    Layout layout;
    bool visible = true;
    IWidgetContainer* parent = nullptr;
  private:
    std::vector<std::shared_ptr<IHandlerBase>> _event_handler;
    ~WidgetBase() = default;
    friend Derived;
  };

}

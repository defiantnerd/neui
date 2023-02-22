#pragma once

/*
    events.h

    Declares the low level event and eventhandler classes. An event is an object being sent from the seat to the client view.
    The client view will process the event and eventually pass it to an event handler which calls a lambda function.

*/

#include <string>
#include <functional>
#include <memory>
#include "render.h"

namespace neui
{
  class WidgetReference;

  template<typename T>
  class IBinding
  {
  public:
    virtual const T& get() = 0;
    virtual void set(const T& value) = 0;
    virtual void has_changed() = 0;
    operator const T && () = 0;
  };

  namespace event
  {
    enum category : uint32_t
    {
      reserved = 0x0000,
      mousebutton = 0x0100,
      mousemove = 0x0100,
      keys = 0x0200,
      content = 0x0300,
      layout = 0x4000,
      window = 0x5000,
    };

    enum class type : uint32_t
    {
      reserved = 0,

      // content updates
      textupdate = category::content | 0x0001,
      selected   = category::content | 0x0002,

      // mouse events
      click = category::mousebutton | 0x0001,
      down  = category::mousebutton | 0x0002,
      up    = category::mousebutton | 0x0004,

      enter = category::mousemove | 0x0001,
      leave = category::mousemove | 0x0002,
      move  = category::mousemove | 0x0004,

      // keyboard events
      keydown  = category::keys | 0x0001,
      keychar  = category::keys | 0x0002,
      keyup    = category::keys | 0x0004,
      modifier = category::keys | 0x0008,

      // window events
      close =    category::window | 0x0001,
      minimize = category::window | 0x0002,
      maximize = category::window | 0x0004,

      repaint  = category::window | 0x0080,
    };

    // base class
    class Base
    {
    protected:
      Base() = default;
    public:
      WidgetReference* _sender = nullptr;
      virtual type getType() const = 0;
      virtual ~Base() {};
      template<typename T>
      T* sender()
      {
        return dynamic_cast<T*>(_sender);
      }
    };

    // simplified events
    class Clicked : public Base
    {
    public:
      static const type type_v = type::click;
      Clicked(int x, int y, uint32_t flags)
        : x(x), y(y), flags(flags) {}
      type getType() const override { return type_v; }
      const int x = 0;
      const int y = 0;
      const uint32_t flags = 0;
      bool handled = false;
    };

    class Selected : public Base
    {
    public:
      static const type type_v = type::selected;
      Selected(int index, uint32_t flags)
        : index(index), flags(flags) {}
      type getType() const override { return type_v; }
      const size_t index = 0;
      const uint32_t flags = 0;
      bool handled = false;
    };

    class Textupdate : public Base
    {
    public:
      static const type type_v = type::textupdate;
      type getType() const override { return type_v; }
      Textupdate(const std::string_view text, int32_t caretPos, int32_t index, uint32_t flags)
        : text(text), caretPos(caretPos), index(index), flags(flags) {}
      std::string text;
      int32_t caretPos;
      const int32_t index = 0;
      const uint32_t flags = 0;
      bool useContent = false;
    };

    class Paint : public Base
    {
    public:
      static const type type_v = type::repaint;
      Paint(std::shared_ptr<IRenderer> renderer, uint32_t flags)
        : renderer(renderer), flags(flags) {}
      type getType() const override { return type_v; }
      std::shared_ptr<IRenderer> renderer = nullptr;
      const uint32_t flags = 0;
      bool handled = false;
    };
  }

  using Event = event::Base;

  class IEventCallback
  {
  public:
    virtual bool wantsEvent(const event::type eventtype) = 0;
    virtual void processEvent(Event& ev) = 0;
    virtual void disconnect() = 0;
    virtual void setIntegerFromSeat(int32_t value, int32_t index = 0) = 0;
    virtual ~IEventCallback() = default;
  };

  class IHandlerBase
  {
  public:
    virtual event::type getType() const = 0;
    virtual std::shared_ptr<IHandlerBase> make_shared() const = 0;
    virtual void execute(event::Base& ev) = 0;
  };

  template<typename T>
  class Handler : public IHandlerBase
  {
  public:
    event::type getType() const override { return T::type_v; }
    using Args = T&;
    using handler_t = std::function<auto(T& ev)->void>;
    using handler_function = std::function<auto(T& ev)->void>;
    std::shared_ptr<IHandlerBase> make_shared() const override { return std::make_shared<Handler<T>>(*this); }

    Handler(handler_function func)
      : func(func) {}
    void execute(event::Base& ev) override
    {
      if (ev.getType() == T::type_v)
      {
        func(static_cast<T&>(ev));
      }
    }
  private:
    handler_t func;
  };

  using OnClick = Handler<event::Clicked>;
  using OnUpdate = Handler<event::Textupdate>;
  using OnSelect = Handler<event::Selected>;
  using OnPaint = Handler<event::Paint>;

}
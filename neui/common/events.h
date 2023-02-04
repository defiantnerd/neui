#pragma once

/*
    events.h

    Declares the low level event and eventhandler classes. An event is an object being sent from the seat to the client view.
    The client view will process the event and eventually pass it to an event handler which calls a lambda function.

*/

#include <string>
#include <functional>

namespace neui
{
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
      virtual type getType() const = 0;
      virtual ~Base() = default;
    };

    // simplified events
    class Clicked : public Base
    {
    public:
      Clicked(int x, int y, uint32_t flags)
        : x(x), y(y), flags(flags) {}
      type getType() const override { return type::click; }
      int x = 0;
      int y = 0;
      uint32_t flags = 0;
    };

    class Textupdate : public Base
    {
    public:
      type getType() const override { return type::textupdate; }
      const std::string text;
      const int32_t index;
    };
  }

  using Event = event::Base;

  class IEventCallback
  {
  public:
    virtual bool wantsEvent(const event::type eventtype) = 0;
    virtual bool processEvent(const Event& ev) = 0;
    virtual ~IEventCallback() = default;
  };

  template<typename T>
  class Handler
  {
  public:
    using handler_t = std::function<auto(const T& ev)->bool>;
    Handler(std::function<auto(const T& ev)->bool> func)
      : func(func) {}
    bool execute(const event::Base& ev)
    {
      if (ev.getType() == T::getType())
      {
        return func(static_cast<const T&>(ev));
      }
      return false;
    }
  private:
    handler_t func;
  };

  using OnClick = Handler<event::Clicked>;


}
#pragma once

/*
    events.h

    Declares the low level event and eventhandler classes. An event is an object being sent from the seat to the client view.
    The client view will process the event and eventually pass it to an event handler which calls a lambda function.

*/

#include <string>
#include <functional>
#include <memory>

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
      static const type type_v = type::click;
      Clicked(int x, int y, uint32_t flags)
        : x(x), y(y), flags(flags) {}
      type getType() const override { return type_v; }
      int x = 0;
      int y = 0;
      uint32_t flags = 0;
    };

    class Textupdate : public Base
    {
    public:
      static const type type_v = type::textupdate;
      type getType() { return type_v; }
      const std::string text;
      const int32_t index;
      bool useContent = true;
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

  class IHandlerBase
  {
  public:
    virtual event::type getType() const = 0;
    virtual std::shared_ptr<IHandlerBase> make_shared() = 0;
    virtual bool execute(const event::Base& ev) = 0;
  };

  template<typename T>
  class Handler : public IHandlerBase
  {
  public:
    event::type getType() const override { return T::type_v; }
    using Args = const T&;
    using handler_t = std::function<auto(const T& ev)->bool>;
    using handler_function = std::function<auto(const T& ev)->bool>;
    std::shared_ptr<IHandlerBase> make_shared() override { return std::make_shared<Handler<T>>(*this); }

    Handler(handler_function func)
      : func(func) {}
    bool execute(const event::Base& ev) override
    {
      if (ev.getType() == T::type_v)
      {
        return func(static_cast<const T&>(ev));
      }
      return false;
    }
  private:
    handler_t func;
  };

  using OnClick = Handler<event::Clicked>;
  using OnUpdate = Handler<event::Textupdate>;

}
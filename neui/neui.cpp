// neui.cpp : Defines the entry point for the application.
//

#include "neui.h"

#include "common/widget.h"
#include "common/mujson.h"
#include "common/indexedwidgettree.h"
#include "seat/base/baseseat.h"
// #include "detail/ptr.h"

namespace neui
{
  /* the layout algo test area */

  class LayoutElement : public ILayoutable
  {
  public:
    LayoutElement(const Rect& r)
      : layoutedPosition(r)
    {}
    ~LayoutElement() override = default;
    const Size getSize() const override;
    void setPosition(const Rect& rect) override;
  private:
    Rect layoutedPosition;
  };
  
  void LayoutWrapVertical(std::vector<LayoutElement>& list)
  {
    Size targetSize{ 200,200 };
    int currX{ 0 };
    int maxheight{ 0 };
    int currY{ 0 };
    for (auto& elem : list)
    {
      auto elementSize = elem.getSize();
      if (currX + elementSize.w > targetSize.w)
      {
        // carriage return
        currY += maxheight;
        currX = 0;
        maxheight = 0;
      }
      Rect newpos{ currX, currY, elementSize.w, elementSize.h };
      elem.setPosition(newpos);
      currX += elementSize.w;
      maxheight = std::max(maxheight, elementSize.h);
    }
  }

  void LayoutWrapHorizontal(std::vector<LayoutElement>& list)
  {
    Size targetSize{ 200,200 };
    int currY{ 0 };
    int maxwidth{ 0 };
    int currX{ 0 };
    for (auto& elem : list)
    {
      auto elementSize = elem.getSize();
      if (currY + elementSize.h > targetSize.h)
      {
        // carriage return
        currX += maxwidth;
        currY = 0;
        maxwidth = 0;
      }
      Rect newpos{ currX, currY, elementSize.w, elementSize.h };
      elem.setPosition(newpos);
      currY += elementSize.h;
      maxwidth = std::max(maxwidth, elementSize.w);
    }
  }

  const Size LayoutElement::getSize() const
  {
    return { layoutedPosition.getSize() };
  }

  void LayoutElement::setPosition(const Rect& rect)
  {
    layoutedPosition = rect;
  }

  int run()
  {
    return Seat::Instance().run();
  }

#if 0 
  //class IPlatformView : public RefCounter
  //{
  //public:
  //  virtual ~IPlatformView() {}
  //  virtual void print(std::ostream& out) = 0;
  //};

  class Widg : public IPlatformView
  {
  public:
    Widg(int some) : IPlatformView(), k(some) {}
    void print(std::ostream& out) override
    {
      out << "=" << k;
    }
    int k = 5;
  };

  void foo()
  {
    {
      {
        WidgetIndex<IPlatformView> widgets;
        auto xx = RefPtr<Widg>::make(1);
        auto w1 = widgets.add(xx);
        auto w2 = widgets.add(RefPtr<Widg>::make(2));
        widgets.print(std::cout);
        widgets.remove(w1);
        widgets.remove(w2);
        widgets.print(std::cout);
      }

      {
        WidgetIndex<IPlatformView> widgets;
        auto w1 = widgets.add(RefPtr<Widg>::make(1));
        auto w2 = widgets.add(RefPtr<Widg>::make(2));
        auto w3 = widgets.add(RefPtr<Widg>::make(3));
        widgets.print(std::cout);
        widgets.remove(w3);
        widgets.remove(w1);
        widgets.remove(w2);
        widgets.print(std::cout);
      }

      {
        WidgetIndex<IPlatformView> widgets;
        auto w1 = widgets.add(RefPtr<Widg>::make(1));
        auto c1 = widgets.add(w1, std::make_shared<Widg>(2));
        auto c2 = widgets.insertafter(c1, RefPtr<Widg>::make(3));
        auto c3 = widgets.insertbefore(c1,RefPtr<Widg>::make(3));
        auto c4 = widgets.insertbefore(c2,RefPtr<Widg>::make(3));
        widgets.print(std::cout);
        widgets.remove(c2);
        widgets.print(std::cout);
        c2 = widgets.insertbefore(c4, RefPtr<Widg>::make(2));
        widgets.print(std::cout);
        widgets.remove(c3);
        widgets.print(std::cout);
        widgets.remove(c1);
        widgets.print(std::cout);
        widgets.remove(c4);
        widgets.print(std::cout);
        widgets.remove(c2);
        widgets.print(std::cout);
        widgets.remove(w1);
        widgets.print(std::cout);
      }
    }

     auto bitmap = "{src:faderstack.png,frames:9,animated:true}";
    //auto k = fmt::format("\{content:slot,slot:3,name:\"slotname\"\}", 3);
    //auto l = mujson::parse(k);
    auto p1 = mujson::parse("{key:value,foo:bar,rect:\"1,2,3,4\"}");
    auto p2 = mujson::parse("{\"key\":\"value\",\"foo\":\"bar\"}");    


    std::cout << "neui here.." << std::endl;

    std::vector<LayoutElement>  elems = { LayoutElement{ {0,0,60,40 }}, LayoutElement{ {0,0,60,40 }}, LayoutElement{ {0,0,60,40 }}, LayoutElement{ {0,0,60,40 }}, LayoutElement{ {0,0,30,40 }} };

    LayoutWrapVertical(elems);

    LayoutWrapHorizontal(elems);


  }
#endif



}

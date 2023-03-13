
#include <Windows.h>
#include <iostream>
#include "neui\neui.h"

using std::cout;
using std::endl;

int run();

int _stdcall WinMain(
  _In_ HINSTANCE hInstance,
  _In_opt_ HINSTANCE hPrevInstance,
  _In_ LPSTR     lpCmdLine,
  _In_ int       nShowCmd
)
{
  return run();
}

int main(int argc, char* argv[])
{
  return run();
}

int run()
{

  using namespace neui;
  // neui::foo();

  //class Style
  //{
  //public:
  //};
  //Style mystyle{
  //  Size{15,40},
  //  Font{"Segoe UI"},
  //  FontSize(18)
  //};

  // the droplist
  std::shared_ptr<Droplist> droplist;

  droplist = make<Droplist>("Selection", Id{ "mydroplist" }
    , Rect{ 10,110,300,20 },
    OnSelect([](OnSelect::Args e)->void
      {
        auto self = e.sender<Droplist>();

        if (self)
        {
          auto cid = self->getID();
          // OutputDebugStringA(std::string(cid).c_str());
        }
        e.handled = true;
      }
    )
    );

  auto checkerbox = make<Checkbox>("My Option",
    Id{"the checkbox"},
   Rect{ 10,130,120,20 },
    OnSelect{ [](OnSelect::Args e)
      {
        auto sender = e.sender<Checkbox>();
        e.handled = true;
      }
    }
  );

  auto blaba = make<Label>("First Static", Rect{ 10,10,300,20 });

  OnClick klci([droplist, checkerbox](OnClick::Args e)->void
    {
      // add an additional item
      if (droplist->_items.count() < 4)
      {
        droplist->_items.add("Zonk");
      }

      // select an entry (-> note that change events are sent)
      droplist->_items.setSelectedIndex(1);

      // invert check box (-> note that change events are sent) 
      checkerbox->setChecked(!checkerbox->checked);
      e.handled = true;
      // neui::quit();
    }
  );

  blaba->setText("bla");

  auto window = make<AppWindow>(
    "My Application Window",
    Rect{ 250,250,700,450 }
    , Border{ 20 }
    , Id{ "mainwindow" }
    // ,Button{"Clickme"}
    , blaba
    , Label{ "Second Static Text Label", Id{"second"}, Rect{10,40,300,20}, klci }
    , Text{ "Write Something", Id{"editfield"}, Rect{10,70,300,20} }
    , droplist
    , checkerbox
    , Button{ "Click me", Rect(10,160,120,30), klci }
    , Button{ "On/Off", Id{"togglebutton"}  , Rect(140,160,120,30)}
    , Label{ "XYZ",Id{"Painter"}, Rect{10,200,100,100}
        ,OnPaint{[&](OnPaint::Args e) {
            e.renderer->begin()
              .push()
              .pen(0x0000ff)
              .line(Point(10, 10), Point(90, 90))
              .pen(0xff8000)
              .line(Point(10, 90), Point(90, 10))
              .pop()
              .line(Point(10,50), Point(90,50))
              .rect(Rect{10,30,80,10},3)
              .push()
              .translate(Point{50,50});
              // .translate({60,35})
#if 0
            for (int i = 0; i < 50; i++)
              e.renderer->
              rotate(Point(0, 0), 2)
              .pen(0x000000+(0xff-i*8)+((i*8)<<8)<<8)
              .rect(Rect{ -20-i,-20-i,40+i*2,40+i*2 }, 4);
#endif
            e.renderer->circle(Point(0, 0), 30);
            e.renderer->pop().end();
            e.handled = true;
          }
         }
    }
  );

  auto tog = window->getWidgetById<Button>("togglebutton");
  tog->addProperty(
    OnClick([&](OnClick::Args e)
      {
        auto sender = e.sender<Button>();
        if (checkerbox->isEnabled())
        {
          checkerbox->disable();
        }
        else
        {
          checkerbox->enable();
        }
        auto p = window->getWidgetById<Label>("Painter");
        if (p)
          p->setText("bllsdfl");
        droplist->_items.set({"a","b","c"});
      })
  );

  // get the shared pointer to an object by ID - you must provide a Class Type
  auto mc = window->getWidgetById<Checkbox>("the checkbox");
  mc->hide();
  mc->setChecked(true);
  mc->show();
  
  auto r = neui::run();  
  //window->hide();
  //window.reset();
  return r;
}

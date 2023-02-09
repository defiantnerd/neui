
#include <Windows.h>
#include <iostream>
#include "neui\neui.h"

using std::cout;
using std::endl;

int run();

int _stdcall WinMain(
  HINSTANCE hInstance,
  HINSTANCE hPrevInstance,
  LPSTR     lpCmdLine,
  int       nShowCmd
)
{
  run();
}

int main(int argc, char* argv[])
{
  run();
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

  auto checkerbox = make<Checkbox>("My Option", Rect{ 10,130,120,20 },
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

  );

  // get the shared pointer to an object by ID - you must provide a Class Type
  auto mc = window->getWidgetById<IWidget>("editfield");
  mc->hide();

  mc->show();
  
  return neui::run();

  return 0;
}

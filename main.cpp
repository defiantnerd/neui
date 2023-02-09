
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


auto myfunc(int arg) -> bool
{
  return false;
}

int run()
{

  cout << "Hello CMake." << endl;


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

  droplist = make<Droplist>( "Selection", Id{ "mydroplist" }
    , Rect{ 10,110,300,20 } , 
    OnSelect ([](OnSelect::Args e)->void
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
      droplist->_items.add("Zonk");
      droplist->_items.setSelectedIndex(1);
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
  return neui::run();  
#if 0
  auto sharedone = std::make_shared<Widget>(Rect(40, 2, 20, 20));
  auto sharedpanel = std::make_shared<Panel>(Rect(70, 2, 20, 20), Label{ "label in Panel" });
  std::cout << "panel: " << sharedpanel->hasChildren() << std::endl;
  
  *sharedpanel += std::make_shared<Label>("Another one bytes the panel!");
  Widget w{ Rect(10, 10, 100, 100),
    Border{2},
    // mystyle,
    Widget(Rect(10,2,20,20)),
    sharedone,
    Label{Rect(40,32,50,52),Id{"text"}, Border{1}, Margin{2},"Lolo"},
    Label{"label"},
    sharedpanel
  };

  Label testlabel{ Rect(40,32,50,52),Id{"text"}, Border{1}, Margin{2},"Lolo" };
  (*sharedpanel) += std::make_shared<Label>("blub");
  std::cout << "label: " << testlabel.hasChildren() << std::endl;
  auto p = w.getWidgetById<Label>("text");
  auto q = w.getWidgetById<Label>("unknown");

  if (p)
  {
    p->setText("Lalala");
  }

  w += std::make_shared<Label>("another one to the widget");
	
  auto c = R"( 

  Widget
    Border()
    Widget
    Label
      Rect()
      Border()
      Margin()
      "Lolo"
    Label
    &sharedone


  )";
#endif
	
	return 0;
}

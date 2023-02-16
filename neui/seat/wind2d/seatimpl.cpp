#include "seatimpl.h"
#define NOMINMAX 1
#include <windowsx.h>
#include "seatimpl.h"
#include "common/common.h"
#include "seatcontrols.h"
#include "staticcontrols.h"
#include "appwindow.h"
#include <CommCtrl.h>

#pragma comment(lib, "Comctl32")

#if 1
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace neui
{
#if 0 // see common.h
  constexpr uint32_t magic(const char n[5])
  {
    if constexpr (endian::native == endian::little)
    {
      return n[0] | n[1] << 8 | n[2] << 16 | n[3] << 24;
    }
    if constexpr (endian::native == endian::little)
    {
      return n[1] | n[2] << 8 | n[1] << 16 | n[0] << 24;
    }
    return 0;
  }
  
  enum widgettype : uint32_t
  {
    appwindow = magic("wapp"),
    pluginwindow = magic("wplg"),
    panel = magic("cpnl"),
    label = magic("clbl"),
    text = magic("cedt"),
    button = magic("cbtn"),
    radiobutton = magic("crad"),
    checkbox = magic("cchk"),
    dropbutton = magic("cdrp"),
    droplist = magic("cdrl"),
    toggle = magic("ctgl"),
    progressbar = magic("cbar"),
    slider = magic("csld"),
    bitmap = magic("cbmp"),
    canvas = magic("ccan"),
    // accordion
    none = magic("none")
  };
#endif 

  namespace wind2d
  {
    HFONT gDefaultFont = 0;
    HFONT DefaultFont()
    {
      return gDefaultFont;
    }

    static std::atomic_int32_t gInstances = 0;
  }

  wind2dSeat::wind2dSeat()
    : BaseSeat()
    , IFrontEnd()
  {
    if (wind2d::gInstances == 0)
    {
      initComCtrl32();
      wind2d::gDefaultFont = ::CreateFont(-11, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, _T("sans serif"));
    }
    ++wind2d::gInstances;
  }
  
  void wind2dSeat::initComCtrl32()
  {
    INITCOMMONCONTROLSEX p;
    p.dwSize = sizeof(p);
    p.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_TREEVIEW_CLASSES | ICC_USEREX_CLASSES | ICC_TREEVIEW_CLASSES;
    ::InitCommonControlsEx(&p);
  }

  wind2dSeat::~wind2dSeat()
  {
    wind2d::gInstances--;
    if (wind2d::gInstances == 0)
    {
      for (auto i = atexit.rbegin(); i != atexit.rend(); ++i)
      {
        (*i)();
      }
      atexit.clear();
      ::DeleteFont(wind2d::gDefaultFont);
    }
  }

  uint32_t wind2dSeat::allocate(widget_type_t widgettype, widget_index_t parent, IEventCallback* cb)
  {
    RefPtr<IPlatformView> view;
    // TODO: this switch should be a factory function where additional seat based control types can be registered
    switch (widgettype)
    {
      case widgettype::appwindow:
        view = RefPtr<wind2d::AppWindow>::make();
        break;
      case widgettype::label:
        view = RefPtr<wind2d::Label>::make();
        break;
      case widgettype::button:
        view = RefPtr<wind2d::Button>::make();
        break;
      case widgettype::checkbox:
        view = RefPtr<wind2d::Checkbox>::make();
        break;
      case widgettype::text:
        view = RefPtr<wind2d::TextField>::make();
        break;
      case widgettype::droplist:
        view = RefPtr<wind2d::Droplist>::make();
        break;
      case widgettype::panel:
        return 0;
        break;
      default:
        return 0;
    }
    if (view)
    {
      widget_index_t r = 0;
      if (parent != 0)
      {
        r = this->widgets.add(parent, view);
        view->setParent(this->widgets[parent]);
      }
      else
      {
        r = this->widgets.add(view);
      }
      view->setViewHandle({ this, r, cb });
      return r;
    }
    return 0;
  }

  uint32_t wind2dSeat::release(uint32_t widget)
  {
    auto&& w = widgets[widget];
    auto child = widgets.getfirstchild(widget);
    while (child)
    {
      auto n = child;
      child = widgets.getnextsibling(child);
      destroyWidget(n);
    }
    // TODO: actually, everything should be handled via the widget tree, check if it is the case    
    widgets.remove(widget);
    return 0;
  }

  uint32_t wind2dSeat::createWidget(widget_index_t widget)
  {
    widgets[widget]->create();    
    return 0;
  }

  uint32_t wind2dSeat::destroyWidget(widget_index_t widget)
  {
    auto w = widgets[widget];
    if (w)
    {
      destroyChildren(widget);
      w->destroy();
      return 0;
    }
    return 1;
  }

  uint32_t wind2dSeat::show(widget_index_t widget)
  {
    widgets[widget]->show(1);
    return 0;
  }

  uint32_t wind2dSeat::hide(widget_index_t widget)
  {
    widgets[widget]->hide();
    return 0;
  }

  uint32_t wind2dSeat::enable(widget_index_t widget)
  {
    widgets[widget]->enable();
    return 0;
  }

  uint32_t wind2dSeat::disable(widget_index_t widget)
  {
    widgets[widget]->disable();
    return 0;
  }

  uint32_t wind2dSeat::setParent(widget_index_t widget, widget_index_t parent)
  {
    auto&& c = widgets[widget];
    auto&& p = widgets[parent];
    widgets.add(parent, c);
    return 0;
  }

  uint32_t wind2dSeat::setRect(widget_index_t widget, const Rect& pos, int index)
  {
    if (widgets[widget])
    {
      widgets[widget]->setRect(pos);
      return 0;
    }
    return 1;
  }

  uint32_t wind2dSeat::setBoxModel(widget_index_t widget, const BoxModel& boxmodel)
  {
    if (widgets[widget])
    {
      widgets[widget]->setBoxModel(boxmodel);
      return 0;
    }
    return 1;
  };

  uint32_t wind2dSeat::setColor(widget_index_t widget, uint32_t color, int index)
  {
    return 0;
  }
  uint32_t wind2dSeat::setPos(widget_index_t widget, int size, int index)
  {
      return uint32_t();
  }

  uint32_t wind2dSeat::setSize(widget_index_t widget, int size, int32_t index)
  {
    return 0;
  }

  uint32_t wind2dSeat::setString(widget_index_t widget, const std::string_view string, int32_t index)
  {
    widgets[widget]->setText(string, index);
    return 0;
  }

  uint32_t wind2dSeat::setInteger(widget_index_t widget, int32_t value, int index)
  {
    widgets[widget]->setInteger(value, index);
    return 0;
  }

  int32_t wind2dSeat::run()
  {    
    MSG message = { 0 };
    do
    {
      TranslateMessage(&message);
      DispatchMessage(&message);
    } while (GetMessage(&message, NULL, 0, 0) > 0);

    return (int32_t)message.wParam;
  }

  int32_t wind2dSeat::quit(int32_t result)
  {
    ::PostQuitMessage(result);
    return 0;
  }

  void wind2dSeat::destroyChildren(widget_index_t widget)
  {
    // children must be destroyed and disconnected, too
    auto child = widgets.getfirstchild(widget);
    while (child)
    {
      auto n = child;
      child = widgets.getnextsibling(n);
      destroyChildren(n);
      auto w = widgets[n];
      w->destroy();
      widgets.remove(n);
    }
  }

}

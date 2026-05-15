#include "oui.h"
#define WINDOWS_LEAN_AND_MEAN 1
#include <Windows.h>
#include <string>


host_ui_t* Host::getUIExtension()
{

  return &_ext;
}

void Plugin::init(Host* host)
{
  _host = host;
}

void Plugin::terminate()
{
}

void Plugin::open()
{

  auto ext = _host->getUIExtension();
  // create a session
  _session = ext->create_session(ext, 640, 480, this, this->getUIExtension() );

  // get the widgets interface
  const auto& widgets = _session->widgets;

  auto root = widgets->get_root(_session);
  // set the root to 600,300
  widgets->set_size(_session, root, 600, 300);

  // add a label (type as label or uint32_t !?!?)
  auto label = widgets->create(_session, root, "label", 10, 10, 100, 20, nullptr);

  auto group = widgets->create(_session, root, "group", 80, 40, 300, 200, nullptr);
  _buttons[0] = widgets->create(_session, group, "button1", 50, 50, 80, 15, &_buttons[0]);
  _buttons[1] = widgets->create(_session, group, "button2", 50, 70, 80, 15, &_buttons[1]);
  _buttons[2] = widgets->create(_session, group, "button3", 50, 90, 80, 15, &_buttons[2]);

  auto myasset = _session->assets->from_identifier(_session, "something.png");

  for (auto& i : _buttons) {
    widgets->set_emit_events(_session, i, true);
  }

  widgets->layer->add_text(_session, _buttons[0], 1, "Schnonk", 2, 0);
  widgets->layer->add_text(_session, _buttons[1], 2, "Scnnink", 2, 0);
  widgets->layer->add_text(_session, _buttons[1], 3, "_------", 2, 0);
  widgets->layer->add_text(_session, _buttons[2], 1, "New Schnonk", 2, 0);
  widgets->layer->clear(_session, group);
  //widgets->remove(_session, label);
  //// todo: reaction to events
  //label = widgets->create(_session, root, "label", 10, 10, 100, 20);
  //widgets->remove(_session, button);
  // widgets->remove(_session, button2);
  //ext->destroy(ext, _session);
}

void Plugin::close()
{
  auto ext = _host->getUIExtension();
  ext->destroy(ext, _session);
}

bool Plugin::event(const event_t* ev, void* userdata)
{
  if (ev->event == 1)
  {
    auto& c = ev->data.click;
    if (userdata)
    {
      auto wgt = (widget_t*)(userdata);
      size_t a = wgt - _buttons;
      char buff[256];
      snprintf(buff, sizeof(buff), "Button %zu at %d/%d\n", a, ev->data.click.x, ev->data.click.y);
      switch (a) {
      case 0:
        // button 1;
        OutputDebugStringA(buff);
        break;
      case 1:
        OutputDebugStringA(buff);
        break;
      case 2:
        OutputDebugStringA(buff);
        break;
      default:
        break;
      }
    }

    
  }
  return false;
}

void Plugin::release_cookie(void* userdata)
{
  // would release a userdata
  OutputDebugStringA("releasing a userdata\n");
}

client_ui_t* Plugin::getUIExtension()
{
  return &_uiext;
}

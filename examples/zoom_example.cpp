// UI zoom demo (NEUI_ATTR_UI_SCALE).
//
// Three buttons switch the frame between 100 % / 150 % / 200 %. Everything in
// the window - text, borders, the knob, the custom-drawn panels - scales, and
// NONE of the layout code below knows about it: every coordinate here is
// logical (96 DPI base) and written once.
//
// The two CUSTOMDRAW panels differ in exactly one attribute, to show the
// choice a client has:
//   * left  - default. Draws in logical units; the framework scales the
//             result. Its hairline grid is drawn at 1 logical unit, so at
//             200 % it becomes 2 physical px (and can land between pixels).
//   * right - NEUI_ATTR_PAINT_DEVICE_PIXELS. Draws in device pixels, so its
//             grid stays exactly 1 physical px crisp at every zoom - what a
//             meter / analyser / waveform view wants.
// Both print the scale they were handed, so you can see what each receives.

#include <neui/neui.h>

#include <cstdio>
#include <cstring>
#include <cmath>

namespace {

struct App {
  neui_session_t     sess{};
  neui_widget_api_t* w     = nullptr;
  neui_attr_api_t*   attrs = nullptr;

  neui_widget_t frame{};
  neui_widget_t btn100{}, btn150{}, btn200{};
  neui_widget_t knob{};
  neui_widget_t panel_logical{}, panel_device{};
  neui_widget_t readout{};
};

App g_app;

void set_zoom(float z)
{
  g_app.attrs->set_float(g_app.sess, g_app.frame, NEUI_ATTR_UI_SCALE, z);
  char buf[128];
  std::snprintf(buf, sizeof buf, "NEUI_ATTR_UI_SCALE = %.0f%%", z * 100.0f);
  g_app.w->set_text(g_app.sess, g_app.readout, buf);
}

// Grid + diagonal + a 1-unit hairline border. Identical code for both panels;
// only the widget's PAINT_DEVICE_PIXELS attr changes what a "unit" means.
void paint_panel(neui_event_paint_t& p, bool device_px)
{
  auto* pa = p.painter_api;
  auto* h  = p.p;

  pa->fill_rect(h, 0, 0, p.width, p.height, device_px ? 0xFF102030u : 0xFF301020u);

  // One-unit hairline. In logical mode this is 1 logical unit (scale physical
  // px once zoomed); in device mode it is exactly one physical pixel.
  const float step = device_px ? 16.0f * p.scale : 16.0f;
  for (float x = 0; x < p.width; x += step)
    pa->fill_rect(h, x, 0, 1.0f, p.height, 0x40FFFFFFu);
  for (float y = 0; y < p.height; y += step)
    pa->fill_rect(h, 0, y, p.width, 1.0f, 0x40FFFFFFu);

  pa->draw_rect(h, 0, 0, p.width, p.height, 1.0f, 0xFFFFFFFFu);

  char buf[96];
  std::snprintf(buf, sizeof buf, "%s  scale %.2f  %.0fx%.0f",
                device_px ? "device px" : "logical", p.scale, p.width, p.height);
  // Text position and size follow the space we were handed, so the label stays
  // the same apparent size in both modes.
  const float u = device_px ? p.scale : 1.0f;
  pa->draw_text(h, 6.0f * u, 6.0f * u, p.width, p.height,
                buf, 11.0f * u, 0xFFFFFFFFu);
}

bool NEUI_ABI onevent(void* /*token*/, neui_event_t* ev)
{
  switch (ev->type) {
    case NEUI_EVENT_MOUSE_BUTTON_CLICK: {
      uint32_t id = ev->data.mouse.widget.id;
      if (id == g_app.btn100.id) { set_zoom(1.00f); return true; }
      if (id == g_app.btn150.id) { set_zoom(1.50f); return true; }
      if (id == g_app.btn200.id) { set_zoom(2.00f); return true; }
      break;
    }
    case NEUI_EVENT_WIDGET_PAINT: {
      uint32_t id = ev->data.paint.widget.id;
      if (id == g_app.panel_logical.id) { paint_panel(ev->data.paint, false); return true; }
      if (id == g_app.panel_device.id)  { paint_panel(ev->data.paint, true);  return true; }
      break;
    }
    case NEUI_EVENT_METRICS_CHANGED:
      // Fired by the zoom change. A client that wants to RE-LAY-OUT for zoom
      // (rather than just be scaled) would do it here; this demo only logs,
      // because scaling is exactly what we want.
      std::printf("METRICS_CHANGED: ui_scale = %.2f\n", ev->data.metrics.ui_scale);
      return true;
    case NEUI_EVENT_APP_QUIT:
      return true;
    default: break;
  }
  return false;
}

neui_widget_client_t g_widget_client = { NEUI_VERSION, nullptr, onevent };

void* NEUI_ABI get_iface(void* /*token*/, const char* name)
{
  if (!std::strcmp(name, NEUI_API_WIDGETS)) return &g_widget_client;
  return nullptr;
}

neui_client_t g_client = { NEUI_VERSION, get_iface };

} // namespace

int main()
{
  neui_init();
  // The zoom is a crossplatform-host feature (the native hosts' children are
  // real OS controls), so select it explicitly rather than taking the default.
  neui_api_t* api = neui_get_api("neui.host.crossplatform");
  if (!api) { std::printf("no crossplatform host\n"); return 1; }

  g_app.sess  = api->create_session(&g_client, nullptr);
  g_app.w     = (neui_widget_api_t*)api->get_interface(g_app.sess, NEUI_API_WIDGETS);
  g_app.attrs = (neui_attr_api_t*)  api->get_interface(g_app.sess, NEUI_API_ATTRS);

  auto* w = g_app.w;

  // Content is laid out once, in logical px, with a 12 px margin. 460x300
  // holds it with room at the right/bottom edges.
  g_app.frame = w->create(g_app.sess, widget_none, NEUI_W_APPWINDOW,
                          100, 100, 460, 300, nullptr);
  w->set_text(g_app.sess, g_app.frame, "neui - UI zoom");

  g_app.btn100 = w->create(g_app.sess, g_app.frame, NEUI_W_BUTTON, 12, 12, 90, 28, nullptr);
  g_app.btn150 = w->create(g_app.sess, g_app.frame, NEUI_W_BUTTON, 110, 12, 90, 28, nullptr);
  g_app.btn200 = w->create(g_app.sess, g_app.frame, NEUI_W_BUTTON, 208, 12, 90, 28, nullptr);
  w->set_text(g_app.sess, g_app.btn100, "100%");
  w->set_text(g_app.sess, g_app.btn150, "150%");
  w->set_text(g_app.sess, g_app.btn200, "200%");

  g_app.readout = w->create(g_app.sess, g_app.frame, NEUI_W_LABEL, 306, 16, 140, 22, nullptr);
  w->set_text(g_app.sess, g_app.readout, "NEUI_ATTR_UI_SCALE = 100%");

  // A native painted control, to show that framework widgets scale too.
  g_app.knob = w->create(g_app.sess, g_app.frame, NEUI_W_KNOB, 12, 54, 64, 64, nullptr);
  g_app.attrs->set_float(g_app.sess, g_app.knob, NEUI_PARAM_VALUE, 0.35f);

  g_app.panel_logical = w->create(g_app.sess, g_app.frame, NEUI_W_CUSTOMDRAW,
                                  92, 54, 168, 120, nullptr);
  g_app.panel_device  = w->create(g_app.sess, g_app.frame, NEUI_W_CUSTOMDRAW,
                                  270, 54, 168, 120, nullptr);
  // The single difference between the two panels.
  g_app.attrs->set_int(g_app.sess, g_app.panel_device,
                       NEUI_ATTR_PAINT_DEVICE_PIXELS, 1);

  neui_widget_t note = w->create(g_app.sess, g_app.frame, NEUI_W_LABEL,
                                 12, 186, 430, 40, nullptr);
  w->set_text(g_app.sess, note,
              "Every coordinate in this example is logical - the zoom is applied "
              "by the framework.");

  w->show(g_app.sess, g_app.frame);
  api->run(g_app.sess);
  return 0;
}

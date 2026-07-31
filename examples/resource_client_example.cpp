// Client resource provider example (NEUI_API_RESOURCE_CLIENT).
//
// Demonstrates a client that keeps its assets somewhere neui knows nothing
// about. Nothing here touches the filesystem: the image is BUILT IN MEMORY at
// startup and handed to the host on demand, under a name that does not exist as
// a file anywhere. The same seam serves a plugin bundle, an executable resource
// section, an encrypted pack, or flash on an MCU.
//
// Two widgets reference the generated asset by name:
//   * an IMAGE widget via set_text("generated.bmp") - the framework loads it
//     lazily on first paint, through the provider;
//   * an explicit assets->create_from_file("generated.bmp") handle, drawn by a
//     CUSTOMDRAW widget, showing the same name resolving via the asset API.
//
// A BMP is generated rather than a PNG so the example stays self-contained with
// no encoder: the header is 54 bytes and every platform decoder reads it.

#include <neui/neui.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// --- The client's "container": a BGR bottom-up BMP built at startup ---------

std::vector<uint8_t> g_bmp;          // the whole file image, header included
int                  g_provide_calls = 0;
int                  g_release_calls = 0;
float                g_last_hint     = 0.0f;

void put_u32(std::vector<uint8_t>& v, uint32_t x)
{
  v.push_back((uint8_t)(x        & 0xFF));
  v.push_back((uint8_t)((x >> 8) & 0xFF));
  v.push_back((uint8_t)((x >> 16) & 0xFF));
  v.push_back((uint8_t)((x >> 24) & 0xFF));
}
void put_u16(std::vector<uint8_t>& v, uint16_t x)
{
  v.push_back((uint8_t)(x       & 0xFF));
  v.push_back((uint8_t)((x >> 8) & 0xFF));
}

// 24-bit BMP: a teal-to-magenta gradient with a 2px dark border, so it is
// obvious at a glance that the pixels came from here and not from a file.
void build_bmp(int w, int h)
{
  const int row_raw = w * 3;
  const int pad     = (4 - (row_raw % 4)) % 4;
  const int row     = row_raw + pad;
  const uint32_t pixels_size = (uint32_t)(row * h);
  const uint32_t offset      = 14 + 40;

  g_bmp.clear();
  g_bmp.reserve(offset + pixels_size);

  // BITMAPFILEHEADER
  g_bmp.push_back('B'); g_bmp.push_back('M');
  put_u32(g_bmp, offset + pixels_size);
  put_u16(g_bmp, 0); put_u16(g_bmp, 0);
  put_u32(g_bmp, offset);
  // BITMAPINFOHEADER
  put_u32(g_bmp, 40);
  put_u32(g_bmp, (uint32_t)w);
  put_u32(g_bmp, (uint32_t)h);        // positive = bottom-up
  put_u16(g_bmp, 1);                  // planes
  put_u16(g_bmp, 24);                 // bpp
  put_u32(g_bmp, 0);                  // BI_RGB
  put_u32(g_bmp, pixels_size);
  put_u32(g_bmp, 2835); put_u32(g_bmp, 2835);   // ~72 dpi
  put_u32(g_bmp, 0); put_u32(g_bmp, 0);

  for (int y = h - 1; y >= 0; --y) {           // bottom-up rows
    for (int x = 0; x < w; ++x) {
      const bool border = (x < 2 || y < 2 || x >= w - 2 || y >= h - 2);
      uint8_t r, g, b;
      if (border) { r = 32; g = 32; b = 40; }
      else {
        const float u = (float)x / (float)(w - 1);
        const float v = (float)y / (float)(h - 1);
        r = (uint8_t)(40.0f + 200.0f * u);
        g = (uint8_t)(190.0f - 120.0f * v);
        b = (uint8_t)(150.0f + 90.0f * v);
      }
      g_bmp.push_back(b); g_bmp.push_back(g); g_bmp.push_back(r);  // BGR
    }
    for (int p = 0; p < pad; ++p) g_bmp.push_back(0);
  }
}

// --- The provider itself ---------------------------------------------------
//
// Serves exactly one name. Everything else is declined, which lets the host fall
// back to its normal filesystem / embedded-resource resolution.

const char* k_asset_name = "generated.bmp";

bool NEUI_ABI res_provide(void* /*token*/, const neui_resource_request_t* req,
                          neui_resource_bytes_t* out)
{
  ++g_provide_calls;
  if (req->kind != NEUI_RESOURCE_KIND_IMAGE) return false;
  if (!req->name || strcmp(req->name, k_asset_name) != 0) return false;

  // scale_hint is what the host would resolve @2x / @3x for. We only have the
  // one resolution, so report it as 1.0 via `scale` and ignore the hint - a real
  // client would pick its closest variant here.
  g_last_hint = req->scale_hint;

  out->data          = g_bmp.data();   // borrowed - only for this call
  out->len           = (uint32_t)g_bmp.size();
  out->scale         = 1.0f;
  out->release_token = nullptr;        // nothing to free: it is a member buffer
  return true;
}

void NEUI_ABI res_release(void* /*token*/, const neui_resource_bytes_t* /*res*/)
{
  ++g_release_calls;   // no-op for a static buffer; counted to show the pairing
}

neui_resource_client_t resource_client = {
  NEUI_VERSION,
  NEUI_RESOURCE_MASK_IMAGE,   // images only - never asked for fonts / components
  res_provide,
  res_release,
};

// --- App -------------------------------------------------------------------

struct App {
  neui_api_t*        neui    = nullptr;
  neui_session_t     sess    = {};
  neui_widget_api_t* widgets = nullptr;
  neui_asset_api_t*  assets  = nullptr;
  neui_asset_t       handle  = asset_none;
  uint32_t           canvas_id = 0;
  uint32_t           status_id = 0;
};
App app;

bool NEUI_ABI onevent(void* /*token*/, neui_event_t* event)
{
  if (event->type == NEUI_EVENT_WIDGET_PAINT &&
      event->data.paint.widget.id == app.canvas_id) {
    auto* p   = event->data.paint.p;
    auto* api = event->data.paint.painter_api;
    const float w = event->data.paint.width;
    const float h = event->data.paint.height;
    api->fill_rect(p, 0, 0, w, h, 0xFF202024u);
    if (app.handle.id != asset_none.id)
      api->draw_asset(p, app.handle, 4, 4, w - 8, h - 8);
    return true;
  }
  return false;
}

neui_widget_client_t widget_client = { NEUI_VERSION, nullptr, onevent };

neui_client_t host_client = {
  NEUI_VERSION,
  [](void* /*token*/, const char* iface) -> void* {
    if (!strcmp(iface, NEUI_API_WIDGETS))         return &widget_client;
    // This is the whole opt-in: return the interface and the host starts asking
    // for bytes before it looks anywhere itself.
    if (!strcmp(iface, NEUI_API_RESOURCE_CLIENT)) return &resource_client;
    return nullptr;
  }
};

}  // namespace

int main()
{
  build_bmp(96, 96);

  neui_init();
  // Pinned to the crossplatform host (as examples/main.cpp does): its IMAGE
  // widget resolves lazily from the path-keyed tier during PAINT, which is the
  // more interesting provider path, and in an LVGL build this is the LVGL host.
  app.neui = neui_get_api("neui.host.crossplatform");
  if (!app.neui) return 1;
  app.sess    = app.neui->create_session(&host_client, &app);
  app.widgets = (neui_widget_api_t*)app.neui->get_interface(app.sess, NEUI_API_WIDGETS);
  app.assets  = (neui_asset_api_t*) app.neui->get_interface(app.sess, NEUI_API_ASSETS);
  if (!app.widgets) return 1;

  // Content is 2 columns of 96px art + labels, so 460x260 holds it with margins.
  auto win = app.widgets->create(app.sess, widget_none, NEUI_W_APPWINDOW,
                                 120, 120, 460, 260, nullptr);
  app.widgets->set_text(app.sess, win, "neui - client resource provider");

  auto title = app.widgets->create(app.sess, win, NEUI_W_LABEL, 12, 10, 430, 20, nullptr);
  app.widgets->set_text(app.sess, title,
                        "Both images below come from client memory, not a file:");

  // 1. IMAGE widget by name. The framework resolves this lazily on first paint,
  //    which is the path that goes through the provider.
  auto img = app.widgets->create(app.sess, win, NEUI_W_IMAGE, 12, 40, 96, 96, nullptr);
  app.widgets->set_text(app.sess, img, k_asset_name);
  auto l1 = app.widgets->create(app.sess, win, NEUI_W_LABEL, 12, 142, 200, 18, nullptr);
  app.widgets->set_text(app.sess, l1, "IMAGE widget (set_text)");

  // 2. Explicit asset handle by the same name, drawn by a CUSTOMDRAW.
  if (app.assets)
    app.handle = app.assets->create_from_file(app.sess, k_asset_name);
  auto canvas = app.widgets->create(app.sess, win, NEUI_W_CUSTOMDRAW,
                                    140, 40, 104, 104, nullptr);
  app.canvas_id = canvas.id;
  auto l2 = app.widgets->create(app.sess, win, NEUI_W_LABEL, 140, 142, 220, 18, nullptr);
  app.widgets->set_text(app.sess, l2, "CUSTOMDRAW (asset handle)");

  // Counts as of the explicit create_from_file above. The IMAGE widget resolves
  // lazily on its first paint, so its own provide() calls land after this.
  auto status = app.widgets->create(app.sess, win, NEUI_W_LABEL, 12, 172, 430, 20, nullptr);
  app.status_id = status.id;
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "%zu BMP bytes in memory; %d provide() / %d release() so far, "
                "scale hint %.1f",
                g_bmp.size(), g_provide_calls, g_release_calls, g_last_hint);
  app.widgets->set_text(app.sess, status, buf);

  auto note = app.widgets->create(app.sess, win, NEUI_W_LABEL, 12, 196, 430, 20, nullptr);
  app.widgets->set_text(app.sess, note,
                        "No file named generated.bmp exists anywhere.");

  app.widgets->show(app.sess, win);
  app.neui->run(app.sess);
  app.neui->endsession(app.sess);
  return 0;
}

// Client resource provider example (NEUI_API_RESOURCE_CLIENT).
//
// Demonstrates a client that keeps its assets somewhere neui knows nothing
// about. Nothing here touches the filesystem: the image is BUILT IN MEMORY at
// startup and handed to the host on demand, under a name that does not exist as
// a file anywhere. The same seam serves a plugin bundle, an executable resource
// section, an encrypted pack, or flash on an MCU.
//
// Three widgets reference the generated asset by name:
//   * an IMAGE widget via set_text("generated.bmp") - the framework loads it
//     lazily on first paint, through the provider;
//   * an explicit assets->create_from_file("generated.bmp") handle, drawn by a
//     CUSTOMDRAW widget, showing the same name resolving via the asset API;
//   * a COMPONENT built from a document that is ALSO served from memory, whose
//     "assets" block references the same image. This is the interesting case for
//     the name contract: the document lives at "widgets/meter.json", so its
//     base_dir is "widgets", and the provider is asked for the raw entry
//     ("generated.bmp") with base_dir passed ALONGSIDE - never joined into
//     "widgets/generated.bmp", which this client would decline. The status lines
//     print the exact name and base_dir the provider saw, so the contract is
//     visible rather than implied.
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

// --- The client's second "file": a component document, also in memory --------
//
// Referenced by the path below, which is what gives it a base_dir of "widgets".
// Its "assets" entry is the RAW image name this client knows - the host must ask
// for exactly that, with "widgets" in base_dir.

const char* k_asset_name     = "generated.bmp";
const char* k_component_name = "widgets/meter.json";

const char* k_component_json = R"json({
  "component": "meter",
  "size": [104, 104],
  "params": [
    { "key": "neui.param.value", "default": 0.7, "min": 0, "max": 1, "label": "Level" }
  ],
  "assets": { "face": "generated.bmp" },
  "layers": [
    { "kind": "asset", "z": 0, "anchor": ["top", "top"], "size": [96, 80],
      "offset": [0, 4], "asset": "face" },
    { "kind": "rect", "z": 1, "anchor": ["center", "center"], "size": [104, 104],
      "stroke_color": "#FF202024", "stroke_width": 2 },
    { "kind": "text", "z": 2, "anchor": ["bottom", "bottom"], "size": ["fill", 18],
      "text": "from client bytes", "font_size": 11, "color": "#FF202024",
      "align": ["center", "center"] }
  ]
})json";

// What the provider was asked for on behalf of the component's asset - printed
// in the UI, because "was it joined onto base_dir?" is the whole point.
std::string g_comp_asset_name;
std::string g_comp_asset_dir;

// --- The provider itself ---------------------------------------------------
//
// Serves exactly two names, one image and one component document. Everything
// else is declined, which lets the host fall back to its normal filesystem /
// embedded-resource resolution.

bool NEUI_ABI res_provide(void* /*token*/, const neui_resource_request_t* req,
                          neui_resource_bytes_t* out)
{
  ++g_provide_calls;
  if (!req->name) return false;

  if (req->kind == NEUI_RESOURCE_KIND_COMPONENT) {
    if (strcmp(req->name, k_component_name) != 0) return false;
    out->data          = (const uint8_t*)k_component_json;
    out->len           = (uint32_t)strlen(k_component_json);
    out->release_token = nullptr;
    return true;
  }

  if (req->kind != NEUI_RESOURCE_KIND_IMAGE) return false;
  if (strcmp(req->name, k_asset_name) != 0) return false;

  // base_dir is set only for a name that came out of a component document. A
  // client with per-document asset tables would key on it; here it is recorded
  // so the UI can show that the name arrived raw and the directory separately.
  if (req->base_dir) {
    g_comp_asset_name = req->name;
    g_comp_asset_dir  = req->base_dir;
  }

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
  // Images + component documents; the mask keeps this client off the font path
  // and off the filmstrip-sidecar path entirely.
  NEUI_RESOURCE_MASK_IMAGE | NEUI_RESOURCE_MASK_COMPONENT,
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

  // Content is 3 columns of ~104px art at y=40..144 (widest right edge 300+104),
  // labels at y=148, and 3 status lines 616 wide ending at y=240; 640x272 holds
  // that with a margin on all sides.
  auto win = app.widgets->create(app.sess, widget_none, NEUI_W_APPWINDOW,
                                 120, 120, 640, 272, nullptr);
  app.widgets->set_text(app.sess, win, "neui - client resource provider");

  auto title = app.widgets->create(app.sess, win, NEUI_W_LABEL, 12, 10, 616, 20, nullptr);
  app.widgets->set_text(app.sess, title,
                        "Everything below comes from client memory, not a file:");

  // 1. IMAGE widget by name. The framework resolves this lazily on first paint,
  //    which is the path that goes through the provider.
  auto img = app.widgets->create(app.sess, win, NEUI_W_IMAGE, 12, 40, 96, 96, nullptr);
  app.widgets->set_text(app.sess, img, k_asset_name);
  auto l1 = app.widgets->create(app.sess, win, NEUI_W_LABEL, 12, 148, 132, 18, nullptr);
  app.widgets->set_text(app.sess, l1, "IMAGE (set_text)");

  // 2. Explicit asset handle by the same name, drawn by a CUSTOMDRAW.
  if (app.assets)
    app.handle = app.assets->create_from_file(app.sess, k_asset_name);
  auto canvas = app.widgets->create(app.sess, win, NEUI_W_CUSTOMDRAW,
                                    156, 40, 104, 104, nullptr);
  app.canvas_id = canvas.id;
  auto l2 = app.widgets->create(app.sess, win, NEUI_W_LABEL, 156, 148, 132, 18, nullptr);
  app.widgets->set_text(app.sess, l2, "CUSTOMDRAW handle");

  // 3. A COMPONENT whose document AND whose layer asset both come from the
  //    provider. The document path gives it base_dir "widgets"; the asset inside
  //    it is named "generated.bmp" and must arrive that way, or this client
  //    declines and the face is blank.
  if (app.assets) {
    neui_asset_t comp = app.assets->create_component_from_file(app.sess,
                                                              k_component_name, nullptr);
    if (comp.id != asset_none.id)
      app.widgets->create_from_component(app.sess, win, comp, 300, 40, 104, 104);
  }
  auto l3 = app.widgets->create(app.sess, win, NEUI_W_LABEL, 300, 148, 160, 18, nullptr);
  app.widgets->set_text(app.sess, l3, "COMPONENT doc + asset");

  // Counts as of the loads above. The IMAGE widget resolves lazily on its first
  // paint, so its own provide() calls land after this.
  auto status = app.widgets->create(app.sess, win, NEUI_W_LABEL, 12, 172, 616, 20, nullptr);
  app.status_id = status.id;
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "%zu BMP bytes in memory; %d provide() / %d release() so far, "
                "scale hint %.1f",
                g_bmp.size(), g_provide_calls, g_release_calls, g_last_hint);
  app.widgets->set_text(app.sess, status, buf);

  // The name contract, printed rather than asserted: raw entry + separate dir.
  auto prov = app.widgets->create(app.sess, win, NEUI_W_LABEL, 12, 196, 616, 20, nullptr);
  char buf2[256];
  std::snprintf(buf2, sizeof(buf2),
                "component doc \"%s\" -> its asset asked as name=\"%s\" base_dir=\"%s\"",
                k_component_name,
                g_comp_asset_name.empty() ? "(never asked)" : g_comp_asset_name.c_str(),
                g_comp_asset_dir.empty()  ? "(none)"        : g_comp_asset_dir.c_str());
  app.widgets->set_text(app.sess, prov, buf2);

  auto note = app.widgets->create(app.sess, win, NEUI_W_LABEL, 12, 220, 616, 20, nullptr);
  app.widgets->set_text(app.sess, note,
                        "Neither generated.bmp nor widgets/meter.json exists on disk.");

  app.widgets->show(app.sess, win);
  app.neui->run(app.sess);
  app.neui->endsession(app.sess);
  return 0;
}

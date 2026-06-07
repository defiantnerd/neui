#include "neui_test.h"

#include "clipboard_item.h"

#ifdef _WIN32
// Win32-only: PNG <-> CF_DIBV5 round-trip helpers. The pragma comment in
// the header auto-links Windowscodecs.lib, so the otherwise-header-only
// Tier-1 test exe stays correct.
#include "win32/clipboard_format_png_win32.h"
#endif

#include <cstring>
#include <string>
#include <vector>

using namespace neui_detail;

// ---------------------------------------------------------------------------
// Eager bytes path - unchanged behaviour from the pre-lazy DataItem.
// ---------------------------------------------------------------------------

TEST_CASE("DataItem: set_format / get_format round-trip")
{
  DataItem item;
  const char* payload = "hello";
  item.set_format("text/plain", payload, 5);

  CHECK(item.has_format("text/plain"));
  CHECK_EQ(item.get_format("text/plain", nullptr, 0), 5);

  char buf[8] = { 0 };
  CHECK_EQ(item.get_format("text/plain", buf, sizeof(buf)), 5);
  CHECK_EQ(std::string(buf, 5), std::string("hello"));
}

TEST_CASE("DataItem: get_format on absent mime returns 0")
{
  DataItem item;
  CHECK_FALSE(item.has_format("application/octet-stream"));
  CHECK_EQ(item.get_format("application/octet-stream", nullptr, 0), 0);
}

// ---------------------------------------------------------------------------
// Lazy provider path - bytes produced on first read, cached for subsequent.
// ---------------------------------------------------------------------------

namespace {
  struct LazyProbe {
    int          call_count = 0;
    std::string  last_mime;
    std::vector<uint8_t> bytes;
  };

  const uint8_t* probe_provider(void* userdata, const char* mime, uint32_t* out_size)
  {
    auto* p = static_cast<LazyProbe*>(userdata);
    p->call_count++;
    p->last_mime = mime ? mime : "";
    if (out_size) *out_size = static_cast<uint32_t>(p->bytes.size());
    return p->bytes.empty() ? nullptr : p->bytes.data();
  }
}

TEST_CASE("DataItem: set_format_provider defers materialisation until get_format")
{
  DataItem item;
  LazyProbe probe;
  probe.bytes = { 'l', 'a', 'z', 'y' };

  item.set_format_provider("image/png", &probe_provider, &probe);

  // has_format returns true immediately - the entry exists.
  CHECK(item.has_format("image/png"));
  // is_lazy_format flags it as not-yet-materialised.
  CHECK(item.is_lazy_format("image/png"));
  // Provider has NOT been called yet.
  CHECK_EQ(probe.call_count, 0);

  // First read fires the provider.
  CHECK_EQ(item.get_format("image/png", nullptr, 0), 4);
  CHECK_EQ(probe.call_count, 1);
  CHECK_EQ(probe.last_mime, std::string("image/png"));
  // Now materialised - second read uses the cached bytes.
  CHECK_FALSE(item.is_lazy_format("image/png"));

  char buf[8] = { 0 };
  CHECK_EQ(item.get_format("image/png", buf, sizeof(buf)), 4);
  CHECK_EQ(std::string(buf, 4), std::string("lazy"));
  // Provider was not called again.
  CHECK_EQ(probe.call_count, 1);
}

TEST_CASE("DataItem: lazy provider returning 0 / null caches empty result")
{
  DataItem item;
  LazyProbe probe;  // bytes empty by default
  item.set_format_provider("application/x-empty", &probe_provider, &probe);

  CHECK_EQ(item.get_format("application/x-empty", nullptr, 0), 0);
  CHECK_EQ(probe.call_count, 1);

  // Second call: cached empty result, provider not re-fired.
  CHECK_EQ(item.get_format("application/x-empty", nullptr, 0), 0);
  CHECK_EQ(probe.call_count, 1);
}

TEST_CASE("DataItem: for_each_format materialises lazy entries on iteration")
{
  DataItem item;
  LazyProbe probe;
  probe.bytes = { 'a', 'b', 'c' };

  item.set_format("text/plain", "x", 1);
  item.set_format_provider("application/x-custom", &probe_provider, &probe);

  int total = 0;
  item.for_each_format([&](const std::string& mime,
                            const std::vector<uint8_t>& bytes) {
    total += static_cast<int>(bytes.size());
    (void)mime;
  });
  CHECK_EQ(total, 1 + 3);  // text/plain (1) + lazy bytes (3)
  CHECK_EQ(probe.call_count, 1);
}

TEST_CASE("DataItem: for_each_mime does NOT materialise lazy entries")
{
  DataItem item;
  LazyProbe probe;
  probe.bytes = { 'a', 'b', 'c' };

  item.set_format_provider("application/x-lazy", &probe_provider, &probe);

  int count = 0;
  item.for_each_mime([&](const std::string& mime) {
    if (mime == "application/x-lazy") ++count;
  });
  CHECK_EQ(count, 1);
  // Provider stayed silent - lazy enumeration must not force materialisation.
  CHECK_EQ(probe.call_count, 0);
  CHECK(item.is_lazy_format("application/x-lazy"));
}

TEST_CASE("DataItem: set_format on a lazy entry switches it back to eager")
{
  DataItem item;
  LazyProbe probe;
  probe.bytes = { 'z' };
  item.set_format_provider("application/x-mix", &probe_provider, &probe);
  CHECK(item.is_lazy_format("application/x-mix"));

  // Overwrite with eager bytes - lazy state should clear.
  const char* hi = "hi";
  item.set_format("application/x-mix", hi, 2);
  CHECK_FALSE(item.is_lazy_format("application/x-mix"));
  CHECK_EQ(item.get_format("application/x-mix", nullptr, 0), 2);
  CHECK_EQ(probe.call_count, 0);
}

TEST_CASE("DataItem: get_lazy_provider exposes (fn, userdata) for the macOS delegate")
{
  DataItem item;
  LazyProbe probe;
  item.set_format_provider("application/x-tagged", &probe_provider, &probe);

  DataProviderFn fn = nullptr;
  void* ud = nullptr;
  CHECK(item.get_lazy_provider("application/x-tagged", &fn, &ud));
  CHECK(fn == &probe_provider);
  CHECK(ud == &probe);

  // Absent mime / eager entry both return false.
  item.set_format("text/plain", "x", 1);
  CHECK_FALSE(item.get_lazy_provider("text/plain", nullptr, nullptr));
  CHECK_FALSE(item.get_lazy_provider("missing/mime", nullptr, nullptr));
}

// ---------------------------------------------------------------------------
// DataItemStore: ids, slot reuse.
// ---------------------------------------------------------------------------

#ifdef _WIN32
// ---------------------------------------------------------------------------
// PNG <-> CF_DIBV5 round-trip (Win32 only). Uses WIC, which needs CoInitialize
// on this thread. STA matches the Tier-1 test runner.
// ---------------------------------------------------------------------------

namespace {
  // Process-static CoInitialize. The WIC factory inside image_loader_win32.h
  // caches the IWICImagingFactory* statically; if we paired Init/Uninit per
  // test, the second test would see a dangling factory pointer and crash on
  // CoUninitialize-on-exit. Keep the apartment alive for the whole run.
  struct CoInitOnce {
    CoInitOnce() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
  };
  static CoInitOnce s_co_init_once;

  // Encode a tiny synthetic BGRA8 (2x2 colour grid) into PNG, decode back,
  // verify dimensions + a representative pixel.
  std::vector<uint8_t> make_test_png()
  {
    // 2x2 image, BGRA, straight alpha.
    // (0,0) red opaque, (1,0) green opaque,
    // (0,1) blue opaque, (1,1) transparent.
    uint8_t bgra[4 * 4] = {
      0x00, 0x00, 0xFF, 0xFF,  // BGRA = red
      0x00, 0xFF, 0x00, 0xFF,  // green
      0xFF, 0x00, 0x00, 0xFF,  // blue
      0x00, 0x00, 0x00, 0x00,  // transparent
    };
    return bgra8_to_png_bytes_w32(bgra, 2, 2);
  }
}

TEST_CASE("Win32 PNG <-> DIBV5: round-trip preserves pixels")
{
  auto png = make_test_png();
  CHECK(!png.empty());

  auto dib = png_bytes_to_dibv5_bytes_w32(png.data(),
                                           static_cast<uint32_t>(png.size()));
  CHECK(!dib.empty());

  uint32_t w = 0, h = 0;
  uint8_t* bgra = dib_bytes_to_bgra8_w32(dib.data(),
                                          static_cast<uint32_t>(dib.size()),
                                          &w, &h);
  CHECK(bgra != nullptr);
  CHECK_EQ((int)w, 2);
  CHECK_EQ((int)h, 2);

  // Spot-check pixels we encoded. BGRA, top-down.
  CHECK_EQ((int)bgra[0 * 4 + 0], 0x00); CHECK_EQ((int)bgra[0 * 4 + 1], 0x00);
  CHECK_EQ((int)bgra[0 * 4 + 2], 0xFF); CHECK_EQ((int)bgra[0 * 4 + 3], 0xFF);
  CHECK_EQ((int)bgra[3 * 4 + 3], 0x00);  // transparent

  delete[] bgra;
}

TEST_CASE("Win32 PNG decode handles top-down + bottom-up DIBs")
{
  auto png = make_test_png();
  CHECK(!png.empty());

  // Build a bottom-up DIB by hand and verify dib_bytes_to_bgra8 flips it.
  uint32_t w = 0, h = 0;
  uint8_t* bgra = png_bytes_to_bgra8_w32(png.data(),
                                          static_cast<uint32_t>(png.size()),
                                          &w, &h);
  CHECK(bgra != nullptr);

  uint32_t stride = w * 4;
  uint32_t pixels_size = stride * h;
  std::vector<uint8_t> dib(sizeof(BITMAPV5HEADER) + pixels_size, 0);
  auto* hdr = reinterpret_cast<BITMAPV5HEADER*>(dib.data());
  hdr->bV5Size        = sizeof(BITMAPV5HEADER);
  hdr->bV5Width       = (LONG)w;
  hdr->bV5Height      = (LONG)h;  // positive = bottom-up
  hdr->bV5Planes      = 1;
  hdr->bV5BitCount    = 32;
  hdr->bV5Compression = BI_BITFIELDS;
  hdr->bV5SizeImage   = pixels_size;
  hdr->bV5RedMask     = 0x00FF0000;
  hdr->bV5GreenMask   = 0x0000FF00;
  hdr->bV5BlueMask    = 0x000000FF;
  hdr->bV5AlphaMask   = 0xFF000000;
  // Pack rows reversed (bottom-up).
  for (uint32_t y = 0; y < h; ++y) {
    std::memcpy(dib.data() + sizeof(BITMAPV5HEADER) + y * stride,
                 bgra + (h - 1 - y) * stride, stride);
  }
  delete[] bgra;

  uint32_t w2 = 0, h2 = 0;
  uint8_t* roundtrip = dib_bytes_to_bgra8_w32(dib.data(),
                                                static_cast<uint32_t>(dib.size()),
                                                &w2, &h2);
  CHECK(roundtrip != nullptr);
  CHECK_EQ((int)w2, (int)w);
  CHECK_EQ((int)h2, (int)h);
  // Top-row first pixel should be the red we encoded at (0,0).
  CHECK_EQ((int)roundtrip[0 * 4 + 2], 0xFF);
  delete[] roundtrip;
}
#endif  // _WIN32

TEST_CASE("DataItemStore: allocate -> get -> release -> slot reuse")
{
  DataItemStore store;
  uint32_t a = store.allocate();
  uint32_t b = store.allocate();
  CHECK(a >= 1);
  CHECK(b >= 1);
  CHECK(a != b);
  CHECK(store.get(a) != nullptr);
  CHECK(store.get(b) != nullptr);

  store.release(a);
  CHECK(store.get(a) == nullptr);

  uint32_t c = store.allocate();
  // The freed slot should be reused.
  CHECK_EQ(c, a);
}

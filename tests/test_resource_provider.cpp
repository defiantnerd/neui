#include "neui_test.h"

// Tier-1 coverage for the client resource provider (NEUI_API_RESOURCE_CLIENT,
// <neui/d/resource.h>) as wired into AssetStore<Loader> (hosts/shared/) plus the
// (name, scale-bucket) resolution cache it rides on
// (plans/client-resource-provider.md).
//
// What matters here and is easy to regress:
//   * order          - client is asked BEFORE the filesystem ladder
//   * caching        - resolution is probed once per (name, scale band); in
//                      particular NOT once per paint, which is what the cache
//                      exists for
//   * one decode     - the probe's pixels are parked for the load that wanted
//                      them, so a cold load decodes (and provides) once
//   * band keys      - one name served by the client at two scales gets two
//                      cache keys, so the derived path-keyed caches cannot serve
//                      the wrong resolution
//   * negative cache - a declining client is not re-asked per paint, while an
//                      explicit load re-probes; clear() drops outcomes too
//   * kinds_mask     - a client that only serves images is never asked for fonts
//   * release        - paired with every provide() that returned true
//   * bad bytes      - a blob that fails to decode falls through to the file
//                      rather than shadowing it, on EVERY load and not just the
//                      probe that decided the route
//
// Uses a counting fake loader + a scripted fake client, so no host, no backend
// and no real files are involved.

#include "asset_store.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace neui_detail;

namespace {

// --- Fake loader counting both entry points, with a scripted set of paths that
// "exist" so the @Nx ladder can be steered. --------------------------------
struct CountingLoader {
  static std::vector<std::string> s_existing;   // paths load() succeeds for
  static int                     s_path_calls;
  static int                     s_mem_calls;
  static bool                    s_mem_fails;   // simulate undecodable bytes
  static uint32_t                s_w, s_h;

  static void reset(std::vector<std::string> existing = {}) {
    s_existing   = std::move(existing);
    s_path_calls = 0;
    s_mem_calls  = 0;
    s_mem_fails  = false;
    s_w = 8; s_h = 4;
  }

  static uint8_t* alloc(uint32_t* w, uint32_t* h) {
    if (w) *w = s_w;
    if (h) *h = s_h;
    return static_cast<uint8_t*>(std::calloc((size_t)s_w * s_h * 4u, 1));
  }

  static uint8_t* load(const char* path, uint32_t* w, uint32_t* h) {
    ++s_path_calls;
    for (const auto& p : s_existing)
      if (p == path) return alloc(w, h);
    return nullptr;
  }
  static uint8_t* load_memory(const uint8_t*, size_t, uint32_t* w, uint32_t* h) {
    ++s_mem_calls;
    if (s_mem_fails) return nullptr;
    return alloc(w, h);
  }
  static void free_pixels(uint8_t* p) { std::free(p); }
};
std::vector<std::string> CountingLoader::s_existing;
int      CountingLoader::s_path_calls = 0;
int      CountingLoader::s_mem_calls  = 0;
bool     CountingLoader::s_mem_fails  = false;
uint32_t CountingLoader::s_w = 8;
uint32_t CountingLoader::s_h = 4;

// --- Scripted fake client. Records every request; answers the names in
// `serves` with a dummy blob at `reply_scale`. ------------------------------
struct FakeClient {
  std::vector<std::string>          names;      // requested names, in order
  std::vector<neui_resource_kind_t> kinds;
  std::vector<float>                hints;
  std::vector<std::string>          dirs;       // request base_dir ("" if NULL)
  std::vector<std::string>          serves;     // names it has bytes for
  float                             reply_scale = 0.0f;   // 0 -> "treat as 1.0"
  int                               provides    = 0;      // returned true count
  int                               releases    = 0;
  uint8_t                           blob[4]     = { 1, 2, 3, 4 };

  void reset() {
    names.clear(); kinds.clear(); hints.clear(); dirs.clear();
    provides = releases = 0;
  }
};

FakeClient g_fc;

bool NEUI_ABI fc_provide(void* token, const neui_resource_request_t* req,
                         neui_resource_bytes_t* out) {
  auto* fc = static_cast<FakeClient*>(token);
  fc->names.push_back(req->name ? req->name : "");
  fc->kinds.push_back(req->kind);
  fc->hints.push_back(req->scale_hint);
  fc->dirs.push_back(req->base_dir ? req->base_dir : "");
  for (const auto& s : fc->serves) {
    if (s == fc->names.back()) {
      out->data          = fc->blob;
      out->len           = sizeof(fc->blob);
      out->scale         = fc->reply_scale;
      out->release_token = fc;
      ++fc->provides;
      return true;
    }
  }
  return false;
}

void NEUI_ABI fc_release(void* token, const neui_resource_bytes_t* res) {
  auto* fc = static_cast<FakeClient*>(token);
  CHECK(res->release_token == fc);   // cookie round-trips
  ++fc->releases;
}

// Build a store with the fake client installed. kinds_mask 0 = all kinds.
ResourceProvider make_provider(neui_resource_client_t& iface, uint32_t kinds_mask) {
  iface = neui_resource_client_t{};
  iface.kinds_mask = kinds_mask;
  iface.provide    = fc_provide;
  iface.release    = fc_release;
  ResourceProvider p;
  p.client = &iface;
  p.token  = &g_fc;
  return p;
}

}  // namespace

TEST_CASE("resource provider: client is asked before the filesystem")
{
  CountingLoader::reset({ "knob.png" });      // the file DOES exist
  g_fc = FakeClient{};
  g_fc.serves = { "knob.png" };

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  store.set_resource_provider(make_provider(iface, 0));

  uint32_t slot = store.allocate_from_file("knob.png", 1.0f);
  CHECK(slot != 0);

  // Client won: it was asked, and the @Nx ladder never ran.
  CHECK(g_fc.names[0] == "knob.png");
  CHECK_EQ(CountingLoader::s_path_calls, 0);
  // Cold-load cost: the validating probe's pixels are PARKED and handed to the
  // load that wanted them, so one provide() and one decode - not two of each.
  CHECK_EQ((int)g_fc.names.size(), 1);
  CHECK_EQ(CountingLoader::s_mem_calls, 1);
  CHECK_EQ(g_fc.provides, 1);
  CHECK_EQ(g_fc.releases, 1);          // release paired with every hit
}

TEST_CASE("resource provider: a cold filesystem load decodes once, not twice")
{
  CountingLoader::reset({ "bg.png" });
  AssetStore<CountingLoader> store;    // no provider at all

  CHECK(store.allocate_from_file("bg.png", 1.0f) != 0);
  // resolve_path has to decode a candidate to know it exists; those pixels are
  // parked for the load rather than thrown away.
  CHECK_EQ(CountingLoader::s_path_calls, 1);
}

TEST_CASE("resource provider: client routes do not collapse across scale bands")
{
  CountingLoader::reset();
  g_fc = FakeClient{};
  g_fc.serves = { "logo.png" };

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  store.set_resource_provider(make_provider(iface, 0));

  // Same name, two display scales. The client is free to answer each band with
  // different pixels, so the two routes must not share a cache_key - a
  // path-keyed cache downstream stores one entry per key, and collapsing them
  // serves the first band's bitmap at every other scale.
  const std::string k1 = store.image_route("logo.png", 1.0f).cache_key;
  const std::string k2 = store.image_route("logo.png", 2.0f).cache_key;
  CHECK(store.image_route("logo.png", 1.0f).from_client);
  CHECK(!k1.empty());
  CHECK(k1 != k2);
  // ... and neither may be mistakable for a filesystem path.
  CHECK(k1.find("logo.png") != std::string::npos);
  CHECK(k1[0] == '\x01');
}

TEST_CASE("resource provider: base_dir is passed alongside the name, not joined")
{
  // The component-document path (ComponentApis::bitmap_from_name -> here). The
  // client must be asked for the raw "assets"-map entry with the document's
  // directory as base_dir; only the filesystem fallback joins them.
  CountingLoader::reset({ "res/b/knob.png" });   // the joined file exists
  g_fc = FakeClient{};
  g_fc.serves = { "knob.png" };                  // client knows the RAW name
  g_fc.dirs.clear();

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  store.set_resource_provider(make_provider(iface, 0));

  CHECK(store.allocate_from_file("knob.png", 1.0f, "res/a") != 0);
  REQUIRE((int)g_fc.names.size() == 1);
  CHECK(g_fc.names[0] == "knob.png");            // NOT "res/a/knob.png"
  CHECK(g_fc.dirs[0]  == "res/a");
  CHECK_EQ(CountingLoader::s_path_calls, 0);     // client won, no ladder

  // A different document, same asset name: its own route, its own provider call
  // (two components may use one name for different images).
  CHECK(store.allocate_from_file("knob.png", 1.0f, "res/b") != 0);
  REQUIRE((int)g_fc.names.size() == 2);
  CHECK(g_fc.dirs[1] == "res/b");

  // And with no provider answer, the ladder resolves base_dir + name.
  g_fc.serves.clear();
  AssetStore<CountingLoader> store2;
  neui_resource_client_t iface2{};
  store2.set_resource_provider(make_provider(iface2, 0));
  CHECK(store2.allocate_from_file("knob.png", 1.0f, "res/b") != 0);
  CHECK(store2.image_route("knob.png", 1.0f, false, "res/b").file_path
        == std::string("res/b/knob.png"));
  // A bare name with no base_dir is a different route and does not exist.
  CHECK_EQ((int)store2.allocate_from_file("knob.png", 1.0f), 0);
}

TEST_CASE("resource provider: a client route still falls back to the file")
{
  CountingLoader::reset({ "logo.png" });     // the file is there all along
  g_fc = FakeClient{};
  g_fc.serves = { "logo.png" };

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  store.set_resource_provider(make_provider(iface, 0));

  CHECK(store.allocate_from_file("logo.png", 1.0f) != 0);
  CHECK(store.image_route("logo.png", 1.0f).from_client);
  CHECK_EQ(CountingLoader::s_path_calls, 0);

  // The provider now stops answering (a transient container failure, a bug on a
  // second call). The route is already cached as from_client, but the load must
  // still succeed off the filesystem instead of failing for the rest of the
  // session - "a buggy provider cannot shadow a good file" applies to every
  // load, not only to the probe that decided the route.
  g_fc.serves.clear();
  CHECK(store.allocate_from_file("logo.png", 1.0f) != 0);
  CHECK(CountingLoader::s_path_calls > 0);
}

TEST_CASE("resource provider: raw name and scale hint reach the client")
{
  CountingLoader::reset();
  g_fc = FakeClient{};
  g_fc.serves = { "knob.png" };
  g_fc.reply_scale = 2.0f;

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  store.set_resource_provider(make_provider(iface, 0));

  CountingLoader::s_w = 16; CountingLoader::s_h = 8;
  uint32_t slot = store.allocate_from_file("knob.png", 2.0f);
  CHECK(slot != 0);

  // The name is passed through verbatim - NOT rewritten to "knob@2x.png".
  CHECK(g_fc.names[0] == "knob.png");
  CHECK_EQ(g_fc.hints[0], 2.0f);
  CHECK_EQ(g_fc.kinds[0], NEUI_RESOURCE_KIND_IMAGE);

  // The client's declared scale is what the entry records, so the asset's
  // logical size is 16x8 / 2 = 8x4.
  const AssetEntry* e = store.get_slot(slot);
  REQUIRE(e != nullptr);
  CHECK_EQ(e->scale, 2.0f);
  CHECK_EQ((int)e->width_px, 16);
  CHECK_EQ((int)e->height_px, 8);
}

TEST_CASE("resource provider: a declining client is asked once, not per lookup")
{
  CountingLoader::reset({ "bg.png" });
  g_fc = FakeClient{};
  g_fc.serves = {};                   // declines everything

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  store.set_resource_provider(make_provider(iface, 0));

  CHECK(store.allocate_from_file("bg.png", 1.0f) != 0);
  const int after_first = CountingLoader::s_path_calls;
  const int asked_once  = (int)g_fc.names.size();
  CHECK_EQ(asked_once, 1);

  // Repeat lookups of the same (name, scale band) must not re-probe: no more
  // client calls and no more ladder decodes. This is the property that keeps a
  // per-frame IMAGE resolve off the decoder.
  for (int i = 0; i < 5; ++i)
    CHECK(store.image_route("bg.png", 1.0f).found);
  CHECK_EQ((int)g_fc.names.size(), asked_once);
  CHECK_EQ(CountingLoader::s_path_calls, after_first);
  CHECK_EQ(g_fc.releases, 0);         // nothing to release on a decline

  // Fractional scales inside one band (>1 and <=2) share a single entry, so a
  // 125% / 175% display does not multiply the probing.
  CHECK(store.image_route("bg.png", 1.25f).found);
  const int after_band = (int)g_fc.names.size();
  CHECK_EQ(after_band, asked_once + 1);          // the >1 band probed once
  CHECK(store.image_route("bg.png", 1.75f).found);
  CHECK_EQ((int)g_fc.names.size(), after_band);  // and only once
}

TEST_CASE("resource provider: a missing resource is negatively cached")
{
  CountingLoader::reset();            // nothing exists anywhere
  g_fc = FakeClient{};
  g_fc.serves = {};

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  store.set_resource_provider(make_provider(iface, 0));

  CHECK_EQ((int)store.allocate_from_file("nope.png", 1.0f), 0);
  const int probes = CountingLoader::s_path_calls;   // the @Nx ladder ran once
  CHECK(probes > 0);

  // The per-frame resolve is where stickiness has to hold: an IMAGE widget
  // pointing at a missing file must not re-ask the client - or re-run the ladder
  // - on every paint. (An explicit allocate_from_file deliberately DOES get
  // another look; see "an explicit load re-probes a cached miss".)
  for (int i = 0; i < 5; ++i)
    CHECK(!store.image_route("nope.png", 1.0f).found);

  CHECK_EQ((int)g_fc.names.size(), 1);               // asked once, ever
  CHECK_EQ(CountingLoader::s_path_calls, probes);    // ladder not re-run

  // clear_image_routes drops the sticky miss so a late-published resource can
  // still appear (the v0 invalidation story).
  store.clear_image_routes();
  CHECK_EQ((int)store.allocate_from_file("nope.png", 1.0f), 0);
  CHECK_EQ((int)g_fc.names.size(), 2);
}

TEST_CASE("resource provider: an explicit load re-probes a cached miss")
{
  CountingLoader::reset();             // late.png does not exist yet
  AssetStore<CountingLoader> store;    // no provider - filesystem only

  CHECK_EQ((int)store.allocate_from_file("late.png", 1.0f), 0);

  // The per-frame resolve stays sticky: a repaint must not re-run the @Nx ladder.
  const int after_miss = CountingLoader::s_path_calls;
  for (int i = 0; i < 5; ++i) CHECK(!store.image_route("late.png", 1.0f).found);
  CHECK_EQ(CountingLoader::s_path_calls, after_miss);

  // An explicit create_from_file is a client-initiated load, so it gets another
  // look - a file written after the first attempt (a downloader, a save-then-
  // reload, a designer tool rewriting an asset) must not stay unloadable for the
  // rest of the session.
  CountingLoader::s_existing.push_back("late.png");
  CHECK(store.allocate_from_file("late.png", 1.0f) != 0);
  CHECK(store.image_route("late.png", 1.0f).found);
}

TEST_CASE("resource provider: clear() drops resolution outcomes with the assets")
{
  CountingLoader::reset();
  AssetStore<CountingLoader> store;

  CHECK(!store.image_route("later.png", 1.0f).found);
  CountingLoader::s_existing.push_back("later.png");
  CHECK(!store.image_route("later.png", 1.0f).found);   // still the cached miss

  store.clear(nullptr);                                 // full asset reset
  CHECK(store.image_route("later.png", 1.0f).found);
}

TEST_CASE("resource provider: different scale bands resolve independently")
{
  CountingLoader::reset({ "k.png", "k@2x.png" });
  g_fc = FakeClient{};
  g_fc.serves = {};

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  store.set_resource_provider(make_provider(iface, 0));

  const AssetStore<CountingLoader>::ImageRoute& r1 = store.image_route("k.png", 1.0f);
  CHECK(r1.file_path == "k.png");
  CHECK_EQ(r1.scale, 1.0f);

  const AssetStore<CountingLoader>::ImageRoute& r2 = store.image_route("k.png", 2.0f);
  CHECK(r2.file_path == "k@2x.png");
  CHECK_EQ(r2.scale, 2.0f);

  // Two bands -> the client was consulted once per band, not once per variant.
  CHECK_EQ((int)g_fc.names.size(), 2);
}

TEST_CASE("resource provider: kinds_mask keeps a client off the kinds it declines")
{
  CountingLoader::reset({ "x.png" });
  g_fc = FakeClient{};
  g_fc.serves = { "x.png", "font.ttf" };

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  // Images only: the font path must not reach the client at all.
  store.set_resource_provider(make_provider(iface, NEUI_RESOURCE_MASK_IMAGE));

  const ResourceProvider& p = store.resource_provider();
  CHECK(p.serves(NEUI_RESOURCE_KIND_IMAGE));
  CHECK(!p.serves(NEUI_RESOURCE_KIND_FONT));
  CHECK(!p.serves(NEUI_RESOURCE_KIND_COMPONENT));
  CHECK(!p.serves(NEUI_RESOURCE_KIND_SIDECAR));

  CHECK(store.allocate_from_file("x.png", 1.0f) != 0);
  const int image_asks = (int)g_fc.names.size();   // probe + fetch

  // A masked-out kind must not reach the client even though it has bytes for
  // that name: read_bytes goes straight to the (non-existent) file and fails.
  std::string out;
  CHECK(!p.read_bytes(NEUI_RESOURCE_KIND_FONT, "font.ttf", out));
  CHECK_EQ((int)g_fc.names.size(), image_asks);
  for (auto k : g_fc.kinds) CHECK_EQ(k, NEUI_RESOURCE_KIND_IMAGE);

  // mask 0 == every kind.
  neui_resource_client_t all{};
  store.set_resource_provider(make_provider(all, 0));
  const ResourceProvider& q = store.resource_provider();
  CHECK(q.serves(NEUI_RESOURCE_KIND_IMAGE));
  CHECK(q.serves(NEUI_RESOURCE_KIND_FONT));
  CHECK(q.serves(NEUI_RESOURCE_KIND_SIDECAR));
}

TEST_CASE("resource provider: undecodable client bytes fall through to the file")
{
  CountingLoader::reset({ "logo.png" });     // the file is fine
  CountingLoader::s_mem_fails = true;        // the client's blob is not
  g_fc = FakeClient{};
  g_fc.serves = { "logo.png" };

  AssetStore<CountingLoader> store;
  neui_resource_client_t iface{};
  store.set_resource_provider(make_provider(iface, 0));

  uint32_t slot = store.allocate_from_file("logo.png", 1.0f);
  CHECK(slot != 0);                          // loaded - from the FILE

  const AssetStore<CountingLoader>::ImageRoute& r = store.image_route("logo.png", 1.0f);
  CHECK(!r.from_client);
  CHECK(r.file_path == "logo.png");
  CHECK(CountingLoader::s_path_calls > 0);
  CHECK_EQ(g_fc.releases, 1);                // the rejected blob was released
}

TEST_CASE("resource provider: absent client leaves resolution untouched")
{
  CountingLoader::reset({ "a.png" });
  AssetStore<CountingLoader> store;          // no provider installed

  CHECK(store.allocate_from_file("a.png", 1.0f) != 0);
  CHECK_EQ((int)store.allocate_from_file("b.png", 1.0f), 0);
  CHECK_EQ(CountingLoader::s_mem_calls, 0);  // byte path never entered
}

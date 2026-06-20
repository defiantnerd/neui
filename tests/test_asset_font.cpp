#include "neui_test.h"

// Tier-1 coverage for the FONT asset slot lifecycle in AssetStore<Loader>
// (hosts/shared/asset_store.h). Uses a fake image loader (never invoked for
// fonts) + a fake backend stub recording register / unregister calls, so the
// portable allocate -> family-stored -> release-unregisters path is exercised
// with no real backend / host linked.

#include "asset_store.h"

#include <cstring>
#include <string>

using namespace neui_detail;

namespace {

// --- Fake image loader: fonts never touch it. ------------------------------
struct FakeLoader {
  static uint8_t* load(const char*, uint32_t*, uint32_t*) { return nullptr; }
  static void     free_pixels(uint8_t*) {}
};

// --- Fake backend recording font register / unregister traffic. ------------
int      g_register_calls      = 0;
int      g_register_file_calls = 0;
int      g_unregister_calls    = 0;
uint64_t g_last_unregistered   = 0;
bool     g_register_should_fail = false;

void put_family(char* out, uint32_t cap, const char* fam) {
  if (!out || cap == 0) return;
  uint32_t n = (uint32_t)std::strlen(fam);
  if (n > cap - 1) n = cap - 1;
  if (n) std::memcpy(out, fam, n);
  out[n] = '\0';
}

bool fake_register_font(const uint8_t*, uint32_t, char* of, uint32_t cap, uint64_t* ot) {
  ++g_register_calls;
  if (g_register_should_fail) { if (of && cap) of[0] = '\0'; if (ot) *ot = 0; return false; }
  put_family(of, cap, "FakeFamily");
  if (ot) *ot = 0xABCDu;
  return true;
}
bool fake_register_font_file(const char*, char* of, uint32_t cap, uint64_t* ot) {
  ++g_register_file_calls;
  if (g_register_should_fail) { if (of && cap) of[0] = '\0'; if (ot) *ot = 0; return false; }
  put_family(of, cap, "FileFamily");
  if (ot) *ot = 0x1234u;
  return true;
}
void fake_unregister_font(uint64_t token) { ++g_unregister_calls; g_last_unregistered = token; }

neui_render_backend_t make_backend() {
  neui_render_backend_t b{};
  b.register_font      = fake_register_font;
  b.register_font_file = fake_register_font_file;
  b.unregister_font    = fake_unregister_font;
  return b;
}

void reset_counters() {
  g_register_calls = g_register_file_calls = g_unregister_calls = 0;
  g_last_unregistered = 0;
  g_register_should_fail = false;
}

} // namespace

TEST_CASE("AssetStore font: allocate stores token + family, kind is FONT")
{
  reset_counters();
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;

  const uint8_t bytes[] = { 'O', 'T', 'T', 'O', 1, 2, 3, 4 };
  uint32_t slot = store.allocate_font(bytes, sizeof(bytes), &backend);
  REQUIRE(slot != 0);
  CHECK_EQ(g_register_calls, 1);

  AssetEntry* e = store.get_slot(slot);
  REQUIRE(e != nullptr);
  CHECK_EQ((int)e->kind, (int)NEUI_ASSET_KIND_FONT);
  CHECK_EQ(e->font_family, std::string("FakeFamily"));
  CHECK(e->font_token == 0xABCDu);
  // Bytes are owned for the loader's lifetime.
  CHECK_EQ((int)e->pixels.size(), (int)sizeof(bytes));
}

TEST_CASE("AssetStore font: get_font_family copies + truncates")
{
  reset_counters();
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;

  const uint8_t bytes[4] = { 0 };
  uint32_t slot = store.allocate_font(bytes, sizeof(bytes), &backend);
  REQUIRE(slot != 0);

  char buf[32] = { 0 };
  uint32_t full = store.get_font_family(slot, buf, sizeof(buf));
  CHECK_EQ((int)full, 10);                 // "FakeFamily"
  CHECK_EQ(std::string(buf), std::string("FakeFamily"));

  // Truncation: cap 5 => 4 chars + NUL, full length still reported.
  char small[5] = { 0 };
  uint32_t full2 = store.get_font_family(slot, small, sizeof(small));
  CHECK_EQ((int)full2, 10);
  CHECK_EQ(std::string(small), std::string("Fake"));
}

TEST_CASE("AssetStore font: get_font_family boundary - exact fit, cap 1, cap 0, null")
{
  reset_counters();
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;

  const uint8_t bytes[4] = { 0 };
  uint32_t slot = store.allocate_font(bytes, sizeof(bytes), &backend);
  REQUIRE(slot != 0);
  // "FakeFamily" is 10 chars; full length is always the return value.

  // Exact fit: cap == len + 1 holds the whole name + NUL, no truncation.
  char exact[11] = { 0 };
  CHECK_EQ((int)store.get_font_family(slot, exact, sizeof(exact)), 10);
  CHECK_EQ(std::string(exact), std::string("FakeFamily"));

  // One short: cap == len truncates by exactly one char (9 + NUL).
  char oneshort[10] = { 0 };
  CHECK_EQ((int)store.get_font_family(slot, oneshort, sizeof(oneshort)), 10);
  CHECK_EQ(std::string(oneshort), std::string("FakeFamil"));

  // cap == 1: only the NUL fits -> empty string, full length still reported.
  char one[1] = { '\x7f' };
  CHECK_EQ((int)store.get_font_family(slot, one, sizeof(one)), 10);
  CHECK_EQ((int)one[0], 0);

  // cap == 0: nothing is written (sentinel survives), full length reported.
  char sentinel = '\x7f';
  CHECK_EQ((int)store.get_font_family(slot, &sentinel, 0), 10);
  CHECK_EQ((int)sentinel, 0x7f);

  // null buffer: no write, no crash, full length reported.
  CHECK_EQ((int)store.get_font_family(slot, nullptr, 16), 10);
}

TEST_CASE("AssetStore font: get_font_family rejects non-FONT slots")
{
  reset_counters();
  AssetStore<FakeLoader> store;

  uint32_t comp = store.allocate_compound();
  REQUIRE(comp != 0);
  char buf[16] = { 0 };
  CHECK_EQ((int)store.get_font_family(comp, buf, sizeof(buf)), 0);
  // Invalid slot.
  CHECK_EQ((int)store.get_font_family(9999, buf, sizeof(buf)), 0);
}

TEST_CASE("AssetStore font: release_slot unregisters the token")
{
  reset_counters();
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;

  const uint8_t bytes[4] = { 0 };
  uint32_t slot = store.allocate_font(bytes, sizeof(bytes), &backend);
  REQUIRE(slot != 0);

  store.release_slot(slot, &backend);
  CHECK_EQ(g_unregister_calls, 1);
  CHECK(g_last_unregistered == 0xABCDu);
  // Slot is freed.
  CHECK(store.get_slot(slot) == nullptr);
}

TEST_CASE("AssetStore font: clear unregisters all live fonts")
{
  reset_counters();
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;

  const uint8_t bytes[4] = { 0 };
  uint32_t a = store.allocate_font(bytes, sizeof(bytes), &backend);
  uint32_t b = store.allocate_font_from_file("X.ttf", &backend);
  REQUIRE(a != 0);
  REQUIRE(b != 0);
  CHECK_EQ(g_register_file_calls, 1);

  store.clear(&backend);
  CHECK_EQ(g_unregister_calls, 2);
}

TEST_CASE("AssetStore font: backend failure yields slot 0")
{
  reset_counters();
  g_register_should_fail = true;
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;

  const uint8_t bytes[4] = { 0 };
  CHECK_EQ((int)store.allocate_font(bytes, sizeof(bytes), &backend), 0);
  CHECK_EQ((int)store.allocate_font_from_file("X.ttf", &backend), 0);
}

TEST_CASE("AssetStore font: null backend / bad args reject")
{
  reset_counters();
  AssetStore<FakeLoader> store;
  const uint8_t bytes[4] = { 0 };
  // No backend.
  CHECK_EQ((int)store.allocate_font(bytes, sizeof(bytes), nullptr), 0);
  // Backend lacking the font entry points.
  neui_render_backend_t empty{};
  CHECK_EQ((int)store.allocate_font(bytes, sizeof(bytes), &empty), 0);
  // Bad args.
  neui_render_backend_t backend = make_backend();
  CHECK_EQ((int)store.allocate_font(nullptr, 4, &backend), 0);
  CHECK_EQ((int)store.allocate_font(bytes, 0, &backend), 0);
}

TEST_CASE("AssetStore font: get_pixels_for_export rejects FONT")
{
  reset_counters();
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;

  const uint8_t bytes[4] = { 1, 2, 3, 4 };
  uint32_t slot = store.allocate_font(bytes, sizeof(bytes), &backend);
  REQUIRE(slot != 0);

  const uint8_t* px = nullptr;
  uint32_t w = 0, h = 0; float sc = 0.0f;
  CHECK_FALSE(store.get_pixels_for_export(slot, &px, &w, &h, &sc));
}

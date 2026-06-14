// INCR (chunked selection transfer) round-trip. Stands up two ClipboardX11
// instances on separate X connections to the same server: the OWNER (on a
// background thread that pumps its events) holds a payload far larger than a
// single X property, and the READER (main thread) requests it. A large payload
// forces the owner's INCR *send* path and the reader's INCR *receive* path, so
// one round-trip exercises both halves. Needs a live X display; built but NOT
// ctest-registered. Run ./tests/neui_incr_smoke.

#include "linux/clipboard_linux.h"

#include <X11/Xlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using neui_detail::ClipboardX11;
using neui_detail::DataItem;

static int g_fail = 0;
static void check(const char* what, bool ok)
{
  if (ok) printf("ok:   %s\n", what);
  else { printf("FAIL: %s\n", what); ++g_fail; }
}

// A deterministic ~1 MB payload (well past any single-property limit) so we can
// verify byte-for-byte integrity across the chunk boundaries.
static std::string make_payload(size_t n)
{
  std::string s; s.reserve(n);
  for (size_t i = 0; i < n; ++i) s.push_back(static_cast<char>('A' + (i % 26)));
  return s;
}

int main()
{
  Display* dpy_owner = XOpenDisplay(nullptr);
  Display* dpy_read  = XOpenDisplay(nullptr);
  if (!dpy_owner || !dpy_read) { printf("no display\n"); return 0; }

  const std::string payload = make_payload(1024 * 1024 + 12345);  // ~1 MB, non-round

  ClipboardX11 owner; owner.init(dpy_owner);
  ClipboardX11 reader; reader.init(dpy_read);

  // Owner: take CLIPBOARD with the big text + a big custom MIME, then pump its
  // connection so it can answer SelectionRequest + stream INCR chunks.
  DataItem item;
  item.set_format("text/plain;charset=utf-8", payload.data(),
                  static_cast<uint32_t>(payload.size()));
  std::string blob = payload + payload;   // ~2 MB custom format
  item.set_format("application/x-neui-test", blob.data(),
                  static_cast<uint32_t>(blob.size()));
  owner.write_item(item);

  std::atomic<bool> stop{false};
  std::thread pump([&] {
    while (!stop.load()) {
      while (XPending(dpy_owner)) {
        XEvent ev; XNextEvent(dpy_owner, &ev);
        owner.handle_event(ev);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let ownership settle

  // Reader: pull the big text back via get_text (INCR receive).
  int n = reader.get_text(nullptr, 0);
  std::string got;
  if (n > 1) {
    std::string buf(static_cast<size_t>(n), '\0');
    reader.get_text(&buf[0], n);
    got.assign(buf.data(), static_cast<size_t>(n - 1));
  }
  check("INCR text size round-trips", got.size() == payload.size());
  check("INCR text bytes intact",     got == payload);

  // Reader: pull the big custom MIME via read_item (INCR receive of a non-text
  // target).
  DataItem out;
  bool read_ok = reader.read_item(out);
  std::vector<uint8_t> recovered;
  int bn = out.get_format("application/x-neui-test", nullptr, 0);
  if (bn > 0) { recovered.resize(bn); out.get_format("application/x-neui-test", recovered.data(), bn); }
  check("read_item succeeded", read_ok);
  check("INCR custom MIME size round-trips", recovered.size() == blob.size());
  check("INCR custom MIME bytes intact",
        recovered.size() == blob.size() &&
        std::memcmp(recovered.data(), blob.data(), blob.size()) == 0);

  stop.store(true);
  pump.join();
  XCloseDisplay(dpy_read);
  XCloseDisplay(dpy_owner);

  printf(g_fail ? "\nINCR FAILED (%d)\n" : "\nINCR OK\n", g_fail);
  return g_fail ? 1 : 0;
}

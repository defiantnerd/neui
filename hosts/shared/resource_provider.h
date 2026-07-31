#pragma once

#include <fstream>
#include <string>

#include <neui/d/resource.h>

// Session-scoped binding of the optional client resource provider
// (NEUI_API_RESOURCE_CLIENT, <neui/d/resource.h>) plus the "ask the client,
// then read the file" helper every byte-native load path shares.
//
// Why a value type rather than the client pointer directly: AssetStore needs the
// provider on the image path but must not learn about client tokens or the
// per-host Session type, so it holds a copy of this two-word struct that each
// host fills in at session-create time.
//
// ODR-safe: header-only, everything inline.

namespace neui_detail
{
  // Read a whole file into `out`. Kept here so the four hosts stop duplicating
  // the ifstream dance (they each had their own copy for font loading).
  inline bool read_file_bytes(const char* path, std::string& out)
  {
    if (!path || !*path) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    return !out.empty();
  }

  struct ResourceProvider
  {
    neui_resource_client_t* client = nullptr;
    void*                   token  = nullptr;

    // Does the client answer this kind at all? kinds_mask == 0 means all.
    bool serves(neui_resource_kind_t kind) const
    {
      if (!client || !client->provide) return false;
      return client->kinds_mask == 0u ||
             (client->kinds_mask & NEUI_RESOURCE_MASK(kind)) != 0u;
    }

    // Ask the client for `name`. On a hit, invoke fn(data, len, scale) -> bool
    // and release the bytes before returning, so the borrow never outlives the
    // call. Returns fn's verdict, or false when the client declined.
    //
    // fn returning false means "these bytes were unusable" and is deliberately
    // indistinguishable from a decline to the caller: a provider that hands back
    // a corrupt blob must not shadow a perfectly good file, so every caller
    // falls through to its own resolution either way.
    template <typename Fn>
    bool with_bytes(neui_resource_kind_t kind, const char* name,
                    float scale_hint, const char* base_dir, Fn&& fn) const
    {
      if (!serves(kind) || !name || !*name) return false;

      neui_resource_request_t req{};
      req.kind       = kind;
      req.name       = name;
      req.scale_hint = scale_hint;
      req.base_dir   = base_dir;

      neui_resource_bytes_t got{};
      if (!client->provide(token, &req, &got)) return false;

      // release() is promised exactly once for every provide() that returned
      // true (<neui/d/resource.h>), so the pairing has to survive an exception
      // unwinding out of `fn`: every consumer ends in an allocation that can
      // throw (a decode into new[] / a std::string assign), and a client that
      // allocated or mapped this blob per request would leak it.
      struct ReleaseGuard {
        neui_resource_client_t*      c;
        void*                        t;
        const neui_resource_bytes_t* b;
        ~ReleaseGuard() { if (c->release) c->release(t, b); }
      } guard{ client, token, &got };

      if (!got.data || got.len == 0) return false;
      return fn(got.data, got.len, got.scale > 0.0f ? got.scale : 1.0f);
    }

    // The byte-native kinds (FONT / COMPONENT / SIDECAR): ask the client, then
    // fall back to reading the file. `path` doubles as the resource name, which
    // is what the client passed in to begin with.
    bool read_bytes(neui_resource_kind_t kind, const char* path,
                    std::string& out, const char* base_dir = nullptr) const
    {
      out.clear();
      const bool from_client = with_bytes(
          kind, path, 0.0f, base_dir,
          [&out](const uint8_t* data, uint32_t len, float) {
            out.assign(reinterpret_cast<const char*>(data), len);
            return true;
          });
      if (from_client) return true;
      return read_file_bytes(path, out);
    }
  };

} // namespace neui_detail

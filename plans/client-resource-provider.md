# Client resource provider (NEUI_API_RESOURCE_CLIENT)

**Status: EXECUTED 2026-07-31.** Design reviewed and approved, then implemented end to end -
see "Implementation notes" at the bottom for what shipped and the one documented deviation.
Reference docs now live in `docs/rendering-and-assets.md` ("Client resource provider").

## Goal

Let a client supply resource bytes for a name. The host asks the client first, with hints about
what it is looking for, and falls back to its own filesystem / embedded-resource resolution when
the client declines. The client returns the complete byte blob (no streaming, no partial reads).

(This section was written before the order was decided; see "Decisions taken" below - the review
settled on client-first rather than the fallback-only shape sketched here.)

This closes a gap that already bites: the LVGL host has no resource loader at all, so
`neui_example`'s `.rc`-embedded `lemur.jpg` shows the failed-load placeholder there while the
native Windows host loads it from the EXE. More generally it lets a client ship assets inside
its own container (plugin bundle, VST3 resource dir, encrypted pack, generated at runtime)
without neui knowing anything about that container.

## Scope decision: which resources

Everything neui currently reads from the filesystem, so the extension is not immediately
half-useful:

| Resource | What reads it today | Bytes are | Consumer |
|---|---|---|---|
| Image (bitmap / filmstrip) | `AssetStore::allocate_from_file` + `resolve_path` (`hosts/shared/asset_store.h:169,754`), `AssetManager::get_bitmap` (`hosts/crossplatform/asset_manager.cpp:24`) via `Loader::load` -> `platform_load_image` / WIC / ImageIO | encoded PNG / JPG / BMP | needs a **decode-from-memory** path (see below) |
| Font | `as_create_font_from_file` (`hosts/crossplatform/widgets.cpp:2204`, `hosts/win32/widgets.cpp:5496`, `hosts/macos/widgets.mm:2444`, `hosts/ios/widgets.mm:1647`) via `std::ifstream` | TTF / OTF / TTC | already bytes-native (`create_font(data, len)`) |
| Component | `as_create_component_from_file` (`hosts/crossplatform/widgets.cpp:2260`) via `std::ifstream` | UTF-8 JSON | already bytes-native (`create_component_from_string`) |
| Filmstrip sidecar | `hosts/shared/filmstrip_recognize.h:159` via `std::ifstream` | UTF-8 JSON | parsed in place |

Fonts, components and sidecars are nearly free - they read a file into a buffer and hand the
buffer on, so the hook is a two-line substitution. Images are the expensive one, because the
current seam is *path in, decoded pixels out*; there is no way to feed it bytes.

## Proposed API

New header `include/neui/d/resource.h`, following the `NEUI_API_*_CLIENT` pattern
(`menu.h` / `theme.h` / `grid.h`): the host calls `client->get_interface(token, ...)` once at
session-create time and caches the pointer, exactly like `_menu_client` / `_grid_client` /
`_theme_client` in `Session::Session` (`hosts/crossplatform/host.cpp:227-263`).

```c
#define NEUI_API_RESOURCE_CLIENT \
  "com.defiantnerd.neui.extension.resource.client/0"

// What the host was trying to load. Reserve new values at the next unused
// integer so old client builds stay forward-compatible (same rule as
// neui_asset_kind_t).
typedef enum neui_resource_kind {
  NEUI_RESOURCE_KIND_NONE      = 0,
  NEUI_RESOURCE_KIND_IMAGE     = 1,  // encoded PNG / JPG / BMP bytes
  NEUI_RESOURCE_KIND_FONT      = 2,  // TTF / OTF / TTC bytes
  NEUI_RESOURCE_KIND_COMPONENT = 3,  // component JSON (UTF-8)
  NEUI_RESOURCE_KIND_SIDECAR   = 4,  // filmstrip layout JSON (UTF-8)
  // reserved: _SVG = 5, _AUDIO = 6, ...
} neui_resource_kind_t;

typedef struct neui_resource_request {
  neui_resource_kind_t kind;
  // The name exactly as the client originally passed it (to create_from_file,
  // set_text on an IMAGE, create_font_from_file, ...). NOT a host-rewritten
  // variant: no "@2x" suffix, no base_dir prefix. The client looks up its own
  // container by the name it knows.
  const char*          name;
  // Display scale the host is resolving for (1.0 / 2.0 / 3.0). IMAGE only;
  // 0.0 for every other kind. The client may return any resolution it has and
  // reports what it actually returned in neui_resource_bytes_t::scale.
  float                scale_hint;
  // For a resource referenced from inside a component document, the component's
  // base_dir (so a client can disambiguate same-named assets of two components).
  // NULL otherwise.
  const char*          base_dir;
} neui_resource_request_t;

typedef struct neui_resource_bytes {
  const uint8_t* data;   // borrowed by the host for the duration of the call only
  uint32_t       len;
  // IMAGE only: HiDPI factor of the returned pixels (1.0 / 2.0 / 3.0), i.e.
  // the same meaning as create_bitmap's `scale`. 0.0 = "treat as 1.0".
  // Ignored for other kinds.
  float          scale;
  // Opaque client cookie echoed back to release(). Lets the client hand out a
  // heap buffer, an mmap, or a static blob and know which on release.
  void*          release_token;
} neui_resource_bytes_t;

typedef struct neui_resource_client {
  uint32_t neui_version;

  // (As shipped: called BEFORE the host's own lookup - decision 1.) Fill *out
  // and return true, or return false to let the host resolve it its own way.
  // The host copies / decodes the bytes before returning and then calls
  // release(), so `data` need only stay valid for the duration of this call.
  bool (NEUI_ABI *provide)(void* token,
                           const neui_resource_request_t* req,
                           neui_resource_bytes_t* out);

  // Called exactly once for every provide() that returned true, before the
  // host's originating API call returns. May be NULL (static blobs).
  void (NEUI_ABI *release)(void* token, const neui_resource_bytes_t* res);
} neui_resource_client_t;
```

### Why these choices

- **Borrow + `release`, not host-owns.** Every consumer already copies or transforms the bytes
  immediately (`create_font` copies, `create_component_from_string` parses, an image decodes to
  new pixels), so the host never needs to retain the blob. Borrowing lets a client point
  straight at an `RT_RCDATA` resource or a `std::vector` member with zero copies on its side,
  and `release_token` still supports a freshly-allocated buffer. `release` is called before the
  triggering API call returns, so lifetime reasoning is trivial on both sides.
- **One call per logical name, with `scale_hint`** - this is the answer to the resolution-hint
  question. The host does *not* ask three times for `knob@3x.png`, `knob@2x.png`, `knob.png`.
  It asks once for `knob.png` with `scale_hint = 2.0` and the client returns whatever variant it
  has, declaring the real scale in `out->scale`. The client owns its own naming convention
  (`@2x`, `_2x`, a subdirectory, one master rendered on demand); the host's `@Nx` filename
  convention stays a *filesystem* convention and does not leak into the extension.
- **`name` is the client's original string.** A client that stores `"knob.png"` should not have
  to recognise `"assets/knob@2x.png"`. `base_dir` is passed separately for the component case
  rather than pre-joined.
- **No `struct_size` field.** Matches the existing `neui_component_env_t` / `neui_component_param_t`
  convention; the `/0` suffix in the interface name is the version gate. The request struct is
  host-allocated and passed by const pointer, so appending a field is safe for old clients as
  long as they never copy it by value (documented in the header). See open question 3.

## Resolution order

**DECIDED: client first, then the host tries itself.** Matches the existing
`neui_component_env_t::resolve_asset` convention and enables overriding shipped files (user
skins, themed packs) rather than only filling gaps.

```
create_from_file("knob.png") / IMAGE set_text / create_font_from_file / ...
  1. resource_client->provide({IMAGE, "knob.png", scale_hint}, &bytes)
       hit  -> platform_load_image_bytes -> pixels, entry.scale = bytes.scale, release(), done
       miss -> fall through
  2. resolve_path(name, scale): @Nx candidate ladder -> Loader::load (win32 also tries the
     RT "PNG" resource inside load_image_bgra8_w32, so the effective order is
     client -> embedded resource -> file)
  3. neither -> asset_none (today's behaviour, unchanged)
```

A client that does not implement the interface sees zero cost and zero behaviour change, exactly
as before. A client that *does* implement it is now on the hot path of every resource load, not
just the failing ones - which is why the resolution cache below is a hard prerequisite rather
than an optimisation, and why `provide` must be cheap and must not call back into neui.

## Host-side plumbing

1. **`Session`** gains `neui_resource_client_t* _resource_client` fetched in the constructor next
   to `_menu_client` / `_grid_client`. Same in the native win32 / macOS / iOS hosts.
2. **`AssetStore<Loader>`** (`hosts/shared/asset_store.h`) gains a settable provider, because the
   `Loader` is a compile-time *static* policy and the client callback is per-session state:
   ```cpp
   using ProvideFn = bool (*)(void* user, const ResourceReq&, ResourceBytes&);
   using ReleaseFn = void (*)(void* user, const ResourceBytes&);
   void set_resource_provider(ProvideFn, ReleaseFn, void* user);
   ```
   `ResourceReq` / `ResourceBytes` are small internal mirrors of the public structs so
   `hosts/shared/` keeps compiling without the public header pulling in host types (it already
   includes the public headers, so plain reuse is also fine - open question 4).
   `resolve_path` and `scale_of_resolved` are currently `static`; the provider path needs
   instance state, so `allocate_from_file` grows a non-static resolution step. The two static
   helpers stay for the derived path-keyed cache in the xpl `AssetManager`.
3. **`Loader` policy** gains `load_memory(const uint8_t*, size_t, uint32_t* w, uint32_t* h)`
   alongside `load` / `free_pixels`, in all four policies (`XplImageLoader`, `W32ImageLoader`,
   `MacOSImageLoader`, `IOSImageLoader`).
4. **Fonts / components / sidecars**: one `if (!read_file(...)) ask_client(...)` at each of the
   sites tabulated above. Note `as_create_font_from_file` is duplicated per host - factor the
   read+fallback into a shared helper in `hosts/shared/` rather than editing four copies.

## New platform seam: decode from memory

`hosts/crossplatform/platform.h` gains the sibling of the existing loader:

```c
  // Decode an in-memory encoded image (PNG / JPG / BMP / ...) to BGRA8
  // premultiplied pixels. Same ownership contract as platform_load_image:
  // release with platform_free_image. Returns nullptr on failure.
  uint8_t* platform_load_image_bytes(const uint8_t* data, size_t len,
                                     uint32_t* width_out, uint32_t* height_out);
```

Per platform, all four already have the primitive:

- **Linux + LVGL**: `stbi_load_from_memory` in `hosts/shared/image_loader_stb.h`. Factor the
  premultiply loop so `load_image_bgra8_stb` and a new `load_image_bgra8_stb_memory` share it.
- **win32**: `image_loader_win32.h` already builds an `IStream` over a buffer with
  `SHCreateMemStream` for the resource path - extract that into
  `load_image_bgra8_w32_memory(data, len, ...)` and have the resource path call it.
- **macOS / iOS**: `CGImageSourceCreateWithData(CFDataCreateWithBytesNoCopy(...))`, mirroring
  the existing `load_image_bgra8_macos`.
- **null**: returns nullptr.

`stbi_load_from_callbacks` (a true pull stream) is deliberately **not** used: stb is not
incremental (it can only rewind inside a 128-byte sniff buffer) and the whole point of the
"client provides the complete blob" simplification is that we never need it. If a streaming
variant is ever wanted, it can be added as a second entry point without disturbing this one.

## Prerequisite: the per-paint resolve cost (must fix, or this hook fires every frame)

`AssetManager::get_bitmap` and `get_logical_size` call `resolve_path(name, scale)` on **every
call**, i.e. once per IMAGE widget per paint - and `resolve_path` tests each candidate by fully
decoding it and throwing the pixels away (`asset_store.h:772-779`). So an IMAGE widget already
pays a full image decode per frame today, before its cache lookup. That is a pre-existing
performance bug, but it becomes a correctness problem for this design: bolting the provider onto
`resolve_path` unchanged would call `provide` (and decode its bytes) once per frame per widget,
and would call it forever for a genuinely missing resource.

With client-first this gets sharper: without a cache, a client that implements the interface would
be called once per IMAGE widget per frame even for resources that resolve perfectly well from
disk. So the design requires, as step 1 of implementation:

- A **resolution cache** keyed on `(name, scale)` storing the outcome: `client` (bytes came from
  the provider), the resolved filesystem variant + actual scale, or `missing`. All three cached,
  so `provide` is called **at most once per (name, scale)** per session.
- **DECIDED for v0: negative results are sticky.** They clear on a DPI change (which already
  forces re-resolution at the new scale) and at session teardown. No explicit invalidation entry
  point yet; if a client needs to publish resources late, add a "forget resolution failures" call
  on `NEUI_API_ASSETS` as a vtable-append later.

This is worth doing on its own merits - it removes a decode per frame per IMAGE widget.

## Threading and reentrancy contract (to document in the header)

- `provide` / `release` are called on the UI thread, synchronously, inside the neui call that
  triggered the load.
- That call is **sometimes a paint**: the xpl path-keyed tier loads lazily from `get_bitmap`
  during widget paint. So `provide` must not call back into any neui API, and must not block for
  long - on the LVGL host a paint runs under the global LVGL lock, so blocking there stalls the
  refresh (and calling into neui would deadlock). Document as: do I/O only, return quickly, no
  neui calls.
- With the resolution cache above, the paint-time call happens at most once per resource, which
  makes this constraint tolerable. Without it, it would not be.

## Rejected alternatives

- **Client returns a handle, not bytes** (i.e. extend `neui_component_env_t::resolve_asset` to a
  session-wide hook). Requires the client to have already built the asset, which needs the very
  bytes path we are adding; and it cannot serve fonts / sidecars, which are not assets.
- **A VFS abstraction** (client implements open/read/seek/close). More power than the stated
  requirement, needs the streaming decode path stb cannot provide, and pushes lifetime and
  reentrancy complexity into the client. The blob form can be widened to this later without
  breaking the interface.
- **Client pre-registers blobs up front** (`register_resource(name, bytes)`). Simple, but forces
  the client to eagerly load everything it *might* need, which defeats the purpose for large
  packs. Worth noting it is trivially implementable *on top of* this pull interface by a client.

## Decisions taken (all approved 2026-07-31 - this section is the binding spec)

1. **Order: client first, then the host.** The deciding argument is the motivating deployment, not
   skinning: for a client whose assets live in a container (plugin bundle, embedded resource, MCU
   flash) host-first would pay a guaranteed miss ladder - up to three failed decodes on paths that
   will never exist - before asking the one place the bytes actually are. On an embedded target
   there may be no filesystem at all. (Section "Resolution order".)
2. **Negative cache: sticky for v0**, cleared on DPI change / session teardown. No invalidation
   entry point yet. (Section "Prerequisite".)
3. **Plain structs, no `struct_size`.** Matches `neui_component_env_t`; the request is
   host-allocated and const, so appending a field stays safe for old clients, and the `/0` suffix
   in the interface name is the version gate. Header documents "never copy the request by value".
4. **`KIND_SIDECAR` stays its own value**, distinct from `KIND_COMPONENT`, so a client can answer
   off `kind` alone without parsing the name.
5. **`out->scale` is the whole scale protocol.** One call per (name, scale bucket); the client
   picks what to return and declares its scale. No "ask me again at 1.0" round trip.
6. **All four hosts in v0** (xpl + win32 + macOS + iOS). Images land in the shared `AssetStore`
   that all four already use; fonts / components go through one shared read-or-ask helper instead
   of four copies. The only genuinely per-host work is `load_memory` in each `Loader` policy.
7. **Both component hooks are kept, chained `env.resolve_asset` -> `provide` -> filesystem.**
   Primary reason: it is the only strictly backward-compatible order - `resolve_asset` already
   runs first and already beats path mode, so inserting `provide` between it and the filesystem
   changes nothing for existing code. Secondary: they answer different questions and neither
   subsumes the other. `resolve_asset` answers "which existing handle does this name mean" and can
   return a runtime-built compound / painted SURFACE / self-tagged filmstrip that has no byte form,
   with borrowed handles shareable across documents; `provide` answers "here are the bytes". The
   one overlapping case (component references an image whose bytes the client has embedded) is
   handled *badly* by `resolve_asset` today - the client has bytes, not a path, so it would need
   `create_bitmap` with pre-decoded pixels, i.e. its own image decoder. `neui_component_env_t` is
   NOT deprecated: it is per-call with a per-call `user`, so two documents can load with different
   asset tables, which a session-wide provider structurally cannot express.
   Both hooks see the **same** name string - the raw entry from the component's JSON `assets` map,
   never a `base_dir`-joined path - so one client lookup table can serve both.
8. **`kinds_mask` on the interface** (0 = all kinds). With client-first, a client that only
   overrides images would otherwise be called for every font and component load.
9. **Client bytes that fail to decode / parse are treated as a miss**, and resolution continues to
   the filesystem ladder rather than failing the load. Client-first means a buggy provider could
   otherwise shadow a perfectly good file, and falling through costs nothing.
10. **`hosts/shared/` reuses the public request / bytes structs** rather than mirroring them (it
    already includes the public headers for `neui_asset_kind_t` / `neui_render_backend_t`). The
    store holds `void* user` + function pointers; each host installs a thunk that calls
    `_resource_client->provide(_token, ...)`, so the store never learns about client tokens.

## Implementation order (once approved)

1. Resolution cache + negative caching in `AssetStore` / `AssetManager` (independent win, no API
   change). Tier-1 test with a counting fake `Loader`.
2. `platform_load_image_bytes` seam + the four platform impls + `Loader::load_memory`. No
   behaviour change yet; `image_loader_win32.h`'s resource path switches to it as the first
   consumer.
3. `include/neui/d/resource.h` + `Session::_resource_client` acquisition in all hosts.
4. Wire the fallback: images (via `AssetStore`), then fonts / components / sidecars (via a shared
   read-or-ask helper).
5. LVGL host: no `.rc` support of its own, so it becomes the natural demo - `neui_example`'s
   `lemur.jpg` loads once a resource client is supplied.
6. Docs: `docs/rendering-and-assets.md` gets a "Client resource provider" section; CLAUDE.md's
   optional-client-interface list gains `_RESOURCE_CLIENT`; example client in `examples/`.
7. Tier-1 tests: fallback order, at-most-once `provide` per (name, scale), scale reporting,
   `release` pairing, negative caching.

---

## Implementation notes (2026-07-31)

Shipped in the planned order. Verified on Windows: the normal D2D build, both LVGL builds (32bpp
and RGB565) and the Tier-1 suite. macOS / iOS / Linux code was written against the existing
loaders in those trees but not compiled here (no toolchain on this machine) - the edits are the
mechanical mirror of the win32/stb ones.

**New files**
- `include/neui/d/resource.h` - the public interface (also added to `neui.h` and CLAUDE.md's list).
- `hosts/shared/resource_provider.h` - `ResourceProvider` (the binding each host installs) plus
  `with_bytes` / `read_bytes` / `read_file_bytes`.
- `tests/test_resource_provider.cpp` - 8 Tier-1 cases (order, at-most-once probing, negative
  caching + `clear_image_routes`, scale bands, `kinds_mask`, `release` pairing, undecodable-bytes
  fallthrough, absent-client no-op).
- `examples/resource_client_example.cpp` / `neui_resource_client_example` - builds a 96x96 BMP in
  memory at startup and serves it under `generated.bmp`, a name that exists nowhere on disk. Drawn
  twice: by an `NEUI_W_IMAGE` widget (`set_text`, lazy resolve on first paint) and by a CUSTOMDRAW
  through an explicit `create_from_file` handle. A BMP avoids needing an encoder in the example.

**Resolution cache** (step 1, the prerequisite): `AssetStore::image_route(name, scale)` returns a
cached `ImageRoute`, keyed on the scale BUCKET (`scale_bucket`: <=1 / <=2 / >2 - all `resolve_path`
actually branches on, so 1.25 / 1.5 / 1.75 share one entry). `AssetManager::get_bitmap` and
`get_logical_size` now route through it, which removes the per-frame-per-IMAGE-widget image decode
that `resolve_path` was paying to answer "which `@Nx` variant?". `load_pixels` takes an
`ImageRoute` instead of a path so the client-bytes branch shares it.

**Decode-from-memory seam** (step 2): `platform_load_image_bytes` (declared in `platform.h`,
implemented in all five platform layers) + `Loader::load_memory` in all four policies. The
premultiply / rasterise tails were factored so path and memory forms share them:
`stb_rgba_to_bgra8_premul`, `wic_decode_frame_w32`, `cg_image_to_bgra8_premul` (x2 - the macOS and
iOS headers stay mutually exclusive per `TARGET_OS_IPHONE`, same ODR reasoning as
`free_image_bgra8`). `image_loader_win32.h`'s embedded-resource branch now decodes through
`load_image_bgra8_w32_memory`, so it is the first consumer of the new entry point.

**Hook sites**: images in `AssetStore::probe_image_route`; fonts in
`AssetStore::allocate_font_from_file` (client bytes go to the in-memory `allocate_font`, so all
four hosts get it from the shared store rather than four copies); component documents at each
host's `create_component_from_file` (three lines each, via `read_bytes`); filmstrip sidecars via
`filmstrip_discover_from_path`'s new optional `const ResourceProvider*`.

### Deviation from the approved spec (one) - SINCE CLOSED

Decision 7 said both component hooks see the raw `assets`-map name plus `base_dir` separately. As
first implemented, an image referenced from inside a component document reached the provider as the
**`base_dir`-joined path**, because `build_component` holds only the public `neui_asset_api_t`,
whose `create_from_file` takes a path, so the join happened before the store (and therefore the
provider) was reached.

**Closed the same day** by follow-up 1, the `ComponentApis` byte hook:

- `ComponentApis` gained `bitmap_from_name(user, name, base_dir)` + `user`. When set it fully
  replaces the `create_from_file(join_path(...))` branch in `build_component`'s `resolve_asset`
  (going on to try the path form as well would only re-probe the same miss under a second cache
  key). When unset - the Tier-1 fakes, any other embedder - the loader behaves exactly as before.
- All four hosts install `component_bitmap_from_name`, a three-line thunk calling
  `AssetStore::allocate_from_file(name, scale, base_dir)`.
- `AssetStore` resolution became `base_dir`-aware end to end: `ImageRoute::base_dir`, `route_key` /
  `client_cache_key` scoped by it (two documents may use one name for different images), the
  provider request carries it (so `neui_resource_request_t::base_dir` is now live rather than
  permanently NULL), and only the filesystem ladder joins it onto the name (`fs_name_of`, reusing
  `cl_detail::join_path`).
- `env.resolve_asset` still runs first and is unaffected; the chain is unchanged in order, only in
  what the provider is told.

### Code-review fixes (2026-07-31, same day)

A `/code-review --max` pass over the branch found the following, all fixed:

- **Client routes collapsed across scale bands.** `ImageRoute::cache_key` for a client route was
  `"\x01client\x01" + name` with no band in it, so the xpl `AssetManager`'s path-keyed `_cache`
  served the first-resolved band's bitmap at every other scale. Now `client_cache_key(name, band)`.
- **`decode_route` re-asked with the wrong hint and dropped the reply's scale.** It passed the
  cached `route.scale` as `scale_hint` (not the display scale) and ignored the `float` the provider
  declared on the second call, so the entry could record one variant's scale while holding
  another's pixels. `ImageRoute` now carries `req_scale`, and `decode_route` reports the decoded
  scale out.
- **No filesystem fallback after the probe.** A route cached as `from_client` failed permanently if
  the provider later declined, defeating decision 9 for every load but the probe. `decode_route`
  now falls back to the `@Nx` ladder.
- **Sticky misses had no reachable invalidation** and decision 2's "clears on a DPI change" was
  false for 125% <-> 200% (same band). `clear()` now drops routes with the assets, and an explicit
  `allocate_from_file` re-probes a cached miss (the per-frame tier still does not) - which is
  follow-up 2 solved without a new public API, since the regression only ever bit explicit,
  client-initiated loads.
- **`ImageRoute::path` meant two different things** depending on `from_client`; split into `name`
  and `file_path`.
- **The route cache was a `std::map`** with a `pair<string,int>` key on the per-frame paint path;
  now `unordered_map` with the band folded into the key string.
- **`with_bytes` leaked the borrow if `fn` threw** (every consumer ends in an allocation that can);
  the provide/release pair is now RAII.
- **`wic_decode_frame_w32` had no overflow guard** on `stride * h` - reachable from client-supplied
  bytes, where the sibling stb path is guarded. Now bounded in 64 bits, with a nothrow allocation
  so an untrusted blob reports failure instead of throwing out of a paint.
- **`Session::set_focus` invalidated nothing when clearing focus** (it read `_focused_widget` after
  overwriting it); the LVGL arm added `prev_focus` but left the other one broken. Hoisted.
- **The LVGL mirror sync hid whole subtrees**: a widget that stopped painting had its hidden mirror
  used as its children's container. It now hands them to its own container with the offset folded
  in, matching `paint_widgets_recursive` (whose only descent gate is `visible`). Reparenting also
  switched from delete + rebuild to `lv_obj_set_parent`, which no longer leaves descendant
  `MirrorEntry::obj` pointers dangling mid-pass.
- **`d/resource.h` documented a contract the code did not honour** (raw name + `base_dir` for
  component assets; "at most once per resource per session" for every kind). The header now states
  the shipped behaviour, including `base_dir` being reserved-and-always-NULL.
- Dead `<fstream>` / `<sstream>` includes removed from the five files whose `ifstream` use moved
  into `resource_provider.h`.

### Follow-ups

1. ~~The `ComponentApis` byte hook~~ - done, see "Deviation ... SINCE CLOSED" above. `base_dir` is
   live; the only remaining name-shape gap is that FONT / COMPONENT / SIDECAR loads still carry no
   `base_dir` (they are addressed by path today and nothing references them from inside a document).
2. ~~Invalidation beyond sticky-until-DPI-change~~ - addressed by the re-probe on explicit loads plus
   `clear()`. A `NEUI_API_ASSETS` "forget resolution failures" entry point is still the answer if a
   client ever needs to re-resolve what the *per-frame* tier cached as missing.
3. ~~`allocate_from_file` decodes twice on a cold FILESYSTEM load~~ - closed: `resolve_path` takes an
   optional `DecodedImage*` and the winning candidate's pixels are parked for the load that wanted
   them. Same mechanism removes the second `provide()` on a cold client load.
4. Compile the macOS / iOS / Linux edits.

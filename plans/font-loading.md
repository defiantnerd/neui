# Plan: client-loaded fonts

Lets a client hand neui a font file (path or in-memory bytes) so it
becomes usable for text rendering, without the font being installed
system-wide. Primary use case: an audio plugin that bundles a `.ttf`
in its resource folder and wants its UI text drawn in that face on a
host machine that has never seen it.

The font binds to the next reserved asset kind -
`NEUI_ASSET_KIND_FONT = 7` (already reserved in `assets.h:65`) - so a
loaded font is an ordinary `neui_asset_t` with a session-scoped
lifetime, acquired like every other media handle.

## Mechanism: push, not pull (resolved)

The client **registers** its fonts up front; neui never calls back
into the client to *request* a missing font. Rationale (full reasoning
captured in the shaping discussion):

- A plugin already knows exactly which files it ships - there is
  nothing to discover, so the laziness pull would buy is worthless
  here.
- Pull's trigger point is font *resolution*, which happens during
  paint / measure (the hot path). A synchronous client callback there,
  potentially forcing a DirectWrite custom-collection rebuild
  mid-frame, is the worst place for it.
- Push is purely additive to today's behaviour: unknown family names
  still fall back to the host default; registration just widens the set
  of names that resolve.

## Reference by family name (resolved)

The backend font stack is **already name-based** -
`push_font(family, weight)` + per-call size (`renderer.h:212-230`),
and `NEUI_ATTR_FONT_FAMILY` / `_FONT_WEIGHT` / `_FONT_SIZE` on
text-bearing widgets. So registration introduces **no new draw-path
addressing**: it makes a family name resolvable, and the entire
existing pipeline (attrs, `push_font`, the per-backend font caches
keyed by family+weight+size) is untouched.

The `neui_asset_t` handle exists only to own the *registration
lifetime*. The client references the font by its family-name string,
exactly as it would a system font. One query is added so the client
can learn the internal family name if it differs from the filename.

### Resolved decisions

- **Push only.** No pull/request-on-miss callback. (A deferred opt-in
  fallback hook is noted in Out of scope.)
- **Family-name reference.** No handle-based addressing in the draw
  path; no alias mechanism. Collisions are documented last-wins -
  plugin authors that fear a clash ship a uniquely-renamed family
  (common practice anyway).
- **Both path and memory.** `create_font` (bytes) is the general
  primitive; `create_font_from_file` (path) is the convenience form and
  is more robust on the backends that prefer URL/path registration.
- **Multi-weight families compose by name.** A family is several files
  (Regular / Bold / Italic); the client registers each file, the
  backends coalesce faces sharing a family name, and
  `push_font(family, weight)` selects the weight. One `create_font*`
  call per file.
- **Italic out of scope.** The font stack is (family, weight) only -
  italic is not selectable today and this work does not add it.
  Loading an italic-only file works; asking for italic does not.
- **Unsupported backends return `asset_none`** (null backend), the same
  graceful-degradation precedent SURFACE set.

## The two-pronged seam

Text in neui is drawn two ways, and a loaded font must reach both:

1. **Painted text** (all `neui-backend-*` draw paths): the xpl host on
   every platform, plus the d2d/cg painted surfaces. Covered by a new
   backend entry point - Part B.
2. **Native-control text** (HFONT / NSFont in the *native* win32 and
   macOS hosts only - the xpl host paints its own text, and Linux is
   xpl-only). Covered as follows:
   - **macOS native**: *free.* Registering through `CTFontManager`
     with process scope (Part B, cg) makes the family visible to
     `NSFont fontWithName:` process-wide. No extra work.
   - **Win32 native**: *needs a second registration.* A DirectWrite
     custom collection does **not** expose the face to GDI
     `CreateFontW`. The win32 native host additionally calls
     `AddFontMemResourceEx` (memory) / `AddFontResourceExW(FR_PRIVATE)`
     (path) so native HFONT widgets pick up the family - Part E.

This asymmetry (one prong on macOS/Linux, two on Windows) is the main
cross-cutting cost and is why the win32 native host carries an extra
step the others don't.

## Part A - Public API

### Append to `include/neui/d/assets.h`

```c
// Discriminator: bind the reserved slot.
NEUI_ASSET_KIND_FONT = 7,

// Register an in-memory font (TTF/OTF bytes). The framework copies the
// bytes and owns the copy for the asset's lifetime (FreeType /
// DirectWrite in-memory loaders require the buffer stay live). Returns
// neui_asset_none if the backend cannot register fonts (null) or the
// data is not a usable font. Vtable entries appended AFTER the last
// existing method (ABI stability).
neui_asset_t (NEUI_ABI *create_font)(neui_session_t session,
                                     const uint8_t* data,
                                     uint32_t       len);

// Path convenience form (.ttf/.otf/.ttc). Resolution is immediate.
neui_asset_t (NEUI_ABI *create_font_from_file)(neui_session_t session,
                                               const char*    path);

// Write the registered family name into out_buf (UTF-8, NUL-terminated,
// truncated to cap). Returns the full length (excluding NUL) or 0 if
// the asset is not a FONT. Lets the client discover the name to pass to
// NEUI_ATTR_FONT_FAMILY when it differs from the filename.
uint32_t (NEUI_ABI *get_font_family)(neui_session_t session,
                                     neui_asset_t   font,
                                     char*          out_buf,
                                     uint32_t       cap);
```

`assets->destroy(font)` unregisters (best-effort - a backend may not
fully release a face still referenced by a cached text format; that is
acceptable, the slot is freed and the name stops resolving for new
draws). Otherwise the font is released at session/context teardown like
any asset.

## Part B - Backend vtable

### Append to `neui_render_backend_t` in `include/neui/d/renderer.h`

```c
// Register a font from memory. The backend reads the family name out of
// the font data and makes that family resolvable by push_font /
// draw_text. out_family receives the family name (UTF-8, truncated to
// cap, NUL-terminated). Returns true on success. The backend does NOT
// own data - the caller (asset store) keeps the bytes alive for the
// font token's lifetime. Returns an opaque token in *out_token for
// unregister_font; 0 on failure.
bool (NEUI_ABI *register_font)(const uint8_t* data, uint32_t len,
                               char* out_family, uint32_t cap,
                               uint64_t* out_token);

// Path variant (some backends register URLs/paths more robustly).
bool (NEUI_ABI *register_font_file)(const char* path,
                                    char* out_family, uint32_t cap,
                                    uint64_t* out_token);

// Best-effort unregister of a previously registered font.
void (NEUI_ABI *unregister_font)(uint64_t token);
```

Notes:
- These are **factory/process level, not per-context** - registration
  affects font resolution for every render context the backend serves.
  Unlike `create_offscreen_context`, they take no `ctx`.
- Font-cache keys (already family+weight+size) need no change;
  registration only widens name resolution.

### D2D (`backends/d2d/d2d_backend.cpp`)

- In-memory: `IDWriteFactory5::CreateInMemoryFontFileLoader` +
  `CreateFontFileReference` over the caller-owned bytes, feed an
  `IDWriteFontSetBuilder1`, `CreateFontSet`, then
  `CreateFontCollectionFromFontSet` -> a custom `IDWriteFontCollection1`
  the backend consults *before* the system collection when resolving a
  family in `push_font` / text-format creation.
- Read the family name via `IDWriteFontSet`/`IDWriteLocalizedStrings`
  for `out_family`.
- `register_font_file`: `CreateFontFileReference(path)` instead of the
  in-memory loader; otherwise identical.
- Token = an index into a small vector of registered entries the
  backend keeps; the merged collection rebuilds on register/unregister.
- DWrite 3 (`IDWriteFactory5`) is Win10+; the example manifest already
  declares Win10/11. If `QueryInterface` for the factory5 fails, return
  false (graceful - family falls back to default).

### CG (`backends/cg/cg_backend.mm`)

- In-memory: `CGDataProviderCreateWithData` -> `CGFontCreateWithDataProvider`
  -> `CTFontManagerRegisterGraphicsFont(cgFont, &err)`. Read the family
  via `CTFontCopyFamilyName(CTFontCreateWithGraphicsFont(...))`.
- Path: `CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, &err)`.
- **Process scope is deliberate**: it also exposes the family to
  `NSFont fontWithName:`, giving the macOS *native* host its prong for
  free.
- Token = retained `CGFontRef` (memory) or the URL (path), stored in a
  registry vector; `unregister_font` calls the matching
  `CTFontManagerUnregister*`.

### Cairo (`backends/cairo/cairo_backend.cpp`)

- In-memory: `FT_New_Memory_Face` over the caller-owned bytes ->
  `cairo_ft_font_face_create_for_ft_face`. Family name from
  `face->family_name`. The bytes must outlive the `FT_Face` (asset
  store owns them - see Part C).
- Also `FcConfigAppFontAddMemory` (if available) / `FcConfigAppFontAddFile`
  for the path form, so Fontconfig name lookups (the normal
  family-resolution path) resolve the app font too.
- Token = index into a registry holding the `FT_Face` + cairo font face
  + the FcConfig add; `unregister_font` drops the cairo face ref and the
  FT face.

### Null (`backends/null/null_backend.cpp`)

- `register_font` / `register_font_file` return false (`out_token = 0`,
  `out_family` empty); `unregister_font` no-op. `create_font*` therefore
  returns `asset_none`.

## Part C - Asset store extension (`hosts/shared/asset_store.h`)

Shared across all three hosts (`AssetStore<Loader>`), so this is wired
once:

- Add a `FONT` discriminant to `AssetEntry` carrying: the owned byte
  buffer copy (memory form), the backend `uint64_t token`, and the
  resolved family name string. (Path form still copies bytes into the
  buffer so the lifetime contract is uniform, or keeps the path and
  relies on the backend holding it - decide per backend; FreeType needs
  the bytes, so memory-copy is the safe default for all.)
- `allocate_font(data, len)` / `allocate_font_from_file(path)`: read
  bytes, call `backend->register_font*`, on success stash token+family
  in a fresh slot and return the handle; on false free the slot and
  return `asset_none`.
- `get_kind` returns `NEUI_ASSET_KIND_FONT`; `get_font_family` copies
  the stored name.
- `release_slot` for a FONT entry calls `backend->unregister_font(token)`
  and frees the byte buffer. **No per-ctx GPU upload cache applies to
  fonts** - a FONT entry never enters the `entry->bitmaps` path, and
  `get_pixels_for_export` rejects it.
- `release_context` must **not** drop FONT entries (they are
  factory-level, not ctx-bound); only `clear` / session teardown frees
  them. (Contrast with SURFACE, which holds a ctx.)

Per-host managers (`W32AssetManager`, `MacOSAssetManager`, xpl
`AssetManager`) inherit this unchanged - the only host-specific image
concern (xpl's path-keyed `_cache` tier) does not touch fonts.

## Part D - Wiring `neui_asset_api_t`

Three thin thunks in each host's `widgets.cpp` (win32 / macos / xpl)
forwarding to the shared `allocate_font*` / `get_font_family`, with the
usual `get_session_for_widget` / cross-session validation. Append the
function pointers to the `neui_asset_api_t` vtable in the same order in
all three hosts.

## Part E - Win32 native host extra prong

Only the native win32 host (`hosts/win32/`) needs this; the xpl host
paints its own text via the backend.

- On `create_font`, after the backend registration succeeds, also call
  `AddFontMemResourceEx(data, len, NULL, &count)` (memory) or
  `AddFontResourceExW(path, FR_PRIVATE, 0)` (path) so native HFONT
  widgets (`Edit`, `Button`, etc.) resolve the family in `CreateFontW`.
  Keep the returned `HANDLE` (memory) on the FONT entry to
  `RemoveFontMemResourceEx` at release.
- macOS native + Linux need nothing extra (CT process scope / xpl-only).

## Part F - Example

`examples/font_loading_example.cpp` -> `neui_font_loading_example`
target. Ship a small open-licensed `.ttf` next to it (CMake copies it
into the bundle/output dir, mirroring the HiDPI-image example's
`XCODE_ATTRIBUTE_COMBINE_HIDPI_IMAGES NO` care - though fonts are not
combined, so just a plain resource copy). The example:

- loads the bundled font via `assets->create_font_from_file`,
- queries `get_font_family`,
- sets `NEUI_ATTR_FONT_FAMILY` on a LABEL / BUTTON and uses the family
  in a CUSTOMDRAW `push_font`, proving both the native-control prong and
  the painted prong,
- demonstrates the bundled-resource path resolution a plugin would use.

## Part G - Docs

- New "Font loading" section in `CLAUDE.md` (after "Painter + asset
  API"): the FONT asset kind, push-not-pull rationale, family-name
  reference, the two-pronged seam, per-backend mechanism, `asset_none`
  on null.
- Add `create_font` / `create_font_from_file` / `get_font_family` to the
  `neui_asset_api_t` description and `NEUI_ASSET_KIND_FONT` to the kind
  list.
- Update the `assets.h` kind comment (drop "font reserved").
- Add the example to the build-outputs / targets list.

## Verification

- **Linux / macOS / Windows**: build warning-clean (`/W4`,
  `-Wall -Wextra`); run `neui_font_loading_example`, confirm the
  bundled face renders in both a native control and a CUSTOMDRAW
  push_font draw (visual check via the macOS verification toolkit on
  Mac; manual on Win/Linux).
- **Parity**: family-name resolution and weight selection match across
  d2d / cg / cairo; an unknown family still falls back to the host
  default (regression check - registration is additive).
- **null platform**: `create_font*` returns `asset_none`, the example
  degrades to default-font text rather than crashing.
- Tier-1 `tests/`: the asset store's font slot lifecycle (allocate ->
  family stored -> release calls unregister) is portable logic and can
  get a header-only unit test with a fake loader/backend stub, alongside
  the existing `hosts/shared/*` coverage.

## Out of scope (deferred)

- **Pull / request-on-miss**: an opt-in client callback fired when a
  family fails to resolve. Possible later addition; explicitly not in
  v1 (see rationale above).
- **Alias / collision-proof addressing**: register under a
  client-chosen private name. Deferred; v1 documents last-wins and
  relies on uniquely-named families.
- **Italic / oblique selection**: needs a style axis on the font stack
  (`push_font` is family+weight today).
- **Variable-font axes** (weight/width/optical-size as continuous
  parameters), font collections (`.ttc` face index selection beyond the
  default face), and a `get_font_count` enumeration of faces in one
  file.
- **`NEUI_ASSET_KIND_SVG` / `_VECTOR`** (slots 5/6) remain unrelated and
  reserved.

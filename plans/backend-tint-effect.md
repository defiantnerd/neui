# Plan: backend-level tint via D2D effects (replacing the CPU pre-tint cache)

## Context

Commit `b00d1f7` ("added tint, path and rect asset layers") shipped asset-layer `tint` as a CPU pre-multiply. Each `(asset, ctx, tint)` triple builds a tinted BGRA buffer once, uploads it as a separate GPU bitmap, and caches the handle alongside the untinted one in `tinted_bitmaps`. This works but is the wrong altitude:

- 4× GPU memory per distinct tint (every variant is a full bitmap).
- CPU cost on every cache miss (`premultiply_tint` loops over every pixel doing four `/ 255` divisions).
- No eviction — a client binding `tint` to a fast-changing attr (animated colour, hover state through a continuous param) thrashes the cache.
- Three near-identical `tinted_bitmaps` vectors + lifecycle in each host's `AssetEntry`.

D2D and CG both have native multiplicative-tint primitives that operate on a single source bitmap at draw time. The right design is: backend gets a `tint` parameter on `draw_bitmap`; each backend applies it natively; the per-asset CPU cache disappears.

This plan is the rip-out + reimplementation. It's a two-part change: the D2D backend has to migrate from `ID2D1RenderTarget` (D2D 1.0) to `ID2D1DeviceContext` (1.1+) to reach the effects framework — that migration is the bulk of the work and opens the door to shadow / blur / colour-matrix effects for future compound layer kinds.

Outcome: `tint` on asset layers (and any future bitmap-tinting consumer) is implemented in the backend, no CPU copy, no per-tint GPU memory, animated tints free, the cache + helpers + per-host duplication all delete.

## Decisions (resolved)

- **D2D**: migrate to `ID2D1DeviceContext`. Use `ID2D1Effect` with `CLSID_D2D1Tint` (multiplicative) — exactly the operation `premultiply_tint` performs today. Effect created lazily per ctx, cached on `D2DContext`.
- **CG**: use `kCGBlendModeMultiply` + `CGContextClipToMask`. Cheaper than `CIFilter`, and the multiply blend mode matches D2D's tint semantics for premultiplied BGRA. (CIFilter remains the fallback if mask-clip turns out to have edge-case issues with non-rectangular alpha.)
- **Null**: ignore `tint` (no-op, same as today's `draw_bitmap`).
- **Backend ABI**: extend `draw_bitmap` with a `uint32_t tint` parameter at the end; `0xFFFFFFFFu` is the passthrough sentinel and bypasses the effect setup entirely so untinted draws stay byte-for-byte identical.
- **Cache disappears**: no `tinted_bitmaps` field on any `AssetEntry`; no `premultiply_tint`, `draw_tinted_bitmap_from_entry`, `release_tinted_bitmaps_for_ctx`, `release_all_tinted_bitmaps`, `TintedCtxBitmap`. The thunk's tint param routes straight to the backend.
- **Compound paint surface unchanged**: `widget_paint_compound.h`'s asset case still extracts `tint`, but now just passes it through one path (no `if (tint == 0xFFFFFFFFu)` branch — the backend short-circuits). The `if (tint == 0u) break;` invisible-layer skip stays (no point uploading + drawing zero alpha).

## Part A — D2D 1.0 → 1.1 migration

This is the heavy lift. The current `D2DContext` is built on `ID2D1HwndRenderTarget`; effects need `ID2D1DeviceContext`. The migration also changes how the swap chain is presented and how device-loss is detected.

### Replace the create path

Today (`backends/d2d/d2d_backend.cpp::d2d_create_context_hwnd` around line 259):

```cpp
ID2D1HwndRenderTarget* target = nullptr;
HRESULT hr = g_factory->CreateHwndRenderTarget(props, hwnd_props, &target);
```

New shape:

```cpp
// 1. D3D11 device for the GPU surface.
ID3D11Device*        d3d_device = nullptr;
ID3D11DeviceContext* d3d_ctx    = nullptr;
D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                  D3D11_SDK_VERSION, &d3d_device, nullptr, &d3d_ctx);

// 2. DXGI device for the D2D layer.
IDXGIDevice* dxgi_device = nullptr;
d3d_device->QueryInterface(&dxgi_device);

// 3. D2D device + device context.
ID2D1Device*        d2d_device = nullptr;
ID2D1DeviceContext* d2d_ctx    = nullptr;
g_factory->CreateDevice(dxgi_device, &d2d_device);     // factory needs ID2D1Factory1
d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx);

// 4. DXGI swap chain bound to the HWND.
IDXGISwapChain1* swap_chain = nullptr;
DXGI_SWAP_CHAIN_DESC1 sc = {};
sc.Width  = 0;                                  // match window
sc.Height = 0;
sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
sc.BufferCount = 2;
sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
sc.SampleDesc.Count = 1;
dxgi_factory->CreateSwapChainForHwnd(d3d_device, hwnd, &sc, nullptr, nullptr, &swap_chain);

// 5. D2D bitmap bound to the swap chain's back buffer.
IDXGISurface* back_buffer = nullptr;
swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
ID2D1Bitmap1* d2d_target = nullptr;
D2D1_BITMAP_PROPERTIES1 bmp_props = {};
bmp_props.bitmapOptions  = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
bmp_props.pixelFormat    = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
d2d_ctx->CreateBitmapFromDxgiSurface(back_buffer, &bmp_props, &d2d_target);

d2d_ctx->SetTarget(d2d_target);
```

`g_factory` becomes `ID2D1Factory1*` (the 1.1+ factory; `D2D1CreateFactory` with `__uuidof(ID2D1Factory1)`).

### Replace `D2DContext` fields

```cpp
struct D2DContext {
  ID2D1DeviceContext* target      = nullptr;   // was ID2D1RenderTarget*
  IDXGISwapChain1*    swap_chain  = nullptr;   // new
  ID2D1Bitmap1*       back_buffer = nullptr;   // new
  // Drop hwnd_target alias; replace with `swap_chain` for the
  // HWND-vs-offscreen discriminator.

  // ... existing fields (path, sink, transform stack, alpha stack,
  //                       font stack, brush, generation) unchanged.

  ID2D1Effect* tint_effect = nullptr;          // lazily created in draw_bitmap
};
```

The off-screen ctx path (`CreateWicBitmapRenderTarget` around line 868) uses `CreateCompatibleRenderTarget` on the device context or `CreateBitmap` with `D2D1_BITMAP_OPTIONS_TARGET` — needs its own conversion. SURFACE assets keep working but their underlying primitive changes.

### Replace `Present`

Today `EndDraw` does the present implicitly through `HwndRenderTarget`. With DXGI:

```cpp
HRESULT hr = ctx->target->EndDraw();
if (SUCCEEDED(hr) && ctx->swap_chain) {
  DXGI_PRESENT_PARAMETERS params = {};
  hr = ctx->swap_chain->Present1(1, 0, &params);
}
// Device-loss now arrives via DXGI_ERROR_DEVICE_REMOVED on Present1, or
// D2DERR_RECREATE_TARGET on EndDraw (keep both paths). Bump generation
// and let the existing per-ctx asset cache rebuild as today.
```

### Replace `resize`

`HwndRenderTarget::Resize(w, h)` → `swap_chain->ResizeBuffers(...)` + re-acquire the back-buffer bitmap + `SetTarget`. The existing `D2DContext::resize` entrypoint stays; only the body changes.

### Verify what stays the same

- All path / transform / alpha / font / brush / draw_text / draw_rect / fill_rect / fill_path / stroke_path code is on `ID2D1RenderTarget` which `ID2D1DeviceContext` inherits from. Zero churn for those.
- The per-ctx asset cache + device-loss generation bump is unchanged.

### Testing the migration in isolation

Build + run `neui_example.exe` after Part A with no Part B changes. Visual output should be identical to today (no tint behaviour change yet). This is a pure backend refactor and is the right moment to confirm Tier-1 unit tests + smoke-test the example before adding the tint primitive on top.

## Part B — backend tint primitive

Once Part A lands:

### Renderer ABI (`include/neui/d/renderer.h`)

```c
void (NEUI_ABI *draw_bitmap)(neui_render_ctx_t ctx, void* bitmap,
                              float src_x, float src_y, float src_w, float src_h,
                              float dst_x, float dst_y, float dst_w, float dst_h,
                              uint32_t tint);   // new; 0xFFFFFFFF = no tint
```

This is internal (host-only) — the public `painter_api->draw_asset` already doesn't expose the raw bitmap pointer. Vtable-append for any other future shape props.

### D2D implementation

In `d2d_draw_bitmap`:

```cpp
if (tint == 0xFFFFFFFFu) {
  // Existing fast path - no effect setup, direct DrawBitmap.
  ctx->target->DrawBitmap(d2d_bmp, dst_rect, current_alpha(ctx),
                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src_rect);
  return;
}

if (!ctx->tint_effect)
  ctx->target->CreateEffect(CLSID_D2D1Tint, &ctx->tint_effect);

ctx->tint_effect->SetInput(0, d2d_bmp);
D2D1_VECTOR_4F v = {
  ((tint >> 16) & 0xff) / 255.0f,   // R
  ((tint >>  8) & 0xff) / 255.0f,   // G
  ((tint >>  0) & 0xff) / 255.0f,   // B
  ((tint >> 24) & 0xff) / 255.0f,   // A
};
ctx->tint_effect->SetValue(D2D1_TINT_PROP_COLOR, v);
ctx->target->DrawImage(ctx->tint_effect,
                       D2D1::Point2F(dst_x, dst_y),
                       D2D1::RectF(src_x, src_y, src_x+src_w, src_y+src_h));
```

`D2D1_TINT_PROP_CLAMP_OUTPUT` defaults to `FALSE` (multiplicative); leave it. The effect is reusable across draws within a frame — `SetInput` and `SetValue` are cheap, no recreate.

`tint_effect` is released alongside `target` in `release_path`/device-loss/destroy_context.

### CG implementation

In `cg_draw_bitmap`:

```objc
if (tint == 0xFFFFFFFFu) {
  // existing path
  return;
}

CGContextSaveGState(ctx->cg_ctx);
CGContextClipToMask(ctx->cg_ctx, dst_rect, cg_image);   // alpha shape
CGFloat rgba[4] = {
  ((tint >> 16) & 0xff) / 255.0,
  ((tint >>  8) & 0xff) / 255.0,
  ((tint >>  0) & 0xff) / 255.0,
  ((tint >> 24) & 0xff) / 255.0,
};
CGContextSetRGBFillColor(ctx->cg_ctx, rgba[0], rgba[1], rgba[2], rgba[3]);
CGContextSetBlendMode(ctx->cg_ctx, kCGBlendModeMultiply);
CGContextDrawImage(ctx->cg_ctx, dst_rect, cg_image);   // multiplies under the mask
CGContextRestoreGState(ctx->cg_ctx);
```

Open question to validate during implementation: whether the `kCGBlendModeMultiply` + `ClipToMask` combo exactly matches D2D's `D2D1_TINT_PROP_CLAMP_OUTPUT=FALSE` for partially-transparent source pixels. If not, fall back to a `CIFilter` (`CIColorMatrix`) path on CG and accept the higher per-draw cost.

### Null implementation

```c
// tint ignored; backend draws nothing anyway
```

## Rip-outs

After Parts A + B land, delete:

- `hosts/shared/painter.h`: `TintedCtxBitmap`, `premultiply_tint`, `draw_tinted_bitmap_from_entry`, `release_tinted_bitmaps_for_ctx`, `release_all_tinted_bitmaps`, `painter_draw_asset_tinted`.
- `hosts/shared/painter.h`: revert `draw_asset_thunk_t` to its previous 8-arg shape (`tint` moves to the backend's `draw_bitmap` instead). The thunk forwards `tint` from the compound paint code straight to `backend->draw_bitmap`.
- `hosts/crossplatform/asset_manager.{h,cpp}`: `tinted_bitmaps` field, `release_all_tinted_bitmaps` / `release_tinted_bitmaps_for_ctx` calls in `paint_surface` / `release_slot` / `release_context` / `clear`.
- `hosts/win32/asset_manager_w32.h`: same field + cleanup paths.
- `hosts/macos/asset_manager_macos.h`: same.
- All three host thunks (`xpl_painter_draw_asset_thunk`, `w32_painter_draw_asset_thunk`, `macos_painter_draw_asset_thunk`): drop the `if (tint != 0xFFFFFFFFu) draw_tinted_bitmap_from_entry(...)` branch — just pass `tint` through to `backend->draw_bitmap`.
- `hosts/shared/widget_paint_compound.h`: the asset case's tint-routing lambda + `if (tint == 0xFFFFFFFFu)` branch collapses to a single `painter_draw_asset_tinted` (or rename — see "API surface" below) call.

## API surface choice

Once tint goes through the thunk uniformly, the two helpers `painter_draw_asset` and `painter_draw_asset_tinted` collapse. Two readable shapes:

- **(a)** Keep `painter_draw_asset(p, asset, x, y, w, h)` for the public painter API (untinted, what clients see), and let compound paint reach the thunk directly with tint via a tiny internal helper. The thunk takes tint; the public API doesn't.
- **(b)** Extend the public painter API to take `tint` (defaulting `0xFFFFFFFF`). More expressive for clients writing their own `WIDGET_PAINT` code, costs one more arg everywhere.

Pick (a) for v1 — public surface stays minimal, tint is a compound-attribute thing.

## Tests

- Existing unit tests for `apply_set_int("tint", ...)` keep passing (mutator behaviour is unchanged).
- Delete `premultiply_tint` tests in `tests/test_compound.cpp` (function no longer exists).
- Add a smoke test that verifies the backend's tint param is forwarded: a null-backend test asserting the thunk passes the tint argument through to `draw_bitmap` (mock the backend's vtable).
- Visual verification: re-run `neui_example.exe`'s features demo widget at (790, 145) — the green-tinted dice should look identical to today's CPU-pre-tint output. Diff the screenshots if possible.

## Order

1. **Part A**: D2D backend migration. Ship, build, run, screenshot — confirm zero visual regression.
2. **Part B step 1**: extend `draw_bitmap` ABI; D2D + CG + null implementations of the tint param; thunks pass tint through.
3. **Part B step 2**: rip out `tinted_bitmaps` + helpers + the compound paint's two-path tint branch.
4. **Docs**: update `CLAUDE.md`'s "Compound drawables" paragraph (drop the `tinted_bitmaps`/`premultiply_tint` mention; describe the backend-level tint instead). Update the asset-layer `tint` prop comment in `include/neui/d/compound.h` (no longer talks about CPU pre-multiply or per-(asset, ctx, tint) cache).

## Why the migration is worth its weight

Tint is the small visible win. The bigger gain is that `ID2D1DeviceContext` is the entry point for every other useful 2D effect — `CLSID_D2D1Shadow`, `CLSID_D2D1GaussianBlur`, `CLSID_D2D1ColorMatrix`, `CLSID_D2D1Crop`, etc. Once compound layers want drop-shadow / blurred backplates / saturation / luminance-keyed icons, they all plug into the same effect-graph model the tint upgrade introduces. Doing this now means the effects framework is wired before the first feature that pulls on it, instead of being a parallel refactor on top of three or four CPU bandaids.

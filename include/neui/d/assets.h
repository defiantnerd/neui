#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"

// Asset API - session-scoped media handles (bitmaps today, SVG / vector /
// font reserved for later). Acquired via
//   neui_asset_api_t* a = (neui_asset_api_t*)
//     neui_api->get_interface(session, NEUI_API_ASSETS);
// and used outside the paint loop to load / preload media. During paint,
// clients render the loaded asset via neui_painter_api_t::draw_asset with
// the handle returned here.
//
// The handle layout matches neui_widget_t: upper 16 bits = owning session
// id, lower 16 bits = slot. Cross-session handles are rejected by the
// host. Handles are not generation-checked - reusing a slot after destroy
// can collide silently (matches the existing widget id contract; deferred).

// Forward-declarations for the surface paint callback below. The full
// types live in <neui/d/painter.h>; we don't pull that header here to
// keep the include graph tight (clients that use neui_asset_api_t
// without ever calling paint_surface don't need painter.h to compile).
struct neui_painter;
struct neui_painter_api;

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_ASSETS "com.defiantnerd.neui.extension.assets/0"

  typedef struct neui_asset {
    uint32_t id;
  } neui_asset_t;

  static const neui_asset_t asset_none = { UINT32_MAX };

  // Discriminator returned by neui_asset_api::get_kind. Lets future code
  // branch on kind without breaking ABI - the enum reserves slots for
  // formats that aren't implemented yet. NEUI_ASSET_KIND_NONE is also
  // returned for invalid handles.
  typedef enum neui_asset_kind {
    NEUI_ASSET_KIND_NONE     = 0,
    NEUI_ASSET_KIND_BITMAP   = 1,
    // Compound drawable: a mutable, declarative stack of typed layers
    // (text / asset) read at paint time. Built via NEUI_API_COMPOUND.
    NEUI_ASSET_KIND_COMPOUND = 2,
    // Behavior: a mutable list of input handlers (drag / wheel / key /
    // click / context-reset) that, when attached to a CUSTOMDRAW widget,
    // manipulate attrs in the widget's AttrBag in response to mouse /
    // keyboard / wheel events. Built via NEUI_API_BEHAVIOR.
    NEUI_ASSET_KIND_BEHAVIOR = 3,
    // Client-owned off-screen render target. Created via
    // neui_asset_api::create_surface and populated via paint_surface;
    // backed by a CPU pixel buffer that's uploaded to per-window GPU
    // caches on draw just like NEUI_ASSET_KIND_BITMAP. Lets clients
    // bake visual diagnostic outputs / thumbnails / cached ornaments
    // once and then draw the result via painter_api->draw_asset.
    NEUI_ASSET_KIND_SURFACE  = 4,
    // Reserved for future kinds. Do NOT renumber; bind new values to the
    // next unused integer so old client builds stay forward-compatible.
    // NEUI_ASSET_KIND_SVG    = 5,
    // NEUI_ASSET_KIND_VECTOR = 6,
    // NEUI_ASSET_KIND_FONT   = 7,
  } neui_asset_kind_t;

  // Client paint callback for neui_asset_api::paint_surface. Mirrors the
  // NEUI_EVENT_WIDGET_PAINT payload shape so a client can reuse the same
  // drawing code on a widget and on a surface. The painter / api pair is
  // valid only for the duration of the call.
  typedef void (NEUI_ABI *neui_surface_paint_fn)(
      struct neui_painter*     painter,
      struct neui_painter_api* api,
      float                    width_logical,
      float                    height_logical,
      void*                    user);

  typedef struct neui_asset_api {
    uint32_t neui_version;

    // Create a bitmap asset from raw BGRA8 (premultiplied) pixels.
    //   width_px / height_px - physical pixel dimensions.
    //   bgra_premul          - width_px * height_px * 4 bytes; copied by
    //                          the host (caller may free immediately).
    //   scale                - HiDPI factor of the source pixels
    //                          (1.0 = @1x, 2.0 = @2x, 3.0 = @3x). The
    //                          asset's logical size is pixel_size / scale,
    //                          so an @2x bitmap declared with scale=2 draws
    //                          at half its pixel dimensions.
    // Returns asset_none on failure (bad args, allocation, etc.).
    neui_asset_t (NEUI_ABI *create_bitmap)(neui_session_t session,
                                            uint32_t width_px,
                                            uint32_t height_px,
                                            const uint8_t* bgra_premul,
                                            float scale);

    // Create a bitmap asset from a file path or an embedded resource name.
    // PNG / JPG / BMP via the platform loader. On Win32 the name is also
    // tried as an embedded RT_USERDATA "PNG" resource (matching the host's
    // existing image-load behaviour).
    // Resolution: a name "foo.png" loaded with the current display scale
    // > 1.0 will prefer "foo@2x.png" / "foo@3x.png" if present.
    neui_asset_t (NEUI_ABI *create_from_file)(neui_session_t session,
                                                const char* path_utf8);

    // Release the asset. CPU pixels are freed immediately; per-context
    // GPU uploads are released on the next paint or at session destroy.
    // Drawing a destroyed handle is a no-op (not a crash).
    void (NEUI_ABI *destroy)(neui_session_t session, neui_asset_t asset);

    // Intrinsic asset size in logical (96-DPI) pixels.
    //   bitmap: width_px / scale, height_px / scale.
    //   future SVG: viewBox dimensions if present, else false.
    // out_logical_w / out_logical_h may be NULL to ignore.
    // Returns false on invalid handle or asset with no intrinsic size.
    bool (NEUI_ABI *get_size)(neui_session_t session, neui_asset_t asset,
                               float* out_logical_w, float* out_logical_h);

    // Returns the asset's kind discriminator. NEUI_ASSET_KIND_NONE if the
    // handle is invalid.
    neui_asset_kind_t (NEUI_ABI *get_kind)(neui_session_t session,
                                            neui_asset_t asset);

    // Create a compound drawable asset - an empty, mutable layer stack.
    // Populate via NEUI_API_COMPOUND. Returns asset_none on allocation
    // failure. Hosts that don't support compound (e.g. native macOS;
    // CUSTOMDRAW isn't implemented there) return asset_none.
    neui_asset_t (NEUI_ABI *create_compound)(neui_session_t session);

    // Create a behavior asset - an empty, mutable list of input handlers.
    // Populate via NEUI_API_BEHAVIOR. Returns asset_none on allocation
    // failure. Attaches to CUSTOMDRAW via widgets->set_asset (kind-routed).
    neui_asset_t (NEUI_ABI *create_behavior)(neui_session_t session);

    // Create an off-screen render surface (NEUI_ASSET_KIND_SURFACE).
    //   width_logical / height_logical - logical (96-DPI) dimensions.
    //   scale - HiDPI factor of the backing pixels (1.0 / 2.0 / 3.0).
    //     The physical pixel buffer is round(width_logical * scale) by
    //     round(height_logical * scale); a scale of 2.0 on a 100x100
    //     logical surface backs 200x200 pixels so a 1:1 draw on a 2x
    //     display stays crisp.
    // Returns asset_none on allocation failure, or on backends that
    // don't support off-screen targets (null backend). The surface is
    // valid (and draw_asset draws transparent black) until the first
    // paint_surface call populates it.
    neui_asset_t (NEUI_ABI *create_surface)(neui_session_t session,
                                              float width_logical,
                                              float height_logical,
                                              float scale);

    // Drive a client paint callback against an off-screen surface:
    //   1. begin_frame on the surface's off-screen ctx (clear to clear_argb).
    //   2. push a clip to [0,0,width_logical,height_logical].
    //   3. invoke fn(painter, api, width_logical, height_logical, user).
    //   4. pop_clip, end_frame.
    //   5. read pixels back into the asset's CPU buffer.
    //   6. drop any cached per-window GPU uploads of this asset so the
    //      next draw_asset call re-uploads the new pixels.
    // Safe to call any time EXCEPT inside a WIDGET_PAINT callback (the
    // backend's path / transform / alpha state is mid-frame). Safe to
    // call repeatedly to re-render. No-op on non-SURFACE handles, on
    // null fn, or on backends without off-screen support.
    void (NEUI_ABI *paint_surface)(neui_session_t        session,
                                    neui_asset_t          surface,
                                    uint32_t              clear_argb,
                                    neui_surface_paint_fn fn,
                                    void*                 user);
  } neui_asset_api_t;

#ifdef __cplusplus
}
#endif

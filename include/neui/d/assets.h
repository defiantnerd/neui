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
    NEUI_ASSET_KIND_NONE   = 0,
    NEUI_ASSET_KIND_BITMAP = 1,
    // Reserved for future kinds. Do NOT renumber; bind new values to the
    // next unused integer so old client builds stay forward-compatible.
    // NEUI_ASSET_KIND_SVG    = 2,
    // NEUI_ASSET_KIND_VECTOR = 3,
    // NEUI_ASSET_KIND_FONT   = 4,
  } neui_asset_kind_t;

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
  } neui_asset_api_t;

#ifdef __cplusplus
}
#endif

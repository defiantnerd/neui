#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"

// Optional client-side interface for supplying resource BYTES.
//
// The host calls client->get_interface(token, NEUI_API_RESOURCE_CLIENT) once
// per session at create time, exactly like NEUI_API_MENU_CLIENT. If a non-null
// neui_resource_client_t is returned, the host asks the client for a resource
// BEFORE trying to locate it itself.
//
// Purpose: let a client keep its assets in its own container - a plugin bundle,
// an executable resource section, an encrypted pack, flash on an MCU, or bytes
// generated at runtime - without neui knowing anything about that container.
// Before this interface, every path in neui that consumes media took a
// filesystem path, so an all-embedded deployment had no way in (and an embedded
// target may have no filesystem at all).
//
// ORDER: client first, then the host's own resolution. For images the host's own
// step is the @2x / @3x candidate ladder plus, on Win32, the embedded RT "PNG"
// resource lookup - so the effective order is
//   client -> embedded resource -> file.
// Bytes that fail to decode or parse are treated as a miss and the host
// continues to its own resolution rather than failing the load - so a buggy
// provider cannot shadow a file that is perfectly good. That holds for every
// load, not just the first: a route that once came from the client but later
// yields nothing falls back to the filesystem too.
//
// HOW OFTEN provide() IS CALLED - it differs by kind:
//   * IMAGE: the host caches resolution outcomes, misses included, so the client
//     is asked ONCE per (name, scale band) per session and never re-probed. In
//     particular `provide` is NEVER called per frame, even though the framework's
//     IMAGE widget resolves its source on every paint, and a client that declines
//     is not asked again. (Negative outcomes are sticky in v0; an explicit
//     create_from_file re-probes one, a repaint does not.)
//   * FONT / COMPONENT / SIDECAR: not cached - each create_font_from_file /
//     create_component_from_file / filmstrip discovery asks again, exactly as it
//     would re-read the file. So N component widgets built from one document cost
//     N calls, and sidecar discovery asks for two different candidate names
//     ("<path>.json", then "<base>.json"). Keep provide() cheap; do not treat it
//     as a once-per-session event for these kinds.
//
// Complementary to neui_component_env_t::resolve_asset (<neui/d/assets.h>),
// which is consulted before this one when loading a component document.
// resolve_asset answers "which existing asset HANDLE does this name mean" and
// can return a runtime-built compound / painted surface that has no byte form;
// this interface answers "here are the BYTES". Full order for an asset named
// inside a component document:
//   env.resolve_asset -> resource_client->provide -> filesystem.
// v0 LIMITATION: the two hooks do NOT see the same string. resolve_asset gets the
// raw entry from the document's "assets" map; provide() gets that entry joined
// onto the document's base_dir, because the component loader holds only the
// public asset API (which takes a path) and the join therefore happens before
// this interface is reached. A client serving component-referenced assets should
// match on the joined path's suffix. Tracked in
// plans/client-resource-provider.md (follow-up 1: a byte hook through
// ComponentApis closes it).
//
// THREADING / REENTRANCY: provide() and release() are called on the UI thread,
// synchronously, inside the neui call that triggered the load. That call is
// sometimes a PAINT (the framework's NEUI_W_IMAGE widget loads lazily on first
// draw). So provide() must not call back into any neui API, and must return
// promptly - on hosts that hold a renderer lock across a frame, blocking there
// stalls the refresh and re-entering neui can deadlock. Do I/O only.

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_RESOURCE_CLIENT \
  "com.defiantnerd.neui.extension.resource.client/0"

  // What the host is trying to load. Reserve new values at the next unused
  // integer so old client builds stay forward-compatible (same rule as
  // neui_asset_kind_t).
  typedef enum neui_resource_kind {
    NEUI_RESOURCE_KIND_NONE      = 0,
    // Encoded image bytes (PNG / JPG / BMP / whatever the platform decodes).
    // Requested by create_from_file, create_filmstrip_from_file, a
    // NEUI_W_IMAGE widget's set_text, and component asset references.
    NEUI_RESOURCE_KIND_IMAGE     = 1,
    // Font file bytes (TTF / OTF / TTC). Requested by create_font_from_file.
    NEUI_RESOURCE_KIND_FONT      = 2,
    // Component document (UTF-8 JSON). Requested by
    // create_component_from_file.
    NEUI_RESOURCE_KIND_COMPONENT = 3,
    // Filmstrip layout sidecar (UTF-8 JSON) - the "<path>.json" / "<base>.json"
    // frame-count document create_filmstrip_from_file discovers.
    NEUI_RESOURCE_KIND_SIDECAR   = 4,
    // Reserved: do NOT renumber.
    // NEUI_RESOURCE_KIND_SVG   = 5,
    // NEUI_RESOURCE_KIND_AUDIO = 6,
  } neui_resource_kind_t;

  // Bit for neui_resource_client_t::kinds_mask.
#define NEUI_RESOURCE_MASK(kind)   (1u << (uint32_t)(kind))
#define NEUI_RESOURCE_MASK_IMAGE     NEUI_RESOURCE_MASK(NEUI_RESOURCE_KIND_IMAGE)
#define NEUI_RESOURCE_MASK_FONT      NEUI_RESOURCE_MASK(NEUI_RESOURCE_KIND_FONT)
#define NEUI_RESOURCE_MASK_COMPONENT NEUI_RESOURCE_MASK(NEUI_RESOURCE_KIND_COMPONENT)
#define NEUI_RESOURCE_MASK_SIDECAR   NEUI_RESOURCE_MASK(NEUI_RESOURCE_KIND_SIDECAR)

  // Host-allocated, passed by const pointer. Fields may be APPENDED in a future
  // version, so a client must never copy this struct by value or assume its
  // size; read the fields it knows.
  typedef struct neui_resource_request {
    neui_resource_kind_t kind;

    // The name exactly as the client originally passed it (to create_from_file,
    // set_text on an IMAGE widget, create_font_from_file, ...). NOT a
    // host-rewritten variant: no "@2x" suffix is ever appended - the @Nx
    // convention stays a filesystem convention. The client looks its own
    // container up by the name it already knows.
    // ONE EXCEPTION (v0, see the file header): an asset referenced from inside a
    // component document arrives as that document's base_dir joined onto the raw
    // "assets"-map entry, not as the bare entry.
    const char*          name;

    // IMAGE only (0.0 for every other kind): the display scale the host is
    // resolving for - 1.0 / 2.0 / 3.0, or a fractional Windows scale. This is a
    // HINT. The client may return any resolution it has and reports what it
    // actually returned in neui_resource_bytes_t::scale; the host asks ONCE per
    // name and scale band, never once per @Nx variant.
    float                scale_hint;

    // RESERVED, always NULL in v0. Intended to carry the base_dir of the
    // component document a resource is referenced from, so a client could
    // disambiguate same-named assets of two different components. It stays NULL
    // until the ComponentApis byte hook lands, because until then `name` above
    // arrives base_dir-joined instead. Do not branch on it.
    const char*          base_dir;
  } neui_resource_request_t;

  typedef struct neui_resource_bytes {
    // Borrowed by the host for the duration of the provide() -> release() pair
    // only, which completes before the triggering neui call returns. Point it
    // at a resource section, a container mapping, a member buffer, or a fresh
    // allocation - whatever suits; nothing needs to outlive the call.
    const uint8_t* data;
    uint32_t       len;

    // IMAGE only: HiDPI factor of the returned pixels - the same meaning as
    // neui_asset_api::create_bitmap's `scale`, so an @2x sheet returned here
    // must say 2.0 or it will draw at twice its intended size. 0.0 = treat as
    // 1.0. Ignored for other kinds.
    float          scale;

    // Opaque client cookie, echoed back to release(). Lets one provider hand
    // out static blobs, heap buffers and mappings and tell them apart.
    void*          release_token;
  } neui_resource_bytes_t;

  typedef struct neui_resource_client {
    uint32_t neui_version;

    // Which kinds this client answers, as a bitwise OR of NEUI_RESOURCE_MASK_*.
    // 0 means "all kinds". Kinds outside the mask are never passed to provide()
    // - cheaper than every client opening with `if (kind != IMAGE) return
    // false;`, and it keeps a client that only overrides images off the font
    // and component load paths entirely.
    uint32_t kinds_mask;

    // Fill *out and return true, or return false to let the host resolve the
    // resource its own way (the pre-existing behaviour). Call frequency differs
    // by kind - at most once per (name, scale band) per session for IMAGE, once
    // per load call for the other kinds; see "HOW OFTEN" in the file header.
    // `out` is zeroed by the host before the call.
    bool (NEUI_ABI *provide)(void* token,
                             const neui_resource_request_t* req,
                             neui_resource_bytes_t*         out);

    // Called exactly once for every provide() that returned true, after the
    // host has copied / decoded the bytes and before the triggering neui call
    // returns. May be NULL when the client hands out only static blobs.
    void (NEUI_ABI *release)(void* token, const neui_resource_bytes_t* res);
  } neui_resource_client_t;

#ifdef __cplusplus
}
#endif

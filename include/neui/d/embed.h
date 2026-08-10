#pragma once

#include <stdint.h>
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

// DAW / host-application embedding for NEUI_W_PLUGWINDOW frames - the neui
// side of an audio-plugin adapter (VST3 / CLAP / LV2 glue lives outside
// neui and calls this). A plugin editor never owns the process, the message
// loop, or the top-level window: the host hands it a native parent and the
// frame must live inside it.
//
// Usage shape (identical on every platform):
//
//   neui_embed_api_t* embed = neui->get_interface(sess, NEUI_API_EMBED);
//   neui_widget_t ui = widgets->create(sess, widget_none, NEUI_W_PLUGWINDOW,
//                                       0, 0, 600, 400, NULL);
//   embed->set_parent(sess, ui, host_parent);   // BEFORE show
//   widgets->show(sess, ui);
//   ...
//   int fd = embed->event_fd(sess, ui);          // Linux: register with the
//                                                // host run loop; else -1
//   embed->pump_and_tick(sess, ui);              // from the host's timer
//   ...
//   widgets->destroy(sess, ui);                  // editor closed
//
// `host_parent` is the host-provided native parent, passed as void*:
//   Win32:  the parent HWND               (frame becomes a WS_CHILD of it)
//   macOS:  the parent NSView*            (frame roots as a subview; no
//                                          NSWindow of its own)
//   Linux:  the X11 Window id, cast to void* through uintptr_t (frame is
//           created as a child of it over a dedicated Display connection)
//
// Event-loop contract: neui owns no loop in embedded mode - never call
// run() / pump_once() from a plugin. On Win32 and macOS the host's own
// pump / main runloop already delivers paint, input, and timer messages to
// a child HWND / NSView subview, so event_fd returns -1 and pump_and_tick
// is a no-op kept for a platform-uniform adapter loop. On Linux the frame
// runs over its own X11 connection: register event_fd with the host's run
// loop (VST3 IRunLoop registerEventHandler / CLAP posix-fd) and call
// pump_and_tick from the host's periodic timer AND whenever the fd signals
// readable - it drains the connection, advances at most one animation tick
// per ~16 ms, and repaints.
//
// A host without an embedding story does not expose this interface;
// clients feature-detect via get_interface returning NULL. Exposed by the
// crossplatform host ("neui.host.crossplatform") - the host plugin builds
// should select explicitly for a pixel-identical UI everywhere.
#define NEUI_API_EMBED "com.defiantnerd.neui.extension.embed/0"

  typedef struct neui_embed_api
  {
    uint32_t neui_version;

    // Set the native parent for a NEUI_W_PLUGWINDOW. Must be called BEFORE
    // widgets->show() realizes the frame (the parent is consumed at native
    // creation); returns false for an invalid widget, a non-PLUGWINDOW
    // frame, or one that is already realized. NULL re-selects a standalone
    // borderless top-level (the default).
    bool (NEUI_ABI* set_parent)(neui_session_t session, neui_widget_t widget,
                                void* native_parent);

    // Pollable event file descriptor for the embedded frame, or -1 when the
    // platform needs none (Win32 / macOS) or the frame is not realized.
    int (NEUI_ABI* event_fd)(neui_session_t session, neui_widget_t widget);

    // Service the embedded frame: drain pending native events (Linux),
    // advance animations, repaint if needed. Call from the host's periodic
    // timer and on event_fd readability. Safe no-op on platforms where the
    // host pump already services the frame. Returns false for an invalid /
    // unrealized widget.
    bool (NEUI_ABI* pump_and_tick)(neui_session_t session, neui_widget_t widget);

    // Append new methods at the end (vtable-append evolution rule).
  } neui_embed_api_t;

#ifdef __cplusplus
}
#endif

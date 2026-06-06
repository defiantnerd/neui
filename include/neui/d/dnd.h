#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "events.h"      // neui_event_dnd_t, neui_data_item_t fwd
#include "clipboard.h"   // shared MIME constants (NEUI_MIME_*)
#include "assets.h"      // neui_asset_t for drag preview images

#ifdef __cplusplus
extern "C" {
#endif

// Drag&drop API. v1 ships drop-target support only: widgets can receive
// drags from external apps (Explorer, Finder, browsers, ...). Initiating
// drags from inside the app is reserved for a future revision.
//
// A drop target is any widget with `set_drop_target(w, true)`. While a
// drag is in flight the framework hit-tests cursor positions through the
// widget tree (deepest first), finds the first drop_target widget whose
// accepted-MIMEs intersects the drag's advertised formats, and fires
// NEUI_EVENT_DND_ENTER / MOVE / LEAVE on that widget. The client decides
// per dispatch whether it will take the drag - and in which mode (COPY /
// MOVE / LINK) - by calling dnd_api->accept(session, action). Default is
// NEUI_DND_ACTION_NONE, which surfaces as the OS "no-drop" cursor.
//
// On NEUI_EVENT_DND_DROP the event carries a transient neui_data_item_t
// id. Use clipboard_api->item_get_format(item, mime, ...) to read bytes;
// the item is released the moment the callback returns, so copy out
// anything you want to keep.
//
// Coordinate space: x/y on the event payload are widget-local logical
// pixels, just like NEUI_EVENT_MOUSE_*.

// NEUI_API_DND is defined in d/api.h.

// Drag operations the source proposes and the target accepts. Bitmask so
// "copy or move, whichever the OS picks" is expressible. Numeric values
// align with Win32 DROPEFFECT_* (1=COPY, 2=MOVE, 4=LINK) so the platform
// layer forwards them with a small mask.
typedef enum neui_dnd_action {
  NEUI_DND_ACTION_NONE = 0,
  NEUI_DND_ACTION_COPY = 1,
  NEUI_DND_ACTION_MOVE = 2,
  NEUI_DND_ACTION_LINK = 4,
} neui_dnd_action_t;

// Optional drag-preview image attached to a drag. The image follows the
// cursor while the drag is in flight, indicating the content being moved.
//
// `image` may be any asset that resolves to displayable BGRA8 pixels -
// NEUI_ASSET_KIND_BITMAP today, NEUI_ASSET_KIND_SURFACE once that lands
// (mutable, paintable bitmap-compatible asset). The drag-source path
// snapshots whatever bytes it needs synchronously inside the begin_drag
// call, so the asset may be released the moment begin_drag returns.
//
// `hot_x` / `hot_y` are the point on the image (in logical px relative to
// the image's top-left) that sticks to the cursor. Pass -1 on either axis
// to default to the image centre on that axis (matches what most apps
// want and lets the client skip arithmetic for the trivial case).
//
// `asset_none` for `image` means "use the OS default" - identical to
// calling the older begin_drag without a preview.
typedef struct neui_drag_preview {
  neui_asset_t image;
  int          hot_x;
  int          hot_y;
} neui_drag_preview_t;

typedef struct neui_dnd_api {
  uint32_t neui_version;

  // Mark a widget as a drop target. Any widget type is accepted (frames
  // act as catch-all when no descendant takes the drag). Default: false.
  void (NEUI_ABI *set_drop_target)(neui_session_t session,
                                    neui_widget_t widget,
                                    bool enable);

  // Read the current drop-target flag.
  bool (NEUI_ABI *get_drop_target)(neui_session_t session,
                                    neui_widget_t widget);

  // Restrict the MIME types this widget will accept. Pass count=0 (or
  // mimes=NULL) to clear - meaning "accept any". Stored per-widget; the
  // host copies the strings, so the caller's buffers can go away after
  // the call returns.
  void (NEUI_ABI *set_accepted_formats)(neui_session_t session,
                                         neui_widget_t widget,
                                         const char* const* mimes,
                                         int count);

  // Signal which action the client will accept for the current drag.
  // Call from inside the onevent callback during DND_ENTER / DND_MOVE
  // (and DND_DROP, where it determines the final reported effect).
  // The framework caches the value until the next dispatch and reports
  // it back to the OS pasteboard so the cursor reflects accept/reject.
  // Calling outside a DnD dispatch is a no-op.
  void (NEUI_ABI *accept)(neui_session_t session,
                           neui_dnd_action_t action);

  // Initiate an OS-level drag with `payload` as the data being dragged.
  // Synchronous: blocks while the user moves the cursor; returns the
  // negotiated action (NEUI_DND_ACTION_NONE if the user cancelled,
  // dropped on a non-target, or no drop target accepted any of the
  // payload's MIMEs).
  //
  // `source_widget` selects the owning frame whose native handle anchors
  // the drag (macOS needs an NSView). The widget must be inside a
  // visible frame; otherwise returns NONE.
  //
  // `payload` is allocated and released by the caller via the clipboard
  // item API (clipboard_api->create_item / item_set_format / release).
  // The framework snapshots formats before spinning the OS drag loop,
  // so the caller may release the item the instant begin_drag returns.
  //
  // `allowed_actions` is an OR-bitmask of neui_dnd_action_t. The OS
  // constrains the cursor to this set; targets that ask for an action
  // outside the mask see DROPEFFECT_NONE.
  //
  // Re-entrancy: events keep firing while a drag is in flight (drop
  // targets in this same session receive ENTER / MOVE / LEAVE / DROP
  // normally). Do not call begin_drag from inside a DnD dispatch -
  // returns NONE in that case.
  neui_dnd_action_t (NEUI_ABI *begin_drag)(neui_session_t session,
                                            neui_widget_t source_widget,
                                            neui_data_item_t payload,
                                            uint32_t allowed_actions);

  // Variant of begin_drag that also attaches a drag-preview image. Same
  // semantics + return value as begin_drag; `preview` may be NULL (in
  // which case behaviour is identical to begin_drag). When non-null and
  // `preview->image` is a valid asset, the OS drag loop shows that image
  // under the cursor with `(hot_x, hot_y)` (or the image centre on axes
  // set to -1) pinned to the pointer.
  //
  // Asset kinds supported v1: NEUI_ASSET_KIND_BITMAP. NEUI_ASSET_KIND_SURFACE
  // will be supported the day that asset kind ships - no API change.
  neui_dnd_action_t (NEUI_ABI *begin_drag_with_preview)(
                                            neui_session_t session,
                                            neui_widget_t source_widget,
                                            neui_data_item_t payload,
                                            uint32_t allowed_actions,
                                            const neui_drag_preview_t* preview);
} neui_dnd_api_t;

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

// System clipboard API.
//
// The clipboard is modelled as an item object that holds zero or more
// named-format representations of the same content (e.g. text/plain and
// text/html views of the same string). v1 only round-trips text/plain
// through the OS, but the API shape locks in the object so future formats
// can be added without breaking existing code.
//
// Most clients only need the convenience shortcuts (set_text / get_text /
// has_text). The item-based API is for clients that need richer content
// (build a multi-format item, then write it once).
//
// External clipboard-change notifications are opt-in via a separate
// client-side interface (NEUI_API_CLIPBOARD_CLIENT).

// NEUI_API_CLIPBOARD is defined in d/api.h.

#define NEUI_API_CLIPBOARD_CLIENT \
  "com.defiantnerd.neui.extension.clipboard.client/0"

// Standard mime types. Stable across versions.
#define NEUI_CLIPBOARD_MIME_TEXT  "text/plain;charset=utf-8"
// Reserved for future versions: "text/html", "image/png", etc.

// Opaque item handle. Lives until release(). Per-session.
typedef struct neui_clipboard_item { uint32_t id; } neui_clipboard_item_t;
static const neui_clipboard_item_t neui_clipboard_item_none = { UINT32_MAX };

typedef struct neui_clipboard_api {
  uint32_t neui_version;

  /* ---- Convenience shortcuts (the 95% case) ---- */

  // Replace the system clipboard with a single text representation.
  // Returns 1 on success, 0 on failure.
  int  (NEUI_ABI *set_text)(neui_session_t session, const char* utf8);

  // Read the system clipboard's text representation into buf as UTF-8.
  // Pass buf=NULL to query the byte count needed (including null terminator).
  // Returns total bytes needed (incl. null) when buf is NULL.
  // Copies up to buflen bytes including null terminator if buf is non-NULL.
  // Returns 0 if the clipboard has no text.
  // Returns -1 on error.
  int  (NEUI_ABI *get_text)(neui_session_t session, char* buf, int buflen);

  // Quick check - for enable/disable of "Paste" UI affordance.
  bool (NEUI_ABI *has_text)(neui_session_t session);

  /* ---- Item-based API (multi-format) ---- */

  // Snapshot the current system clipboard into an item. Returns
  // neui_clipboard_item_none if the clipboard is empty or contains no
  // representation the host knows. Caller must release() the item.
  neui_clipboard_item_t (NEUI_ABI *read)(neui_session_t session);

  // Allocate an empty item to populate before write(). Caller must
  // release() the item.
  neui_clipboard_item_t (NEUI_ABI *create_item)(neui_session_t session);

  // Free the item handle and its associated storage. Items left unreleased
  // when the session is destroyed are cleaned up automatically.
  void (NEUI_ABI *release)(neui_session_t session, neui_clipboard_item_t item);

  // Place the item's representations on the system clipboard. Returns 1 on
  // success, 0 on failure.
  // v1: only NEUI_CLIPBOARD_MIME_TEXT is propagated to the OS; unknown
  // mimes set on the item are stored but silently dropped on write.
  int (NEUI_ABI *write)(neui_session_t session, neui_clipboard_item_t item);

  // Add or replace a format on the item.
  // v1: only NEUI_CLIPBOARD_MIME_TEXT is accepted; other mime strings
  // return 0 so clients exercising future formats see a clear failure.
  int  (NEUI_ABI *item_set_format)(neui_session_t session,
                                    neui_clipboard_item_t item,
                                    const char* mime,
                                    const void* data, uint32_t length);

  // Read bytes for a format.
  // Pass buf=NULL to query the byte count needed.
  // For text mimes this includes a trailing null terminator (which is also
  // copied into buf when copying).
  // Returns 0 if the format is absent on the item, -1 on error.
  int  (NEUI_ABI *item_get_format)(neui_session_t session,
                                    neui_clipboard_item_t item,
                                    const char* mime,
                                    void* buf, int buflen);

  // True if the item carries a representation for the given mime.
  bool (NEUI_ABI *item_has_format)(neui_session_t session,
                                    neui_clipboard_item_t item,
                                    const char* mime);
} neui_clipboard_api_t;

// Optional client-side interface. The host calls
// client->get_interface(token, NEUI_API_CLIPBOARD_CLIENT) once per session
// at create time; if a non-null neui_clipboard_client_t is returned, the
// host wires up a system clipboard listener and invokes onchange(token)
// whenever the clipboard contents change (from any application, including
// the current one).
typedef struct neui_clipboard_client {
  uint32_t neui_version;
  void (NEUI_ABI *onchange)(void* token);
} neui_clipboard_client_t;

#ifdef __cplusplus
}
#endif

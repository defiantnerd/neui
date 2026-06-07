#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

// System clipboard API.
//
// The clipboard is modelled as a data-item object that holds zero or more
// named-format representations of the same content (e.g. text/plain and
// text/html views of the same string). The same neui_data_item_t primitive
// is also used by drag&drop (see d/dnd.h) - one storage shape for any
// MIME-typed payload crossing the application boundary.
//
// Formats round-tripped to/from the OS clipboard today:
//   - text/plain;charset=utf-8       (NEUI_MIME_TEXT)
//   - text/html                      (NEUI_MIME_HTML)
//   - text/uri-list                  (NEUI_MIME_URI_LIST) - file paths
//   - any other MIME string is stored under that name as a custom
//     pasteboard / registered-clipboard format (host-defined encoding).
//
// Most clients only need the convenience shortcuts (set_text / get_text /
// has_text). The item-based API is for clients that need richer content
// (build a multi-format item, then write it once; or inspect every
// representation a drop payload carries).
//
// Clipboard-change polling is the supported pattern: callers (e.g. a
// menu's WM_INITMENUPOPUP / NSMenuValidation handler) call has_text /
// item_has_format on demand to gate Paste-like UI. There is no push-style
// onchange callback in the API.

// NEUI_API_CLIPBOARD is defined in d/api.h.

// Standard mime types. Stable across versions.
#define NEUI_MIME_TEXT      "text/plain;charset=utf-8"
#define NEUI_MIME_HTML      "text/html"
#define NEUI_MIME_URI_LIST  "text/uri-list"
// Image MIMEs. image/png round-trips between neui apps natively (registered
// MIME bytes); on Win32 it's also published as CF_DIBV5 so native shells
// (Explorer, Paint, Outlook) see the same image as a 32bpp BGRA bitmap, and
// CF_DIBV5 read from external apps surfaces back as image/png. On macOS
// image/png maps to NSPasteboardTypePNG with no conversion (PNG bytes are
// the native format).
#define NEUI_MIME_PNG       "image/png"

// Backwards-compatible alias for the original text macro.
#define NEUI_CLIPBOARD_MIME_TEXT  NEUI_MIME_TEXT

// Opaque data-item handle. Lives until release(). Per-session. Backs both
// clipboard items and (transient) drag&drop drop payloads.
#ifndef NEUI_DATA_ITEM_T_DEFINED
#define NEUI_DATA_ITEM_T_DEFINED
typedef struct neui_data_item { uint32_t id; } neui_data_item_t;
#endif
static const neui_data_item_t neui_data_item_none = { UINT32_MAX };

// Lazy-data provider callback. Registered on a data item via
// item_set_format_callback; the framework invokes it when something
// (drag-drop receiver, clipboard reader, or the client itself via
// item_get_format) actually asks for the bytes. The callback returns a
// pointer to its bytes and writes the byte count to *out_size; the
// framework copies the bytes immediately into its own buffer, so the
// pointer only has to remain valid for the duration of the call. Returning
// NULL or 0 = "no bytes available" (same effect as the format being absent).
//
// Lifetime: the provider must outlive every framework call that might
// invoke it. The framework caches the first non-empty result on the data
// item, so the provider is typically called at most once per format per
// item; on macOS DnD it's called by AppKit during the dragging session, on
// Win32 DnD by IDataObject::GetData inside DoDragDrop, on clipboard write
// during the write() call itself.
typedef const uint8_t* (NEUI_ABI *neui_data_provider_t)(
    void* userdata,
    const char* mime,
    uint32_t* out_size);

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

  // Snapshot the current system clipboard into a data item. Returns
  // neui_data_item_none if the clipboard is empty or contains no
  // representation the host knows. Caller must release() the item.
  neui_data_item_t (NEUI_ABI *read)(neui_session_t session);

  // Allocate an empty item to populate before write(). Caller must
  // release() the item.
  neui_data_item_t (NEUI_ABI *create_item)(neui_session_t session);

  // Free the item handle and its associated storage. Items left unreleased
  // when the session is destroyed are cleaned up automatically.
  void (NEUI_ABI *release)(neui_session_t session, neui_data_item_t item);

  // Place every format on the item onto the system clipboard. Returns 1 on
  // success, 0 on failure. Unknown MIMEs (anything beyond the documented
  // built-ins) are placed under their MIME string as a custom format.
  int (NEUI_ABI *write)(neui_session_t session, neui_data_item_t item);

  // Add or replace a format on the item. Any MIME string is accepted;
  // built-in MIMEs round-trip through the OS, custom MIMEs pass through
  // as opaque bytes (host-defined encoding on the OS clipboard).
  int  (NEUI_ABI *item_set_format)(neui_session_t session,
                                    neui_data_item_t item,
                                    const char* mime,
                                    const void* data, uint32_t length);

  // Read bytes for a format.
  // Pass buf=NULL to query the byte count needed.
  // For text mimes this includes a trailing null terminator (which is also
  // copied into buf when copying).
  // Returns 0 if the format is absent on the item, -1 on error.
  int  (NEUI_ABI *item_get_format)(neui_session_t session,
                                    neui_data_item_t item,
                                    const char* mime,
                                    void* buf, int buflen);

  // True if the item carries a representation for the given mime.
  bool (NEUI_ABI *item_has_format)(neui_session_t session,
                                    neui_data_item_t item,
                                    const char* mime);

  // Register a lazy-data callback for the given format. The bytes are
  // produced by the provider on first read (see neui_data_provider_t).
  // Calling item_set_format on the same mime later switches the entry
  // back to eager bytes. Returns 1 on success, 0 on bad item / null mime
  // / null provider.
  //
  // Use this when the bytes are expensive to compute (PNG encode, file
  // bundling) and the receiver may not actually ask for that format. On
  // DnD source the framework registers the format with the OS as a
  // deferred-render entry so other apps see it in the format list without
  // forcing materialisation; the provider only fires if the receiver
  // requests those bytes.
  int (NEUI_ABI *item_set_format_callback)(neui_session_t session,
                                            neui_data_item_t item,
                                            const char* mime,
                                            neui_data_provider_t provider,
                                            void* userdata);
} neui_clipboard_api_t;

#ifdef __cplusplus
}
#endif

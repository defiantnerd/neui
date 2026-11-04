#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

// Web page interop: the parts of running inside a web page that have no
// desktop counterpart. Where a frame mounts in the page, messages between the
// app and the page's script, and whether the page is visible.
//
// Exposed ONLY by neui.host.crossplatform built for the web (Emscripten,
// plans/wasm-host.md). Every other host returns NULL from get_interface, which
// is how a client feature-detects it:
//
//   neui_web_api_t* web = (neui_web_api_t*)api->get_interface(sess, NEUI_API_WEB);
//   if (web) web->post_message(sess, "{\"ready\":true}", 14);
//
// The page side is examples/web/neui-web.js: NeuiApp.mount(element, options)
// starts an app in an element of the page, app.postMessage(data) reaches the
// app as NEUI_EVENT_WEB_MESSAGE, app.on('message', fn) receives what the app
// sends with post_message, app.unmount() ends it.
//
// Page -> app arrives as events (d/events.h, category 0x000C, payload
// neui_event_web_t), delivered to every live session of the app:
//   NEUI_EVENT_WEB_MESSAGE - the page posted a message (data / length);
//   NEUI_EVENT_WEB_PAGE    - the page was hidden, shown again, or is being
//                            left (page_state = NEUI_WEB_PAGE_*).
//
// Threading: there is one thread on the web; call from neui callbacks or main.
#define NEUI_API_WEB "com.defiantnerd.neui.extension.web/0"

// Page visibility, in neui_event_web_t::page_state and page_state().
// Explicitly numbered (ABI); bind a new value to the next unused integer.
typedef enum neui_web_page_state {
  NEUI_WEB_PAGE_VISIBLE = 1,  // the page is shown
  NEUI_WEB_PAGE_HIDDEN  = 2,  // another tab, minimised (document hidden)
  NEUI_WEB_PAGE_LEAVING = 3,  // pagehide: the page is being unloaded or cached
} neui_web_page_state_t;

// The stack-switching build, from flavor(). Both keep the desktop's blocking
// calls (a modal show blocks); they differ in size and speed only.
typedef enum neui_web_flavor {
  NEUI_WEB_FLAVOR_JSPI     = 1,  // JS promise integration (WebAssembly.Suspending)
  NEUI_WEB_FLAVOR_ASYNCIFY = 2,  // Asyncify (any browser with wasm)
} neui_web_flavor_t;

typedef struct neui_web_api {
  uint32_t neui_version;

  // Mount a top-level frame (APPWINDOW, PLUGWINDOW) into the page element that
  // matches the CSS `selector`, looked up first inside the app's own mount
  // element, then in the whole document. Call before the frame's first show;
  // the string is copied. NULL or "" = the default (the app's mount element).
  // A container with a size of its own is filled by the frame and followed
  // when it resizes; otherwise the frame keeps its create() size.
  // NEUI_API_EMBED's set_parent does the same for a PLUGWINDOW, with the
  // selector as its native_parent (a const char*).
  // Returns false for a frame that is already shown, a DIALOG (dialogs float
  // over their owner) or a widget that is not a frame.
  bool (NEUI_ABI *set_mount)(neui_session_t session, neui_widget_t frame,
                             const char* selector);

  // Send a message to the page (app.on('message', fn) there): `length` bytes
  // of UTF-8, typically JSON. Delivered after the call returns (never inside
  // it), in order. Returns false when there is no page side to receive it
  // (the app was not started through neui-web.js).
  bool (NEUI_ABI *post_message)(neui_session_t session, const char* data,
                                uint32_t length);

  // The page's visibility now (NEUI_WEB_PAGE_VISIBLE / _HIDDEN).
  uint32_t (NEUI_ABI *page_state)(neui_session_t session);

  // The build this app runs on (NEUI_WEB_FLAVOR_*).
  uint32_t (NEUI_ABI *flavor)(neui_session_t session);
} neui_web_api_t;

#ifdef __cplusplus
}
#endif

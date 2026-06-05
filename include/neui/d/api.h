#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Calling convention for all neui function pointers.
// Ensures a stable ABI across compilers and build configurations.
#if defined(_WIN32)
#  define NEUI_ABI __cdecl
#else
#  define NEUI_ABI
#endif

#define NEUI_API_RENDERER  "com.defiantnerd.neui.extension.renderer/0"
#define NEUI_API_CLIPBOARD "com.defiantnerd.neui.extension.clipboard/0"
#define NEUI_API_DND       "com.defiantnerd.neui.extension.dnd/0"

#if 0
#define NEUI_API_INPUT    "com.defiantnerd.neui.extension.input/0"
#define NEUI_API_LAYOUT   "com.defiantnerd.neui.extension.layout/0"
#define NEUI_API_THEME    "com.defiantnerd.neui.extension.theme/0"
#define NEUI_API_STORAGE  "com.defiantnerd.neui.extension.storage/0"
#define NEUI_API_NETWORK  "com.defiantnerd.neui.extension.network/0"
#define NEUI_API_AUDIO    "com.defiantnerd.neui.extension.audio/0"
#define NEUI_API_VIDEO    "com.defiantnerd.neui.extension.video/0"
#endif

// Session handle. Treated read-only by host + client convention - do not
// mutate `session` after the host populates it via create_session().
// (Same shape as neui_widget_t / neui_item_t. The struct wrapper exists
// purely for type safety so a session id can't be confused with any
// other uint32_t handle at call sites.)
typedef struct neui_session {
  uint32_t session;             // 0 means "invalid"
} neui_session_t;

// Opaque handle for a tree/menu item.
typedef struct neui_item { uint32_t id; } neui_item_t;

typedef struct neui_client {
  uint32_t neui_version;

  void* (NEUI_ABI *get_interface)(void* token, const char* iface); // get extension for certain objects for a session
} neui_client_t;

// Host base API. Used to create/destroy sessions and get APIs/extensions
// this allows it to be extended in the future without breaking ABI and also to minimize footprint of lightweigt client code

// when creating a session, the client already provides a struct with callbacks.
// Specific instances can be identified by using the token that gets passed back to the client.
typedef struct neui_api {
  uint32_t neui_version;

  neui_session_t (NEUI_ABI *create_session)(neui_client_t* client, void* token);   // creates a new session to work with
  void*          (NEUI_ABI *destroy)(neui_session_t session);                       // destroys a session and gives back the token provided
  void*          (NEUI_ABI *get_interface)(neui_session_t session, const char* iface); // get API for certain objects for a session
  bool           (NEUI_ABI *run)(neui_session_t session);
  // Request the session to end. Emits NEUI_EVENT_APP_QUIT; if the client
  // returns true the root window is closed, otherwise the request is cancelled.
  void           (NEUI_ABI *endsession)(neui_session_t session);
  // Non-blocking variant of run(): drains all pending platform events for
  // this session (mouse, key, paint, timer, accelerator, etc.) and returns.
  // Intended for clients that drive their own outer event loop and want to
  // tick neuilib once per host frame.
  //   returns true  - keep pumping; more events may follow.
  //   returns false - a quit signal arrived (last frame closed); stop.
  // Calling pump_once concurrently with run() on the same session is not
  // supported.
  bool           (NEUI_ABI *pump_once)(neui_session_t session);
} neui_api_t;

#ifdef __cplusplus
}
#endif

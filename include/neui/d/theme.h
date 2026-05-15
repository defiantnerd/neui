#pragma once

#include <stdint.h>
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Optional client-side theme-change notification.
//
// Shape only - not yet wired by hosts in v1. Reserves the ABI so a future
// release can expose system-theme tracking to the client app without
// breaking existing builds. The host queries the client for this
// interface during session creation; if non-null, the host will invoke
// onchange() when the system theme (light/dark, accent colour) changes.
//
// In v1, both hosts already track the system theme internally - clients
// that simply want their UI to follow the OS get that "for free" once
// they opt in via NEUI_ATTR_FOLLOW_SYSTEM_THEME (win32 host) or
// automatically (xpl host). This callback is for clients that need to
// react to a theme change in their own code (e.g. recolour custom
// content).

#define NEUI_API_THEME_CLIENT "com.defiantnerd.neui.extension.theme.client/0"

typedef struct neui_theme_client {
  uint32_t neui_version;

  // Fired on the UI thread after the system theme palette has been
  // updated. Clients typically respond by invalidating their custom
  // drawing.
  void (NEUI_ABI *onchange)(void* token);
} neui_theme_client_t;

#ifdef __cplusplus
}
#endif

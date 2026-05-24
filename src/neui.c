#include <stdio.h>
#include <string.h>
#include "neui/neui.h"

// ---------------------------------------------------------------------------
// Host registry
// ---------------------------------------------------------------------------

#define NEUI_MAX_HOSTS 16

typedef struct {
  const char* id;
  neui_api_t* api;
} neui_host_entry_t;

static neui_host_entry_t neui_registry[NEUI_MAX_HOSTS];
static int neui_registry_count = 0;

void neui_register(const char* id, neui_api_t* api)
{
  if (!id || !api || neui_registry_count >= NEUI_MAX_HOSTS) return;
  neui_registry[neui_registry_count].id  = id;
  neui_registry[neui_registry_count].api = api;
  ++neui_registry_count;
}

neui_api_t* neui_get_api(const char* id)
{
  if (!id)
    return neui_registry_count > 0 ? neui_registry[0].api : NULL;
  for (int i = 0; i < neui_registry_count; ++i) {
    if (strcmp(neui_registry[i].id, id) == 0)
      return neui_registry[i].api;
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Unified startup entry point
// ---------------------------------------------------------------------------
//
// Forward-declare each host's extern "C" registration wrapper. The
// matching NEUI_HAS_*HOST define is set by the root CMakeLists on the
// `neui` target whenever that host's subdirectory is added to the
// build, so the body below only references symbols that the linker
// can actually resolve. The calls themselves double as the forced-
// symbol references that pull each host's object files out of its
// static lib.

#ifdef NEUI_HAS_WIN32HOST
extern void neui_register_win32host(void);
#endif
#ifdef NEUI_HAS_MACOSHOST
extern void neui_register_macoshost(void);
#endif
#ifdef NEUI_HAS_XPLHOST
extern void neui_register_xplhost(void);
#endif

void neui_init(void)
{
  // Native hosts register first so neui_get_api(NULL) returns the
  // native one on platforms that ship one; xpl is the fallback.
#ifdef NEUI_HAS_WIN32HOST
  neui_register_win32host();
#endif
#ifdef NEUI_HAS_MACOSHOST
  neui_register_macoshost();
#endif
#ifdef NEUI_HAS_XPLHOST
  neui_register_xplhost();
#endif
}

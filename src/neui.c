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

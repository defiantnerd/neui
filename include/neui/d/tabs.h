#pragma once

#include <stdint.h>
#include "api.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tabbed-view control (NEUI_W_TABVIEW). The tabs of a tabview are its
// NEUI_W_TABPAGE child widgets, in creation order - index N is the N-th
// TABPAGE child. Each page is a scroll-capable container (reuses the SECTION
// scrolling machinery); set the chip label with widgets->set_text(page, ...)
// and style the chip per-page via NEUI_ATTR_TAB_CHIP_BG_COLOR /
// NEUI_ATTR_TAB_CHIP_TEXT_COLOR. Content widgets are parented INTO a page.
//
// This interface controls which page is active. Tab CRUD is done with the
// generic widget API: create a tab by creating a NEUI_W_TABPAGE child of the
// tabview; remove one by destroying that page widget.
#define NEUI_API_TABS "com.defiantnerd.neui.extension.tabs/0"

typedef struct neui_tabs_api
{
  uint32_t neui_version;

  // Number of tabs (NEUI_W_TABPAGE children) in the tabview. 0 if the widget
  // is not a tabview or the handle is invalid / cross-session.
  uint32_t (NEUI_ABI *count)(neui_session_t session, neui_widget_t tabview);

  // Index of the currently selected tab, or NEUI_ITEM_NONE when there are no
  // tabs (or the widget is not a tabview).
  uint32_t (NEUI_ABI *get_selected)(neui_session_t session, neui_widget_t tabview);

  // Select the tab at `index`. Clamped to [0, count). If the selection
  // actually changes this fires NEUI_EVENT_TAB_DESELECTED for the outgoing
  // tab and NEUI_EVENT_TAB_SELECTED for the incoming one (both before the
  // page swap + repaint), then shows the selected page and hides the rest.
  void (NEUI_ABI *set_selected)(neui_session_t session, neui_widget_t tabview, uint32_t index);

  // The TABPAGE widget at `index`, or { widget_none } if out of range.
  neui_widget_t (NEUI_ABI *get_page)(neui_session_t session, neui_widget_t tabview, uint32_t index);

  // The index of `page` among the tabview's TABPAGE children, or
  // NEUI_ITEM_NONE if `page` is not one of them.
  uint32_t (NEUI_ABI *get_index)(neui_session_t session, neui_widget_t tabview, neui_widget_t page);
} neui_tabs_api_t;

#ifdef __cplusplus
}
#endif

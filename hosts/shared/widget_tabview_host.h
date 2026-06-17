#pragma once

#include <vector>
#include <cstdint>
#include <cstring>

#include <neui/neui.h>

// Host-agnostic TABVIEW page-set + selection logic. The crossplatform, win32,
// and macOS hosts all keep their tab pages as NEUI_W_TABPAGE children of the
// tabview and drive selection identically; that shared logic lives here once
// (the per-host wrappers are thin forwards). Pure geometry stays in
// widget_tabview.h; this file is the part that touches the widget tree + the
// event dispatch, so it is templated over the host Session.
//
// SessionT must expose:
//   _widgets            // a Tree<WidgetData>: child / next / exists / operator[]
//   dispatch_event(neui_event_t*)
// and WidgetData must expose `.type` (const char*) and `.widget_id` (uint32_t).

namespace neui_detail
{
  // Collect the NEUI_W_TABPAGE children of `tv_index` in creation (tab) order.
  template<class SessionT>
  inline void tabview_collect_pages(SessionT* session, uint32_t tv_index,
                                    std::vector<uint32_t>& out)
  {
    out.clear();
    if (!session) return;
    uint32_t c = session->_widgets.child(tv_index);
    while (c != 0) {
      if (session->_widgets.exists(c)) {
        auto& cw = session->_widgets[c];
        if (cw.type && !std::strcmp(cw.type, NEUI_W_TABPAGE))
          out.push_back(c);
      }
      c = session->_widgets.next(c);
    }
  }

  // Commit a tab selection change. Clamps `ni` to [0, count); no-op (returns
  // false) when there are no pages or the selection is unchanged. Otherwise
  // dispatches NEUI_EVENT_TAB_DESELECTED(old) then NEUI_EVENT_TAB_SELECTED(new)
  // BEFORE the page swap, using stable page widget-ids snapshotted up front so
  // a handler that adds/destroys pages can't make the payload read a reused
  // slot; then re-resolves `selected` to the activated page by identity (the
  // page may have shifted index, or be gone - the caller's apply re-clamps).
  // Writes the final index into `selected` and returns true so the caller
  // applies page geometry + repaints.
  template<class SessionT>
  inline bool tabview_commit_selection(SessionT* session, uint32_t tv_widget_id,
                                       uint32_t tv_index, int& selected, int ni)
  {
    if (!session) return false;
    std::vector<uint32_t> pages;
    tabview_collect_pages(session, tv_index, pages);
    int count = static_cast<int>(pages.size());
    if (count == 0) return false;
    if (ni < 0)      ni = 0;
    if (ni >= count) ni = count - 1;
    if (ni == selected) return false;
    int old = selected;

    uint32_t old_page_id    = (old >= 0 && old < count)
                                ? session->_widgets[pages[old]].widget_id : 0;
    uint32_t target_page_id = session->_widgets[pages[ni]].widget_id;

    if (old_page_id) {
      neui_event_t ev{};
      ev.type               = NEUI_EVENT_TAB_DESELECTED;
      ev.data.tab.widget.id = tv_widget_id;
      ev.data.tab.tab_index = static_cast<uint32_t>(old);
      ev.data.tab.page.id   = old_page_id;
      session->dispatch_event(&ev);
    }
    {
      neui_event_t ev{};
      ev.type               = NEUI_EVENT_TAB_SELECTED;
      ev.data.tab.widget.id = tv_widget_id;
      ev.data.tab.tab_index = static_cast<uint32_t>(ni);
      ev.data.tab.page.id   = target_page_id;
      session->dispatch_event(&ev);
    }

    // Re-resolve against the current page set in case a handler mutated the
    // tree, so `selected` follows the page we just activated by identity
    // rather than a now-shifted index.
    std::vector<uint32_t> fresh;
    tabview_collect_pages(session, tv_index, fresh);
    int sel = ni;
    for (int i = 0; i < static_cast<int>(fresh.size()); ++i)
      if (session->_widgets[fresh[i]].widget_id == target_page_id) { sel = i; break; }
    selected = sel;
    return true;
  }
}

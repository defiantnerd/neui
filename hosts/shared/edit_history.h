#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Bounded per-widget undo/redo history shared between text widgets.
//
// State is captured as a triple (text, cursor, anchor). Records are pushed
// BEFORE a mutation, so undo restores the pre-mutation state and redo
// re-applies it. Successive typing or deleting events coalesce into a
// single undo group; any other interaction (cursor movement, mouse click,
// paste, cut, newline, focus change) breaks the run so the next text edit
// starts a fresh group.

namespace neui_detail
{
  struct EditState
  {
    std::string text;
    int         cursor = 0;
    int         anchor = 0;
  };

  class EditHistory
  {
  public:
    enum Action { None, Typing, Deleting };

    static constexpr size_t kMaxDepth = 100;

    // Record the pre-edit state for a modification of the given kind.
    // If `has_selection` is true (the edit replaces a non-empty selection)
    // the run cannot coalesce - the resulting undo step ends here, and the
    // next typed character will begin a new group. The same applies when
    // `kind` differs from the previous run.
    void mark(const EditState& pre, Action kind, bool has_selection)
    {
      bool coalesce = (kind != None) &&
                      (kind == _last_action) &&
                      !has_selection;
      if (!coalesce) {
        _undo.push_back(pre);
        if (_undo.size() > kMaxDepth)
          _undo.erase(_undo.begin());
        _redo.clear();
      }
      _last_action = kind;
    }

    // Force the next edit to start a fresh run. Call on cursor moves, mouse
    // clicks, focus loss, etc.
    void reset_action() { _last_action = None; }

    bool can_undo() const { return !_undo.empty(); }
    bool can_redo() const { return !_redo.empty(); }

    // Pop a pre-state from the undo stack into `out_restored`, pushing the
    // current state onto the redo stack. Returns false if no undo available.
    bool undo(const EditState& current, EditState& out_restored)
    {
      if (_undo.empty()) return false;
      _redo.push_back(current);
      out_restored = _undo.back();
      _undo.pop_back();
      _last_action = None;
      return true;
    }

    // Mirror of undo(): pop from redo into `out_restored`, push current to
    // undo.
    bool redo(const EditState& current, EditState& out_restored)
    {
      if (_redo.empty()) return false;
      _undo.push_back(current);
      out_restored = _redo.back();
      _redo.pop_back();
      _last_action = None;
      return true;
    }

    void clear()
    {
      _undo.clear();
      _redo.clear();
      _last_action = None;
    }

  private:
    std::vector<EditState> _undo;
    std::vector<EditState> _redo;
    Action                 _last_action = None;
  };

} // namespace neui_detail

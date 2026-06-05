#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "edit_history.h"

// Shared single-line text-buffer editing primitives.
//
// Operations work on the buffer (`text`) + two byte offsets (`cursor` is the
// moving end of the selection, `sel_anchor` is the fixed end). When the two
// are equal there is no selection; otherwise the range
// [min(cursor, anchor), max(cursor, anchor)) is selected. All helpers walk
// UTF-8 codepoint boundaries so supplementary characters Just Work.
//
// Used by: GRID in-place cell editor (live), KNOB double-click value entry
// (planned). INPUTBOX / MULTILINE still own their own copies of this logic
// for now (IME + multiline layout entangle the existing implementation);
// migrating those is a follow-up.
//
// The helpers DO NOT call into the platform layer (clipboard, repaint,
// invalidate) or own widget state - the embedder plumbs those. They DO
// optionally feed an EditHistory so undo / redo Just Works once the
// embedder wires Ctrl+Z / Ctrl+Y.

namespace neui_detail
{
  // ---- UTF-8 walking ------------------------------------------------------

  // Length in bytes of the codepoint starting at `s[pos]`. Returns 0 if pos
  // is past the end. Handles malformed sequences by returning 1 (advance one
  // byte at a time) so the caller never gets stuck.
  inline int te_utf8_char_len(const std::string& s, int pos)
  {
    int len = (int)s.size();
    if (pos < 0 || pos >= len) return 0;
    unsigned char b = (unsigned char)s[(size_t)pos];
    if      ((b & 0x80) == 0x00) return 1;
    else if ((b & 0xE0) == 0xC0) return std::min(2, len - pos);
    else if ((b & 0xF0) == 0xE0) return std::min(3, len - pos);
    else if ((b & 0xF8) == 0xF0) return std::min(4, len - pos);
    return 1;
  }

  // Position of the codepoint start immediately before `pos`. Returns 0 if
  // `pos` is already at the start.
  inline int te_utf8_prev_start(const std::string& s, int pos)
  {
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && ((unsigned char)s[(size_t)p] & 0xC0) == 0x80) --p;
    return p;
  }

  // Encode a Unicode codepoint as UTF-8 into out[0..3]. Returns the byte
  // count (1..4). Caller is responsible for guarding against invalid
  // codepoints (e.g. surrogates / > U+10FFFF).
  inline int te_encode_utf8(uint32_t cp, char out[4])
  {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
      out[0] = (char)(0xC0 | (cp >> 6));
      out[1] = (char)(0x80 | (cp & 0x3F));
      return 2;
    }
    if (cp < 0x10000) {
      out[0] = (char)(0xE0 | (cp >> 12));
      out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[2] = (char)(0x80 | (cp & 0x3F));
      return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >>  6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
  }

  // ---- Word boundaries ----------------------------------------------------

  // A "word byte" is alphanumeric ASCII, underscore, or any byte with the high
  // bit set (lead / continuation bytes of UTF-8 multi-byte sequences - treated
  // as part of a word so Latin-extended / CJK selection works without locale
  // data).
  inline bool te_is_word_byte(unsigned char c)
  {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           c == '_' || c >= 0x80;
  }

  // Forward word navigation: returns the next word-start at or after pos.
  // A "word-start" is a position where te_is_word_byte(s[pos]) is true and
  // te_is_word_byte(s[pos-1]) is false (or pos == 0). Lands at end-of-text
  // if there is no further word.
  inline int te_word_right(const std::string& s, int pos)
  {
    int len = (int)s.size();
    if (pos >= len) return len;
    ++pos;
    while (pos < len) {
      bool here = te_is_word_byte((unsigned char)s[(size_t)pos]);
      bool prev = te_is_word_byte((unsigned char)s[(size_t)pos - 1]);
      if (here && !prev) break;
      ++pos;
    }
    return pos;
  }

  // Backward word navigation: returns the previous word-start before pos.
  inline int te_word_left(const std::string& s, int pos)
  {
    if (pos <= 0) return 0;
    --pos;
    while (pos > 0) {
      bool here = te_is_word_byte((unsigned char)s[(size_t)pos]);
      bool prev = te_is_word_byte((unsigned char)s[(size_t)pos - 1]);
      if (here && !prev) break;
      --pos;
    }
    return pos;
  }

  // Returns [start, end) of the contiguous run of same-class bytes containing
  // `pos`. Used by double-click word selection. At end-of-text returns [pos,
  // pos]; on empty text returns [0, 0].
  inline void te_word_bounds(const std::string& s, int pos, int& start, int& end)
  {
    int len = (int)s.size();
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    if (len == 0) { start = end = 0; return; }
    int probe = (pos < len) ? pos : pos - 1;
    bool word = te_is_word_byte((unsigned char)s[(size_t)probe]);
    start = pos;
    while (start > 0 &&
           te_is_word_byte((unsigned char)s[(size_t)start - 1]) == word)
      --start;
    end = (pos < len) ? pos : len;
    while (end < len &&
           te_is_word_byte((unsigned char)s[(size_t)end]) == word)
      ++end;
  }

  // ---- Convenience bundle -------------------------------------------------

  // Pack of the state every editor needs. Embedders that already have
  // independent cursor / anchor fields (InputBoxWidget today) pass refs to
  // the helpers directly; new clients embed this struct.
  struct TextEditState {
    std::string text;
    int         cursor      = 0;   // byte offset, moving end of selection
    int         sel_anchor  = 0;   // byte offset, fixed end of selection
    bool        overwrite   = false;
    // Win32 WM_CHAR delivers UTF-16 units, so a supplementary codepoint
    // arrives across two messages. Non-zero holds a pending high surrogate
    // waiting for its low half; cleared when the pair is assembled (or on
    // any non-surrogate input). Inert on hosts that don't use WM_CHAR.
    uint16_t    pending_high_surrogate = 0;
  };

  // ---- Selection helpers -------------------------------------------------

  inline bool te_has_selection(int cursor, int anchor) { return cursor != anchor; }
  inline int  te_sel_lo(int cursor, int anchor) { return (cursor < anchor) ? cursor : anchor; }
  inline int  te_sel_hi(int cursor, int anchor) { return (cursor > anchor) ? cursor : anchor; }

  // Erase the selection in-place. cursor + anchor end up at the start of the
  // erased range. Returns true if the buffer changed.
  inline bool te_erase_selection(std::string& text, int& cursor, int& anchor)
  {
    if (cursor == anchor) return false;
    int lo = te_sel_lo(cursor, anchor);
    int hi = te_sel_hi(cursor, anchor);
    text.erase((size_t)lo, (size_t)(hi - lo));
    cursor = anchor = lo;
    return true;
  }

  inline void te_select_all(const std::string& text, int& cursor, int& anchor,
                              EditHistory* history)
  {
    if (history) history->reset_action();
    anchor = 0;
    cursor = (int)text.size();
  }

  // Pull the selected substring out of `text`. Returns an empty string when
  // there is no selection. Useful for Ctrl+C / Ctrl+X.
  inline std::string te_selected_text(const std::string& text,
                                       int cursor, int anchor)
  {
    if (cursor == anchor) return {};
    int lo = te_sel_lo(cursor, anchor);
    int hi = te_sel_hi(cursor, anchor);
    return text.substr((size_t)lo, (size_t)(hi - lo));
  }

  // ---- Mutators ----------------------------------------------------------

  // Mark the pre-edit state on `history` (if non-null). Helper so the
  // mutators below stay one-liners.
  inline void te_history_mark(EditHistory* h, const std::string& text,
                                int cursor, int anchor,
                                EditHistory::Action kind, bool has_selection)
  {
    if (h) h->mark(EditState{ text, cursor, anchor }, kind, has_selection);
  }

  // Insert UTF-8 bytes at the cursor, replacing any active selection.
  // Honours `overwrite` (replaces one codepoint forward) when no selection
  // is active. Returns true if the buffer changed.
  inline bool te_insert_utf8(std::string& text, int& cursor, int& anchor,
                              bool overwrite, const char* utf8, int n,
                              EditHistory* history)
  {
    if (!utf8 || n <= 0) return false;
    bool has_sel = te_has_selection(cursor, anchor);
    te_history_mark(history, text, cursor, anchor,
                    EditHistory::Typing, has_sel);
    if (has_sel) {
      te_erase_selection(text, cursor, anchor);
    } else if (overwrite && cursor < (int)text.size()) {
      int existing = te_utf8_char_len(text, cursor);
      text.replace((size_t)cursor, (size_t)existing, utf8, (size_t)n);
      cursor += n;
      anchor = cursor;
      return true;
    }
    text.insert((size_t)cursor, utf8, (size_t)n);
    cursor += n;
    anchor = cursor;
    return true;
  }

  // Backspace one codepoint (or word, when `word`=true) to the left of the
  // cursor. If a selection is active, deletes the selection instead.
  inline bool te_backspace(std::string& text, int& cursor, int& anchor,
                            bool word, EditHistory* history)
  {
    bool has_sel = te_has_selection(cursor, anchor);
    if (!has_sel && cursor <= 0) return false;
    te_history_mark(history, text, cursor, anchor,
                    EditHistory::Deleting, has_sel);
    if (has_sel) {
      te_erase_selection(text, cursor, anchor);
    } else {
      int start = word ? te_word_left(text, cursor)
                       : te_utf8_prev_start(text, cursor);
      text.erase((size_t)start, (size_t)(cursor - start));
      cursor = anchor = start;
    }
    return true;
  }

  // Delete one codepoint (or word) to the right of the cursor; with a
  // selection, deletes the selection.
  inline bool te_delete_forward(std::string& text, int& cursor, int& anchor,
                                  bool word, EditHistory* history)
  {
    int len = (int)text.size();
    bool has_sel = te_has_selection(cursor, anchor);
    if (!has_sel && cursor >= len) return false;
    te_history_mark(history, text, cursor, anchor,
                    EditHistory::Deleting, has_sel);
    if (has_sel) {
      te_erase_selection(text, cursor, anchor);
    } else {
      int end = word ? te_word_right(text, cursor)
                     : cursor + te_utf8_char_len(text, cursor);
      text.erase((size_t)cursor, (size_t)(end - cursor));
    }
    return true;
  }

  // Replace the selection (or insert at cursor) with `paste_text`. Strips
  // CR / LF from the pasted text when `strip_newlines` is set (single-line
  // editors).
  inline bool te_paste(std::string& text, int& cursor, int& anchor,
                        const std::string& paste_text, bool strip_newlines,
                        EditHistory* history)
  {
    std::string clean;
    clean.reserve(paste_text.size());
    if (strip_newlines) {
      for (char c : paste_text) {
        if (c == '\r' || c == '\n') continue;
        clean.push_back(c);
      }
    } else {
      clean = paste_text;
    }
    if (clean.empty() && !te_has_selection(cursor, anchor)) return false;
    bool has_sel = te_has_selection(cursor, anchor);
    te_history_mark(history, text, cursor, anchor,
                    EditHistory::None, has_sel);
    if (has_sel) te_erase_selection(text, cursor, anchor);
    text.insert((size_t)cursor, clean);
    cursor += (int)clean.size();
    anchor = cursor;
    return true;
  }

  // ---- Caret motion ------------------------------------------------------
  // `word` -> step by word (Ctrl+key); `extend` -> keep anchor in place
  // (Shift+key). All caret moves break the undo run on `history`.

  inline void te_move_left(const std::string& text, int& cursor, int& anchor,
                            bool word, bool extend, EditHistory* history)
  {
    (void)text;
    if (history) history->reset_action();
    if (!extend && !word && te_has_selection(cursor, anchor)) {
      cursor = te_sel_lo(cursor, anchor); anchor = cursor; return;
    }
    if (word)        cursor = te_word_left(text, cursor);
    else if (cursor > 0)
                     cursor = te_utf8_prev_start(text, cursor);
    if (!extend) anchor = cursor;
  }

  inline void te_move_right(const std::string& text, int& cursor, int& anchor,
                              bool word, bool extend, EditHistory* history)
  {
    int len = (int)text.size();
    if (history) history->reset_action();
    if (!extend && !word && te_has_selection(cursor, anchor)) {
      cursor = te_sel_hi(cursor, anchor); anchor = cursor; return;
    }
    if (word)            cursor = te_word_right(text, cursor);
    else if (cursor < len)
                          cursor += te_utf8_char_len(text, cursor);
    if (!extend) anchor = cursor;
  }

  inline void te_move_home(const std::string& text, int& cursor, int& anchor,
                             bool extend, EditHistory* history)
  {
    (void)text;
    if (history) history->reset_action();
    cursor = 0;
    if (!extend) anchor = cursor;
  }

  inline void te_move_end(const std::string& text, int& cursor, int& anchor,
                            bool extend, EditHistory* history)
  {
    if (history) history->reset_action();
    cursor = (int)text.size();
    if (!extend) anchor = cursor;
  }

  // ---- Undo / redo round-trip -------------------------------------------
  // Convenience: pulls an EditState out of history into (text, cursor,
  // anchor). Returns true if a state was restored.

  inline bool te_undo(std::string& text, int& cursor, int& anchor,
                       EditHistory& history)
  {
    EditState restored;
    if (!history.undo(EditState{ text, cursor, anchor }, restored))
      return false;
    text   = restored.text;
    cursor = restored.cursor;
    anchor = restored.anchor;
    return true;
  }

  inline bool te_redo(std::string& text, int& cursor, int& anchor,
                       EditHistory& history)
  {
    EditState restored;
    if (!history.redo(EditState{ text, cursor, anchor }, restored))
      return false;
    text   = restored.text;
    cursor = restored.cursor;
    anchor = restored.anchor;
    return true;
  }

} // namespace neui_detail

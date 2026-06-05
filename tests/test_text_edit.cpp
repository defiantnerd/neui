#include "neui_test.h"

#include "text_edit.h"

using namespace neui_detail;

// ---------------------------------------------------------------------------
// UTF-8 walking
// ---------------------------------------------------------------------------

TEST_CASE("te_utf8_char_len: ASCII + multi-byte + end-of-string")
{
  std::string s = "a\xC3\xA4\xE2\x98\x83";   // 'a', 'ä' (2 bytes), '☃' (3 bytes)
  CHECK_EQ(te_utf8_char_len(s, 0), 1);
  CHECK_EQ(te_utf8_char_len(s, 1), 2);
  CHECK_EQ(te_utf8_char_len(s, 3), 3);
  CHECK_EQ(te_utf8_char_len(s, (int)s.size()), 0);
}

TEST_CASE("te_utf8_prev_start skips continuation bytes")
{
  std::string s = "a\xC3\xA4\xE2\x98\x83";
  CHECK_EQ(te_utf8_prev_start(s, 6), 3);   // 6 -> start of '☃'
  CHECK_EQ(te_utf8_prev_start(s, 3), 1);   // 3 -> start of 'ä'
  CHECK_EQ(te_utf8_prev_start(s, 1), 0);
  CHECK_EQ(te_utf8_prev_start(s, 0), 0);
}

TEST_CASE("te_encode_utf8: round-trip for ASCII / Latin-1 / BMP / supplementary")
{
  char buf[4];
  CHECK_EQ(te_encode_utf8('A', buf), 1);
  CHECK_EQ((unsigned char)buf[0], 'A');
  CHECK_EQ(te_encode_utf8(0xE4, buf), 2);            // 'ä'
  CHECK_EQ((unsigned char)buf[0], 0xC3);
  CHECK_EQ((unsigned char)buf[1], 0xA4);
  CHECK_EQ(te_encode_utf8(0x2603, buf), 3);          // '☃'
  CHECK_EQ((unsigned char)buf[0], 0xE2);
  CHECK_EQ(te_encode_utf8(0x1F600, buf), 4);         // emoji
  CHECK_EQ((unsigned char)buf[0], 0xF0);
}

// ---------------------------------------------------------------------------
// Word boundaries
// ---------------------------------------------------------------------------

TEST_CASE("te_word_right / te_word_left step word-by-word")
{
  std::string s = "hello world foo";
  CHECK_EQ(te_word_right(s, 0),  6);   // start of "world"
  CHECK_EQ(te_word_right(s, 6),  12);  // start of "foo"
  CHECK_EQ(te_word_right(s, 12), 15);  // end of string
  CHECK_EQ(te_word_left (s, 15), 12);
  CHECK_EQ(te_word_left (s, 12), 6);
  CHECK_EQ(te_word_left (s, 6),  0);
  CHECK_EQ(te_word_left (s, 0),  0);
}

TEST_CASE("te_word_bounds isolates the word at a position")
{
  std::string s = "  alpha   beta";
  int a, b;
  te_word_bounds(s, 4, a, b);     // inside "alpha"
  CHECK_EQ(a, 2); CHECK_EQ(b, 7);
  te_word_bounds(s, 1, a, b);     // inside leading whitespace run
  CHECK_EQ(a, 0); CHECK_EQ(b, 2);
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

TEST_CASE("te_select_all anchors at 0, cursor at size; te_has_selection reflects it")
{
  std::string t = "hello";
  int c = 5, a = 5;
  CHECK_FALSE(te_has_selection(c, a));
  te_select_all(t, c, a, /*history=*/nullptr);
  CHECK(te_has_selection(c, a));
  CHECK_EQ(te_sel_lo(c, a), 0);
  CHECK_EQ(te_sel_hi(c, a), 5);
}

TEST_CASE("te_erase_selection collapses cursor to the low end")
{
  std::string t = "abcdef";
  int c = 5, a = 1;            // selection [1,5)
  CHECK(te_erase_selection(t, c, a));
  CHECK_EQ(t, std::string("af"));
  CHECK_EQ(c, 1); CHECK_EQ(a, 1);
  // No-op when no selection.
  CHECK_FALSE(te_erase_selection(t, c, a));
}

TEST_CASE("te_selected_text returns the selected substring")
{
  std::string t = "the quick brown fox";
  int c = 9, a = 4;
  CHECK_EQ(te_selected_text(t, c, a), std::string("quick"));
  CHECK(te_selected_text(t, 4, 4).empty());
}

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------

TEST_CASE("te_insert_utf8 replaces an active selection")
{
  std::string t = "hello world";
  int c = 11, a = 6;           // selection over "world"
  EditHistory h;
  CHECK(te_insert_utf8(t, c, a, /*overwrite=*/false, "neui", 4, &h));
  CHECK_EQ(t, std::string("hello neui"));
  CHECK_EQ(c, 10); CHECK_EQ(a, 10);
}

TEST_CASE("te_insert_utf8 with overwrite replaces one codepoint forward")
{
  std::string t = "abc";
  int c = 1, a = 1;
  CHECK(te_insert_utf8(t, c, a, /*overwrite=*/true, "X", 1, nullptr));
  CHECK_EQ(t, std::string("aXc"));
  CHECK_EQ(c, 2);
}

TEST_CASE("te_backspace: word vs codepoint, erases selection when present")
{
  std::string t = "hello world";
  int c = 11, a = 11;
  EditHistory h;
  CHECK(te_backspace(t, c, a, /*word=*/false, &h));   // erase 'd'
  CHECK_EQ(t, std::string("hello worl"));
  CHECK(te_backspace(t, c, a, /*word=*/true,  &h));   // erase "worl"
  CHECK_EQ(t, std::string("hello "));
  // With a selection, just erases the selection regardless of word.
  c = 5; a = 0;
  CHECK(te_backspace(t, c, a, /*word=*/false, &h));
  CHECK_EQ(t, std::string(" "));
}

TEST_CASE("te_delete_forward mirrors backspace on the other side")
{
  std::string t = "hello world";
  int c = 0, a = 0;
  EditHistory h;
  // word-delete-forward erases everything up to the next word-start, so
  // "hello " (including the trailing space) goes.
  CHECK(te_delete_forward(t, c, a, /*word=*/true, &h));
  CHECK_EQ(t, std::string("world"));
}

TEST_CASE("te_paste strips newlines on single-line, replaces selection")
{
  std::string t = "abc";
  int c = 3, a = 3;
  EditHistory h;
  CHECK(te_paste(t, c, a, "hi\r\nworld\n!", /*strip_newlines=*/true, &h));
  CHECK_EQ(t, std::string("abchiworld!"));
  // Newlines preserved when strip_newlines=false.
  t = ""; c = 0; a = 0;
  CHECK(te_paste(t, c, a, "x\ny", /*strip_newlines=*/false, &h));
  CHECK_EQ(t, std::string("x\ny"));
}

// ---------------------------------------------------------------------------
// Caret motion - shift+key extends, ctrl+key steps by word
// ---------------------------------------------------------------------------

TEST_CASE("te_move_left collapses a selection when not extending")
{
  std::string t = "hello";
  int c = 5, a = 0;
  EditHistory h;
  te_move_left(t, c, a, /*word=*/false, /*extend=*/false, &h);
  CHECK_EQ(c, 0); CHECK_EQ(a, 0);   // collapsed to lo
}

TEST_CASE("te_move_left with extend keeps anchor in place")
{
  std::string t = "hello";
  int c = 5, a = 5;
  EditHistory h;
  te_move_left(t, c, a, /*word=*/false, /*extend=*/true, &h);
  CHECK_EQ(c, 4); CHECK_EQ(a, 5);   // selection of "o"
}

TEST_CASE("te_move_right with word jumps to next word-start")
{
  std::string t = "hello world";
  int c = 0, a = 0;
  EditHistory h;
  te_move_right(t, c, a, /*word=*/true, /*extend=*/false, &h);
  CHECK_EQ(c, 6); CHECK_EQ(a, 6);   // start of "world"
}

TEST_CASE("te_move_home / te_move_end honour extend")
{
  std::string t = "hello";
  int c = 3, a = 3;
  EditHistory h;
  te_move_home(t, c, a, /*extend=*/true, &h);
  CHECK_EQ(c, 0); CHECK_EQ(a, 3);
  te_move_end (t, c, a, /*extend=*/false, &h);
  CHECK_EQ(c, 5); CHECK_EQ(a, 5);
}

// ---------------------------------------------------------------------------
// Undo / redo round-trip
// ---------------------------------------------------------------------------

TEST_CASE("te_undo / te_redo cycle a sequence of typing edits")
{
  std::string t;
  int c = 0, a = 0;
  EditHistory h;
  te_insert_utf8(t, c, a, false, "h", 1, &h);
  te_insert_utf8(t, c, a, false, "i", 1, &h);
  CHECK_EQ(t, std::string("hi"));
  CHECK(te_undo(t, c, a, h));
  CHECK_EQ(t, std::string(""));   // the typing run coalesced into one step
  CHECK(te_redo(t, c, a, h));
  CHECK_EQ(t, std::string("hi"));
}

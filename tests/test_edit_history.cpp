#include "neui_test.h"

#include "edit_history.h"

using namespace neui_detail;

static EditState st(const char* text, int cursor = 0)
{
  EditState s;
  s.text = text;
  s.cursor = cursor;
  s.anchor = cursor;
  return s;
}

TEST_CASE("EditHistory: undo restores pre-state, redo re-applies")
{
  EditHistory h;
  CHECK_FALSE(h.can_undo());
  CHECK_FALSE(h.can_redo());

  // Pre-state "a" recorded before mutating to "ab".
  h.mark(st("a", 1), EditHistory::Typing, false);
  EditState current = st("ab", 2);

  EditState restored;
  CHECK(h.undo(current, restored));
  CHECK_EQ(restored.text, std::string("a"));
  CHECK(h.can_redo());

  EditState now = st("a", 1);
  EditState re;
  CHECK(h.redo(now, re));
  CHECK_EQ(re.text, std::string("ab"));
}

TEST_CASE("EditHistory: consecutive same-action typing coalesces to one step")
{
  EditHistory h;
  h.mark(st("a"), EditHistory::Typing, false);
  h.mark(st("ab"), EditHistory::Typing, false);   // coalesces - no new record
  h.mark(st("abc"), EditHistory::Typing, false);  // coalesces

  EditState restored;
  CHECK(h.undo(st("abcd"), restored));
  CHECK_EQ(restored.text, std::string("a"));   // jumps back to the run start
  CHECK_FALSE(h.can_undo());                    // only one step existed
}

TEST_CASE("EditHistory: a different action breaks the coalescing run")
{
  EditHistory h;
  h.mark(st("a"), EditHistory::Typing, false);
  h.mark(st("ab"), EditHistory::Deleting, false);   // new group

  EditState restored;
  CHECK(h.undo(st("a"), restored));
  CHECK_EQ(restored.text, std::string("ab"));   // undo the delete group
  CHECK(h.can_undo());                            // typing group still there
  CHECK(h.undo(st("ab"), restored));
  CHECK_EQ(restored.text, std::string("a"));
}

TEST_CASE("EditHistory: a non-empty selection breaks coalescing")
{
  EditHistory h;
  h.mark(st("a"), EditHistory::Typing, false);
  h.mark(st("ab"), EditHistory::Typing, true);   // replaced selection -> new group

  CHECK(h.can_undo());
  EditState r;
  CHECK(h.undo(st("aX"), r));
  CHECK_EQ(r.text, std::string("ab"));
  CHECK(h.undo(st("ab"), r));
  CHECK_EQ(r.text, std::string("a"));
}

TEST_CASE("EditHistory: a fresh mark clears the redo stack")
{
  EditHistory h;
  h.mark(st("a"), EditHistory::Typing, false);
  EditState r;
  h.undo(st("ab"), r);
  CHECK(h.can_redo());

  h.mark(st("ax"), EditHistory::Typing, false);   // new edit invalidates redo
  CHECK_FALSE(h.can_redo());
}

TEST_CASE("EditHistory: reset_action forces the next mark to start a new group")
{
  EditHistory h;
  h.mark(st("a"), EditHistory::Typing, false);
  h.reset_action();
  h.mark(st("ab"), EditHistory::Typing, false);   // would coalesce, but reset broke it

  EditState r;
  CHECK(h.undo(st("abc"), r));
  CHECK_EQ(r.text, std::string("ab"));
  CHECK(h.can_undo());
}

TEST_CASE("EditHistory: depth is capped at kMaxDepth, oldest dropped")
{
  EditHistory h;
  const int n = (int)EditHistory::kMaxDepth + 50;
  for (int i = 0; i < n; ++i)
    h.mark(st(std::to_string(i).c_str()), EditHistory::Typing, /*break*/true);

  int undos = 0;
  EditState r;
  while (h.undo(st("cur"), r)) ++undos;
  CHECK_EQ(undos, (int)EditHistory::kMaxDepth);
}

TEST_CASE("EditHistory: undo / redo on empty stacks return false")
{
  EditHistory h;
  EditState r;
  CHECK_FALSE(h.undo(st("x"), r));
  CHECK_FALSE(h.redo(st("x"), r));
}

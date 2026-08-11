#include "neui_test.h"

#include "file_dialog_model.h"

using namespace neui_detail;

// ---- glob matching ---------------------------------------------------------

TEST_CASE("file dialog: glob matches literal and wildcard extensions")
{
  CHECK(glob_match("shot.png", "*.png"));
  CHECK(glob_match("a.b.png", "*.png"));
  CHECK(!glob_match("shot.pngx", "*.png"));
  CHECK(!glob_match("png", "*.png"));
  // "*" alone accepts anything, including a name with no dot at all.
  CHECK(glob_match("Makefile", "*"));
  CHECK(glob_match("", "*"));
  // "*.*" needs a dot - this is why matches_everything() special-cases it
  // rather than relying on the matcher.
  CHECK(!glob_match("Makefile", "*.*"));
}

TEST_CASE("file dialog: glob is ASCII case-insensitive both ways")
{
  CHECK(glob_match("IMG.PNG", "*.png"));
  CHECK(glob_match("img.png", "*.PNG"));
  CHECK(glob_match("MiXeD.PnG", "*.pNg"));
}

TEST_CASE("file dialog: glob '?' matches exactly one character")
{
  CHECK(glob_match("a.pg", "?.pg"));
  CHECK(!glob_match("ab.pg", "?.pg"));
  CHECK(!glob_match(".pg", "?.pg"));
  CHECK(glob_match("v1.wav", "v?.wav"));
}

TEST_CASE("file dialog: glob backtracks over multiple stars")
{
  CHECK(glob_match("aXbXc.txt", "a*b*c.txt"));
  CHECK(!glob_match("aXbXd.txt", "a*b*c.txt"));
  // Trailing stars are absorbed, so the pattern still terminates.
  CHECK(glob_match("abc", "abc***"));
  // The pathological backtracking case: must terminate, must not match.
  CHECK(!glob_match("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "*a*a*a*a*a*a*b"));
}

TEST_CASE("file dialog: glob rejects null inputs rather than crashing")
{
  CHECK(!glob_match(nullptr, "*.png"));
  CHECK(!glob_match("x.png", nullptr));
}

// ---- filter parsing --------------------------------------------------------

TEST_CASE("file dialog: pattern list splits on ';' and trims")
{
  auto p = parse_filter_patterns("*.png; *.jpg ;;*.gif");
  REQUIRE_EQ(p.size(), (size_t)3);
  CHECK_EQ(p[0], std::string("*.png"));
  CHECK_EQ(p[1], std::string("*.jpg"));
  CHECK_EQ(p[2], std::string("*.gif"));
  CHECK_EQ(parse_filter_patterns(nullptr).size(), (size_t)0);
  CHECK_EQ(parse_filter_patterns("").size(), (size_t)0);
  CHECK_EQ(parse_filter_patterns("  ").size(), (size_t)0);
}

TEST_CASE("file dialog: descriptor decodes to filters, empty entries dropped")
{
  neui_file_filter_t f[] = {
    { "PNG images", "*.png;*.PNG" },
    { "Broken",     ""            },   // no usable pattern -> dropped
    { nullptr,      "*.wav"       },   // no label -> falls back to the pattern
    { "All files",  "*"           },
  };
  neui_file_dialog_t d = {};
  d.filters = f; d.filter_count = 4;

  auto fl = parse_filters(&d);
  REQUIRE_EQ(fl.size(), (size_t)3);
  CHECK_EQ(fl[0].label, std::string("PNG images"));
  REQUIRE_EQ(fl[0].patterns.size(), (size_t)2);
  CHECK_EQ(fl[1].label, std::string("*.wav"));
  CHECK(fl[2].matches_everything());
  CHECK(!fl[0].matches_everything());
}

TEST_CASE("file dialog: no filters at all decodes to an empty list")
{
  CHECK_EQ(parse_filters(nullptr).size(), (size_t)0);
  neui_file_dialog_t d = {};
  CHECK_EQ(parse_filters(&d).size(), (size_t)0);
  // A non-null list with a zero count is still nothing.
  neui_file_filter_t f[] = { { "PNG", "*.png" } };
  d.filters = f; d.filter_count = 0;
  CHECK_EQ(parse_filters(&d).size(), (size_t)0);
}

TEST_CASE("file dialog: default_filter clamps instead of going out of range")
{
  neui_file_filter_t f[] = { { "A", "*.a" }, { "B", "*.b" }, { "C", "*.c" } };
  neui_file_dialog_t d = {};
  d.filters = f; d.filter_count = 3;
  auto fl = parse_filters(&d);
  CHECK_EQ(fl.size(), (size_t)3);

  d.default_filter = 0;  CHECK_EQ(clamp_default_filter(&d, fl), (size_t)0);
  d.default_filter = 2;  CHECK_EQ(clamp_default_filter(&d, fl), (size_t)2);
  d.default_filter = 3;  CHECK_EQ(clamp_default_filter(&d, fl), (size_t)0);
  d.default_filter = 99; CHECK_EQ(clamp_default_filter(&d, fl), (size_t)0);
  // Empty list -> 0 regardless (callers must not index with it).
  std::vector<FileFilter> none;
  d.default_filter = 5;  CHECK_EQ(clamp_default_filter(&d, none), (size_t)0);
  CHECK_EQ(clamp_default_filter(nullptr, fl), (size_t)0);
}

TEST_CASE("file dialog: default_filter follows the entry, not the parsed slot")
{
  // parse_filters DROPS the malformed entry, so the client's index 2 ("C") is
  // at parsed slot 1. A plain `index < size()` clamp would see 2 >= 2 and pick
  // slot 0 ("B") - silently the wrong filter, which is worse than the
  // documented fallback because nothing about it looks like a fallback.
  neui_file_filter_t f[] = {
    { "A", ""      },   // no usable pattern -> dropped
    { "B", "*.b"   },
    { "C", "*.c"   },
  };
  neui_file_dialog_t d = {};
  d.filters = f; d.filter_count = 3;
  auto fl = parse_filters(&d);
  REQUIRE_EQ(fl.size(), (size_t)2);
  CHECK_EQ(fl[0].source_index, (uint32_t)1);
  CHECK_EQ(fl[1].source_index, (uint32_t)2);

  d.default_filter = 2;
  CHECK_EQ(clamp_default_filter(&d, fl), (size_t)1);
  CHECK_EQ(fl[clamp_default_filter(&d, fl)].label, std::string("C"));

  d.default_filter = 1;
  CHECK_EQ(clamp_default_filter(&d, fl), (size_t)0);

  // An index that named a DROPPED entry has no surviving answer -> fall back
  // to 0 rather than guessing at the neighbour the client did not ask for.
  d.default_filter = 0;
  CHECK_EQ(clamp_default_filter(&d, fl), (size_t)0);
  CHECK_EQ(fl[clamp_default_filter(&d, fl)].label, std::string("B"));
}

TEST_CASE("file dialog: filter_accepts folds the pattern list")
{
  FileFilter f; f.patterns = { "*.png", "*.jpg" };
  CHECK(filter_accepts(f, "a.png"));
  CHECK(filter_accepts(f, "a.JPG"));
  CHECK(!filter_accepts(f, "a.gif"));
  // A wildcard entry short-circuits even for a dotless name.
  FileFilter all; all.patterns = { "*.dat", "*" };
  CHECK(filter_accepts(all, "Makefile"));
}

// ---- default extension -----------------------------------------------------

TEST_CASE("file dialog: default_extension skips wildcards and '*' entries")
{
  FileFilter a; a.patterns = { "*.preset" };
  CHECK_EQ(a.default_extension(), std::string("preset"));
  // First usable pattern wins.
  FileFilter b; b.patterns = { "*.png", "*.jpg" };
  CHECK_EQ(b.default_extension(), std::string("png"));
  // A "*" entry offers nothing to complete with.
  FileFilter c; c.patterns = { "*" };
  CHECK_EQ(c.default_extension(), std::string(""));
  // Nor does a wildcarded extension - "*.p?g" has no single answer.
  FileFilter d; d.patterns = { "*.p?g" };
  CHECK_EQ(d.default_extension(), std::string(""));
  // ...but it falls through to a later usable one.
  FileFilter e; e.patterns = { "*", "*.p?g", "*.wav" };
  CHECK_EQ(e.default_extension(), std::string("wav"));
  // A pattern with a trailing dot has no extension either.
  FileFilter g; g.patterns = { "*." };
  CHECK_EQ(g.default_extension(), std::string(""));
}

TEST_CASE("file dialog: save completes a bare name with the filter extension")
{
  FileFilter f; f.patterns = { "*.preset" };
  CHECK_EQ(complete_extension("lead", f), std::string("lead.preset"));
}

TEST_CASE("file dialog: save leaves an existing extension alone")
{
  FileFilter f; f.patterns = { "*.preset" };
  // Even a non-matching one: obeying a deliberate ".txt" beats appending.
  CHECK_EQ(complete_extension("notes.txt", f), std::string("notes.txt"));
  CHECK_EQ(complete_extension("lead.preset", f), std::string("lead.preset"));
}

TEST_CASE("file dialog: save does not treat a dotfile as having an extension")
{
  FileFilter f; f.patterns = { "*.preset" };
  CHECK_EQ(complete_extension(".bashrc", f), std::string(".bashrc.preset"));
  CHECK(!has_extension(".bashrc"));
  CHECK(!has_extension("plain"));
  CHECK(!has_extension("trailing."));
  CHECK(has_extension("a.b"));
}

TEST_CASE("file dialog: save with an all-files filter cannot complete")
{
  FileFilter f; f.patterns = { "*" };
  CHECK_EQ(complete_extension("lead", f), std::string("lead"));
  CHECK_EQ(complete_extension("", f), std::string(""));
}

// ---- paths -----------------------------------------------------------------

TEST_CASE("file dialog: path_join inserts exactly one separator")
{
  CHECK_EQ(path_join("/home/u", "f.txt"), std::string("/home/u/f.txt"));
  CHECK_EQ(path_join("/home/u/", "f.txt"), std::string("/home/u/f.txt"));
  CHECK_EQ(path_join("/", "f.txt"), std::string("/f.txt"));
  CHECK_EQ(path_join("", "f.txt"), std::string("f.txt"));
  CHECK_EQ(path_join("/home/u", ""), std::string("/home/u"));
}

TEST_CASE("file dialog: path_join keeps the directory's separator style")
{
  // Re-joining what path_parent took apart must not mix separators - this is
  // the round trip the save-completion path performs on every platform.
  CHECK_EQ(path_join("C:\\dir", "f.txt"), std::string("C:\\dir\\f.txt"));
  CHECK_EQ(path_join("C:\\dir\\", "f.txt"), std::string("C:\\dir\\f.txt"));
  CHECK_EQ(path_join(path_parent("C:\\a\\b\\old.txt"), "new.preset"),
           std::string("C:\\a\\b\\new.preset"));
  // A mixed input follows its LAST separator.
  CHECK_EQ(path_join("C:/a\\b", "f"), std::string("C:/a\\b\\f"));
  // No separator to copy -> '/'.
  CHECK_EQ(path_join("dir", "f"), std::string("dir/f"));
}

TEST_CASE("file dialog: an absolute leaf overrides the directory")
{
  CHECK_EQ(path_join("/etc", "/tmp/x"), std::string("/tmp/x"));
  CHECK(path_is_absolute("/tmp"));
  CHECK(path_is_absolute("C:\\tmp"));
  CHECK(path_is_absolute("C:/tmp"));
  CHECK(!path_is_absolute("tmp"));
  CHECK(!path_is_absolute(""));
  CHECK(!path_is_absolute("C:"));
}

TEST_CASE("file dialog: path_parent walks up and stops at the root")
{
  CHECK_EQ(path_parent("/home/u/f.txt"), std::string("/home/u"));
  CHECK_EQ(path_parent("/home/u"), std::string("/home"));
  CHECK_EQ(path_parent("/home"), std::string("/"));
  CHECK_EQ(path_parent("/"), std::string("/"));
  // A trailing separator must not yield the same directory back.
  CHECK_EQ(path_parent("/home/u/"), std::string("/home"));
  // Nowhere to go from a bare relative leaf.
  CHECK_EQ(path_parent("f.txt"), std::string("f.txt"));
  CHECK_EQ(path_parent(""), std::string(""));
}

TEST_CASE("file dialog: path_leaf ignores trailing separators")
{
  CHECK_EQ(path_leaf("/a/b/c"), std::string("c"));
  CHECK_EQ(path_leaf("/a/b/"), std::string("b"));
  CHECK_EQ(path_leaf("c"), std::string("c"));
  CHECK_EQ(path_leaf("/"), std::string(""));
}

// ---- directory listing model ----------------------------------------------

static std::vector<FileEntry> raw_listing()
{
  return {
    { "zebra.png", false },
    { ".",         true  },
    { "..",        true  },
    { "Apple",     true  },
    { ".hidden",   false },
    { ".git",      true  },
    { "notes.txt", false },
    { "beta",      true  },
    { "IMG.PNG",   false },
  };
}

TEST_CASE("file dialog: listing drops dot entries and sorts dirs first")
{
  auto v = list_directory_view(raw_listing(), nullptr, false);
  // "." / ".." / dot-files gone; dirs (Apple, beta) before files.
  REQUIRE_EQ(v.size(), (size_t)5);
  CHECK_EQ(v[0].name, std::string("Apple"));
  CHECK(v[0].is_dir);
  CHECK_EQ(v[1].name, std::string("beta"));
  CHECK(v[1].is_dir);
  CHECK_EQ(v[2].name, std::string("IMG.PNG"));
  CHECK_EQ(v[3].name, std::string("notes.txt"));
  CHECK_EQ(v[4].name, std::string("zebra.png"));
}

TEST_CASE("file dialog: show_hidden reveals dot-files but never '.' or '..'")
{
  auto v = list_directory_view(raw_listing(), nullptr, true);
  CHECK_EQ(v.size(), (size_t)7);
  for (const auto& e : v) {
    CHECK(e.name != std::string("."));
    CHECK(e.name != std::string(".."));
  }
  CHECK_EQ(v[0].name, std::string(".git"));   // dir, sorts into the dir group
  CHECK(v[0].is_dir);
}

TEST_CASE("file dialog: a type filter never hides directories")
{
  FileFilter png; png.patterns = { "*.png" };
  auto v = list_directory_view(raw_listing(), &png, false);
  // Both dirs survive; only the two .png files remain of the files.
  REQUIRE_EQ(v.size(), (size_t)4);
  CHECK_EQ(v[0].name, std::string("Apple"));
  CHECK_EQ(v[1].name, std::string("beta"));
  CHECK_EQ(v[2].name, std::string("IMG.PNG"));
  CHECK_EQ(v[3].name, std::string("zebra.png"));
}

TEST_CASE("file dialog: listing sort is a total order over case variants")
{
  std::vector<FileEntry> raw = {
    { "b", false }, { "A", false }, { "a", false }, { "B", false },
  };
  auto v = list_directory_view(raw, nullptr, false);
  REQUIRE_EQ(v.size(), (size_t)4);
  // Case-insensitive groups the pairs; the tiebreak orders within a pair
  // deterministically ('A' < 'a' by byte value).
  CHECK_EQ(v[0].name, std::string("A"));
  CHECK_EQ(v[1].name, std::string("a"));
  CHECK_EQ(v[2].name, std::string("B"));
  CHECK_EQ(v[3].name, std::string("b"));
  // Neither ordering direction may hold for a pair, i.e. it is irreflexive.
  FileEntry x{ "a", false }, y{ "A", false };
  CHECK(!(file_entry_less(x, y) && file_entry_less(y, x)));
  CHECK(!file_entry_less(x, x));
}

TEST_CASE("file dialog: empty listing yields empty view")
{
  CHECK_EQ(list_directory_view({}, nullptr, true).size(), (size_t)0);
}

// File-dialog acceptance harness (NEUI_API_NOTIFY::open_file / save_file),
// macOS / both hosts.
//
// The portable half - glob matching, filter decode, extension completion,
// listing order, path join/parent - is Tier-1 tested in
// tests/test_file_dialog_model.cpp. What that cannot reach is the part that
// only a real panel shows:
//
//   A. WIRING     - the appended vtable slots are non-NULL on BOTH macOS hosts
//                   (native and xpl) and reach NSOpenPanel / NSSavePanel.
//   B. VALIDATION - a bad / non-frame / cross-session widget returns -1 (no
//                   dialog), which the contract keeps DISTINCT from 0 (the user
//                   cancelled). This is the distinction a client needs to
//                   decide whether to offer its own path entry.
//   C. CANCEL     - dismissing the panel returns exactly 0 and fires no
//                   callback. A stray callback here would look to a client
//                   like a pick.
//   D. CONFIRM    - confirming a save panel returns exactly 1 path, built from
//                   the requested directory and name, and carrying the active
//                   filter's extension (the documented completion rule). This
//                   is a real end-to-end assertion: the path comes back out of
//                   AppKit, not out of our own descriptor.
//   D2. OPEN OK   - the open path's SUCCESS branch, which the save checks
//                   above never touch: `[panel URLs]` collection and the
//                   per-path callback fan-out. Driven as a folder pick, since
//                   that is the one open flavour whose result can be produced
//                   without a real click on a file row.
//   E. DESCRIPTOR - a NULL descriptor is legal ("every default"), and an
//                   out-of-range default_filter clamps instead of indexing off
//                   the end of the filter array.
//
// The panels are resolved from [NSApp modalWindow] and dismissed the way a
// button click would: cancel through the panel's own -cancel: action, confirm
// by ending the modal session with NSModalResponseOK (modern NSSavePanel runs
// out of process, so -ok: raises "not implemented" and the real Save button is
// unreachable from here - see schedule_panel_action). Nothing test-only is
// added to the public API, and the asserted path is the one AppKit reports, so
// the harness cannot pass by agreeing with itself.
//
// Needs a GUI session (it realizes a real NSWindow and runs modal panels), so
// it is built but NOT ctest-registered; run
// ./tests/<config>/neui_file_dialog_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdio>
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void check_eq_int(int got, int want, const char* what)
{
  bool ok = (got == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) { std::printf("        got %d, want %d\n", got, want); ++g_failures; }
}

// Compare a path the panel produced against an expected directory + leaf.
//
// Not a string compare: on macOS /tmp is a symlink to /private/tmp and AppKit
// canonicalises the panel's directoryURL at a moment of its own choosing, so a
// literal comparison passes or fails depending on whether the panel had
// navigated yet. Resolving both sides with realpath() asserts what the test
// actually means - same directory, same file name - without pinning AppKit's
// symlink behaviour.
void check_path(const std::string& got, const std::string& want_dir,
                const std::string& want_leaf, const char* what)
{
  auto resolve = [](const std::string& p) {
    char buf[PATH_MAX];
    const char* r = realpath(p.c_str(), buf);
    return std::string(r ? r : p.c_str());
  };
  size_t slash = got.rfind('/');
  std::string got_dir  = (slash == std::string::npos) ? "" : got.substr(0, slash);
  std::string got_leaf = (slash == std::string::npos) ? got : got.substr(slash + 1);

  bool ok = (resolve(got_dir) == resolve(want_dir)) && (got_leaf == want_leaf);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) {
    std::printf("        got  \"%s\"\n", got.c_str());
    std::printf("        want dir \"%s\" (%s) leaf \"%s\"\n",
                want_dir.c_str(), resolve(want_dir).c_str(), want_leaf.c_str());
    ++g_failures;
  }
}

// ---- callback plumbing -----------------------------------------------------

std::vector<std::string> g_paths;
int                      g_cb_calls = 0;

void NEUI_ABI collect_path(void* userdata, const char* path)
{
  ++g_cb_calls;
  if (path) g_paths.push_back(path);
  // The userdata must arrive unchanged - a client keying off it would
  // otherwise write into the wrong object.
  if (userdata != (void*)0x1234) {
    std::printf("[FAIL]  callback userdata was not passed through\n");
    ++g_failures;
  }
}

void reset_capture()
{
  g_paths.clear();
  g_cb_calls = 0;
}

// ---- panel driving ---------------------------------------------------------

// Dismiss the modal panel as confirmed (`ok`) or cancelled. Retries because
// the panel is not the modal window until AppKit has finished putting it up,
// and runModal only spins the runloop after we have returned from the call
// that scheduled this.
//
// Two different mechanisms, for a reason worth stating:
//
//   - Cancel goes through the panel's own -cancel: action, i.e. exactly what
//     clicking the button does.
//   - Confirm CANNOT: modern NSSavePanel is hosted out of process
//     (com.apple.appkit.xpc.openAndSavePanelService) and -[NSSavePanel ok:]
//     raises "not implemented", while the real Save button lives in a remote
//     view this process cannot reach. So confirm ends the modal session with
//     NSModalResponseOK - which is what the panel itself ultimately does - and
//     the helper then reads [panel URL] as it would after a real click.
//
// What that means for coverage: the assertions below still exercise OUR code
// end to end (URL -> UTF-8 path -> extension completion -> callback), and the
// URL still comes from AppKit rather than from our descriptor. What they do
// NOT cover is the panel's internal confirm handling - its overwrite prompt
// and name validation - which is AppKit's code, not neui's.
void schedule_panel_action(bool ok, int attempt = 0)
{
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(60 * NSEC_PER_MSEC)),
                 dispatch_get_main_queue(), ^{
    NSWindow* w = [NSApp modalWindow];
    if ([w isKindOfClass:[NSSavePanel class]]) {
      if (ok) [NSApp stopModalWithCode:NSModalResponseOK];
      else    [(NSSavePanel*)w cancel:nil];
      return;
    }
    if (attempt < 50) { schedule_panel_action(ok, attempt + 1); return; }
    // Never found it: fail loudly rather than hanging the harness forever.
    std::printf("[FAIL]  no NSSavePanel became the modal window\n");
    ++g_failures;
    [NSApp abortModal];
  });
}

// ---- client ----------------------------------------------------------------

bool NEUI_ABI onevent(void*, neui_event_t*) { return false; }
neui_widget_client_t g_widget_client = { NEUI_VERSION, nullptr, onevent };

void* NEUI_ABI get_interface(void*, const char* iface)
{
  if (iface && std::strcmp(iface, NEUI_API_WIDGETS) == 0) return &g_widget_client;
  return nullptr;
}

// A writable directory to aim the save panel at. /tmp is guaranteed present
// and writable, and the harness never actually creates the file - save_file
// returns a path, it does not touch the filesystem.
const char* kDir = "/tmp";

} // namespace

int main()
{
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    neui_init();

    neui_client_t client = { NEUI_VERSION, get_interface };

    // ---- A. wiring: both hosts expose the appended slots ------------------
    //
    // The vtable-append rule means a client feature-detects on the slot being
    // non-NULL, so an appended slot left at zero is the failure mode that
    // matters. Check the NATIVE host too: on macOS neui_get_api(NULL) hands it
    // back first, so a client taking the default gets THIS vtable.
    // Not just a non-null check: the native host is what examples/main.cpp
    // actually uses on macOS (ACTIVE_HOST = "neui.host.macos"), so it gets a
    // real panel driven end to end here too. Its validation path differs from
    // the xpl host's - it resolves an NSWindow rather than a native_handle -
    // and that is the part a shared helper cannot cover.
    {
      neui_api_t* native = neui_get_api("neui.host.macos");
      check(native != nullptr, "the native macOS host is registered");
      if (native) {
        neui_session_t s = native->create_session(&client, nullptr);
        auto* w = (neui_widget_api_t*)native->get_interface(s, NEUI_API_WIDGETS);
        auto* n = (neui_notify_api_t*)native->get_interface(s, NEUI_API_NOTIFY);
        check(n != nullptr, "native host: NEUI_API_NOTIFY is present");
        if (n && w) {
          check(n->open_file != nullptr, "native host: open_file slot is wired");
          check(n->save_file != nullptr, "native host: save_file slot is wired");

          neui_widget_t nwin = w->create(s, widget_none, NEUI_W_APPWINDOW,
                                         140, 140, 400, 240, nullptr);
          // Before show() there is no NSWindow, so the native host must report
          // -1 rather than putting up an unowned panel.
          check_eq_int(n->save_file(s, nwin, nullptr, collect_path, (void*)0x1234), -1,
                       "native host: an unrealised frame returns -1");
          w->show(s, nwin);

          neui_file_filter_t nf[] = { { "Presets", "*.preset" } };
          neui_file_dialog_t d = {};
          d.title = "Native host save"; d.initial_dir = kDir;
          d.initial_name = "native-lead";
          d.filters = nf; d.filter_count = 1;

          reset_capture();
          schedule_panel_action(/*ok=*/true);
          check_eq_int(n->save_file(s, nwin, &d, collect_path, (void*)0x1234), 1,
                       "native host: a confirmed save panel returns 1");
          if (g_paths.size() == 1)
            check_path(g_paths[0], kDir, "native-lead.preset",
                       "native host: the path is completed the same way");

          reset_capture();
          schedule_panel_action(/*ok=*/false);
          check_eq_int(n->open_file(s, nwin, &d, collect_path, (void*)0x1234), 0,
                       "native host: a cancelled open panel returns 0");
          check_eq_int(g_cb_calls, 0, "native host: ...and fires no callback");
        }
        native->destroy(s);
      }
    }

    neui_api_t* neui = neui_get_api("neui.host.crossplatform");
    if (!neui) { std::printf("[FAIL] xpl host not registered\n"); return 1; }

    neui_session_t sess = neui->create_session(&client, nullptr);
    auto* widgets = (neui_widget_api_t*)neui->get_interface(sess, NEUI_API_WIDGETS);
    auto* notify  = (neui_notify_api_t*)neui->get_interface(sess, NEUI_API_NOTIFY);
    if (!widgets || !notify) {
      std::printf("[FAIL] missing interfaces\n");
      return 1;
    }
    check(notify->open_file != nullptr, "xpl host: open_file slot is wired");
    check(notify->save_file != nullptr, "xpl host: save_file slot is wired");

    neui_widget_t win = widgets->create(sess, widget_none, NEUI_W_APPWINDOW,
                                        100, 100, 520, 300, nullptr);
    neui_widget_t label = widgets->create(sess, win, NEUI_W_LABEL,
                                          10, 10, 200, 20, nullptr);
    widgets->show(sess, win);
    [NSApp activateIgnoringOtherApps:YES];

    neui_file_filter_t filters[] = {
      { "Presets",   "*.preset" },
      { "All files", "*"        },
    };

    // ---- B. validation: -1 means "no dialog ran" --------------------------
    //
    // Distinct from 0 (cancelled) on purpose - a client that cannot get a
    // dialog may want to fall back to its own path entry, and must not do
    // that just because someone pressed Cancel.
    {
      neui_file_dialog_t d = {};
      d.filters = filters; d.filter_count = 2;

      reset_capture();
      check_eq_int(notify->open_file(sess, { UINT32_MAX - 1 }, &d,
                                     collect_path, (void*)0x1234), -1,
                   "open_file on a bogus widget id returns -1");
      check_eq_int(g_cb_calls, 0, "...and fires no callback");

      reset_capture();
      check_eq_int(notify->open_file(sess, label, &d, collect_path, (void*)0x1234), -1,
                   "open_file on a non-frame widget (LABEL) returns -1");
      check_eq_int(notify->save_file(sess, label, &d, collect_path, (void*)0x1234), -1,
                   "save_file on a non-frame widget (LABEL) returns -1");
      check_eq_int(g_cb_calls, 0, "...and neither fires a callback");
    }

    // ---- C. cancel returns 0, not -1, and delivers nothing ----------------
    {
      neui_file_dialog_t d = {};
      d.title = "Harness open (will be cancelled)";
      d.initial_dir = kDir;
      d.filters = filters; d.filter_count = 2;

      reset_capture();
      schedule_panel_action(/*ok=*/false);
      int r = notify->open_file(sess, win, &d, collect_path, (void*)0x1234);
      check_eq_int(r, 0, "open_file: a cancelled panel returns 0 (not -1)");
      check_eq_int(g_cb_calls, 0, "open_file: a cancelled panel fires no callback");

      reset_capture();
      schedule_panel_action(/*ok=*/false);
      r = notify->save_file(sess, win, &d, collect_path, (void*)0x1234);
      check_eq_int(r, 0, "save_file: a cancelled panel returns 0");
      check_eq_int(g_cb_calls, 0, "save_file: a cancelled panel fires no callback");
    }

    // ---- D. confirm: one path, from the requested dir + name, extension
    //         completed per the documented rule ----------------------------
    {
      neui_file_dialog_t d = {};
      d.title        = "Harness save (will be confirmed)";
      d.initial_dir  = kDir;
      d.initial_name = "harness-lead";       // deliberately NO extension
      d.filters      = filters; d.filter_count = 2;
      d.default_filter = 0;                   // "Presets" -> ".preset"

      reset_capture();
      schedule_panel_action(/*ok=*/true);
      int r = notify->save_file(sess, win, &d, collect_path, (void*)0x1234);
      check_eq_int(r, 1, "save_file: a confirmed panel returns exactly 1");
      check_eq_int(g_cb_calls, 1, "save_file: the callback fires exactly once");
      check_eq_int((int)g_paths.size(), 1, "save_file: one path was delivered");
      if (g_paths.size() == 1) {
        const std::string& p = g_paths[0];
        // The whole point of asserting on AppKit's own URL: the directory and
        // the typed name both have to survive the round trip.
        check(p.size() > 1 && p[0] == '/', "save_file: the path is absolute");
        // The completion rule: no extension typed -> the active filter's.
        bool has_ext = p.size() > 7 &&
                       p.compare(p.size() - 7, 7, ".preset") == 0;
        check(has_ext, "save_file: the filter extension was appended");
        check_path(p, kDir, "harness-lead.preset",
                   "save_file: the path is initial_dir + name + extension");
      }
    }

    // A name that ALREADY has an extension must be left alone, even though it
    // does not match the active filter - the rule says obey the user.
    {
      neui_file_dialog_t d = {};
      d.title        = "Harness save (explicit extension)";
      d.initial_dir  = kDir;
      d.initial_name = "harness-notes.txt";
      d.filters      = filters; d.filter_count = 2;
      d.default_filter = 0;                   // Presets, deliberately mismatched

      reset_capture();
      schedule_panel_action(/*ok=*/true);
      int r = notify->save_file(sess, win, &d, collect_path, (void*)0x1234);
      check_eq_int(r, 1, "save_file: confirmed with an explicit extension");
      if (g_paths.size() == 1) {
        // NSSavePanel may still offer to add the type's extension; what must
        // NOT happen is our own completion pass turning this into
        // "harness-notes.txt.preset".
        check_path(g_paths[0], kDir, "harness-notes.txt",
                   "save_file: an explicit extension is not double-completed");
      }
    }

    // The other direction, which is what keeps the check above from being
    // vacuous: with the "All files" filter active there is no extension to
    // complete with, so the bare typed name must come back unchanged. If the
    // ".preset" assertion passed here too, it would be testing nothing.
    {
      neui_file_dialog_t d = {};
      d.title        = "Harness save (all-files filter)";
      d.initial_dir  = kDir;
      d.initial_name = "harness-bare";
      d.filters      = filters; d.filter_count = 2;
      d.default_filter = 1;                   // "All files" -> "*"

      reset_capture();
      schedule_panel_action(/*ok=*/true);
      int r = notify->save_file(sess, win, &d, collect_path, (void*)0x1234);
      check_eq_int(r, 1, "save_file: confirmed with an all-files filter");
      if (g_paths.size() == 1)
        check_path(g_paths[0], kDir, "harness-bare",
                   "save_file: an all-files filter appends no extension");
    }

    // ---- D2. open_file's SUCCESS path ------------------------------------
    //
    // Everything above confirms only SAVE panels, which left the whole open
    // success path - the `[panel URLs]` collection loop, the N-path callback
    // fan-out - never executed on any platform. A folder picker is the way in:
    // with canChooseDirectories the panel reports its current directory in
    // URLs, so ending the session with OK yields a real non-empty result
    // through the open code path rather than the save one.
    {
      neui_file_dialog_t d = {};
      d.title       = "Harness folder pick";
      d.initial_dir = kDir;
      d.flags       = NEUI_FD_DIRECTORY;

      reset_capture();
      schedule_panel_action(/*ok=*/true);
      int r = notify->open_file(sess, win, &d, collect_path, (void*)0x1234);
      check_eq_int(r, 1, "open_file: a confirmed folder pick returns 1");
      check_eq_int(g_cb_calls, 1, "open_file: the callback fires once per path");
      if (g_paths.size() == 1) {
        // Deliberately NOT asserted: that the path equals initial_dir. Ending
        // the modal session externally leaves the panel's SELECTION at whatever
        // it happened to be, and with canChooseDirectories that is what URLs
        // reports - observed as both "/private/tmp" and its parent "/private"
        // across runs. An equality check here passes or fails on the panel's
        // internal state, not on our code, so it would be a flaky test rather
        // than a strong one. What IS ours, and is asserted: a real absolute
        // path came out of [panel URLs], through the UTF-8 conversion, into
        // exactly one callback.
        struct stat st;
        bool is_dir = (stat(g_paths[0].c_str(), &st) == 0) && S_ISDIR(st.st_mode);
        check(!g_paths[0].empty() && g_paths[0][0] == '/',
              "open_file: the delivered path is absolute");
        check(is_dir, "open_file: the delivered path is an existing directory");
        // A folder pick must not have the save path's extension completion
        // applied to it - filters are ignored in directory mode.
        check(g_paths[0].find(".preset") == std::string::npos,
              "open_file: no extension is appended to a directory");
      }
    }

    // ---- E. descriptor edge cases ----------------------------------------
    {
      // A NULL descriptor is legal: it means "every default". It must reach a
      // real panel (cancelled here), not be rejected as a bad argument.
      reset_capture();
      schedule_panel_action(/*ok=*/false);
      check_eq_int(notify->open_file(sess, win, nullptr, collect_path, (void*)0x1234), 0,
                   "open_file: a NULL descriptor runs a default panel");

      // An out-of-range default_filter must clamp, not index off the end.
      neui_file_dialog_t d = {};
      d.initial_dir  = kDir;
      d.initial_name = "clamped";
      d.filters = filters; d.filter_count = 2;
      d.default_filter = 99;

      reset_capture();
      schedule_panel_action(/*ok=*/true);
      int r = notify->save_file(sess, win, &d, collect_path, (void*)0x1234);
      check_eq_int(r, 1, "save_file: an out-of-range default_filter still runs");
      if (g_paths.size() == 1)
        check_path(g_paths[0], kDir, "clamped.preset",
                   "save_file: default_filter 99 clamped to filter 0");

      // A NULL callback is documented as legal: the dialog still runs and the
      // count is still the answer. This is the "does this host have a file
      // dialog?" probe, and it must not crash.
      reset_capture();
      schedule_panel_action(/*ok=*/false);
      check_eq_int(notify->open_file(sess, win, &d, nullptr, nullptr), 0,
                   "open_file: a NULL callback does not crash and still returns");
    }

    neui->destroy(sess);

    if (g_failures) std::printf("\nFILE DIALOG FAILED (%d)\n", g_failures);
    else            std::printf("\nFILE DIALOG OK\n");
    return g_failures ? 1 : 0;
  }
}

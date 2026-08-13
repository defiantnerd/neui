// Modal-dialog LIFETIME harness - macOS, BOTH hosts.
//
// WHAT THIS FILE IS FOR. A blocking modal show polls a flag for as long as the
// dialog is up, while the documented way OUT of a modal dialog is for a client
// callback to destroy the dialog WIDGET. Those two facts collide: the destroy
// frees the WidgetData the pump is polling. Every host passed an INTERIOR
// pointer (`&w.modal_pump_active`) into its pump, so the loop's exit condition
// was read from freed memory. tests/focus_smoke_win32.cpp caught it in the xpl
// host; this file is the macOS counterpart and covers the NATIVE host too, which
// had the identical defect in hosts/macos/window.mm.
//
// WHY A NORMAL RUN CANNOT SEE THIS, and what this harness does about it. Freed
// memory usually still holds the value that was written to it, so the buggy code
// passes: the destroy path writes `false` into the byte, the allocator frees it,
// and the pump reads back a `false` that is no longer owned by anybody. It is
// undefined behaviour that happens to work.
//
// So the harness re-execs itself under GUARD MALLOC
// (DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib), which places each block on
// its own page and UNMAPS it on free. A read of freed memory then faults
// immediately and cannot be papered over.
//
// MallocScribble (fill-on-free with 0x55) was tried first and is NOT sufficient
// here - worth recording, because it looks sufficient. It only guarantees the
// poison at the moment of free: this block is a few hundred bytes in a process
// where AppKit allocates constantly, so it is reused within microseconds and the
// new occupant had written a zero into that byte by the time the pump next
// polled. The buggy code passed every phase under MallocScribble. An unmapped
// page is the only detector that reuse cannot defeat.
//
// Mutation-verified, and this is the evidence: with the fix reverted, the FIRST
// phase faults under Guard Malloc at modal_pump_macos.h:35 `while (*keep_running)`
// with the freed pointer as its argument, called from widget_show - the defect
// named exactly. With the fix in place the same phase passes.
//
// A WATCHDOG thread also runs, because the other way a broken pump fails is by
// never coming back at all, and a hang with no output is the least useful
// possible test result. It names the phase it died in.
//
// A note on what is NOT asserted: nothing here checks a pointer value or reaches
// into host internals. Each phase asserts only that a blocking `show()` RETURNED
// and that the session is still usable afterwards - observable client behaviour,
// which is what a client actually depends on.
//
// Realizes real NSWindows and blocks in real nested NSEvent pumps, so this is
// built but NOT ctest-registered; run ./tests/<config>/neui_modal_smoke_macos.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  std::fflush(stdout);
  if (!ok) ++g_failures;
}

// ---- watchdog --------------------------------------------------------------
//
// The failure mode under test is a pump that never unwinds, i.e. a hang on the
// MAIN thread inside show(). Only another thread can report it, and it cannot
// unwind the main thread safely - so it prints and _exits. The phase label is
// the payload: "which modal route stopped coming back" is the entire diagnosis.

std::atomic<const char*> g_phase{"startup"};
std::atomic<long long>   g_deadline_ms{0};
std::atomic<bool>        g_done{false};

long long now_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now().time_since_epoch()).count();
}

void enter_phase(const char* label, int budget_ms)
{
  g_phase.store(label);
  g_deadline_ms.store(now_ms() + budget_ms);
}

void leave_phase() { g_deadline_ms.store(0); }

// Assert a modal show() ACTUALLY BLOCKED, and for how long.
//
// This exists because "show() returned" is vacuous on its own: a host that never
// entered its pump at all also returns, and then every phase in this file passes
// while testing nothing. That is not hypothetical - it is what the native host
// did here, and it is why a run with the fix REVERTED still showed green ticks.
// The provocation always arrives on a timer, so a genuine modal show cannot come
// back faster than that delay.
void check_blocked(long long elapsed_ms, long long min_ms, const char* host_label,
                   const char* phase)
{
  char what[220];
  std::snprintf(what, sizeof(what),
                "%s %s: show() BLOCKED in a real modal pump (%lldms)",
                host_label, phase, elapsed_ms);
  if (elapsed_ms < min_ms)
    std::printf("        [diag] show() came back in %lldms, before the %lldms\n"
                "               provocation - so it never entered a modal pump\n"
                "               and everything else this phase asserts is vacuous.\n",
                elapsed_ms, min_ms);
  check(elapsed_ms >= min_ms, what);
}

void start_watchdog()
{
  std::thread([]{
    while (!g_done.load()) {
      const long long d = g_deadline_ms.load();
      if (d != 0 && now_ms() > d) {
        std::printf("[FAIL]  WATCHDOG: stuck in phase \"%s\"\n", g_phase.load());
        std::printf("        A modal pump did not unwind. That is this file's\n"
                    "        defect: the pump is polling a flag inside a\n"
                    "        WidgetData the client already destroyed, and with\n"
                    "        the flag it polls is gone.\n"
                    "        Expected when the modal_pump_flag fix is reverted.\n");
        std::fflush(stdout);
        _exit(1);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }).detach();
}

// ---- host plumbing ---------------------------------------------------------

neui_api_t*        g_neui    = nullptr;
neui_widget_api_t* g_w       = nullptr;
neui_session_t     g_sess    = { 0 };

// Widgets the event handler needs to recognise, plus what it should do.
struct {
  neui_widget_t win{};
  neui_widget_t dlg{};
  // When set, the next KEY event destroys the dialog. This is the RE-ENTRANT
  // route: the destroy arrives from a client callback the host dispatched from
  // inside its own event handling, deeper in the stack than a timer.
  bool destroy_dialog_on_key = false;
  int  key_events   = 0;
  int  focus_events = 0;
} g;

bool NEUI_ABI on_event(void* /*token*/, neui_event_t* ev)
{
  // A KEY event, not a focus event. Focus was the obvious choice and it is the
  // wrong one: the NATIVE host dispatches no client callback at all while a
  // modal dialog is up (focus there is AppKit's business), so a focus-driven
  // phase silently tested nothing on the very host this file was written for -
  // it reported "0 focus events" and then hung for an unrelated reason. Key
  // events are dispatched by BOTH hosts from inside their own event handling,
  // which is exactly the "destroy from deep inside the pump's call stack" shape
  // under test.
  if (ev->type == NEUI_EVENT_KEYDOWN || ev->type == NEUI_EVENT_KEYCHAR) {
    ++g.key_events;
    if (g.destroy_dialog_on_key && g.dlg.id != 0) {
      g.destroy_dialog_on_key = false;
      // The client's ordinary response: "that key means we are done with the
      // dialog, get rid of it" - running from inside the host's dispatch.
      neui_widget_t d = g.dlg;
      g.dlg.id = 0;
      g_w->destroy(g_sess, d);
    }
  }
  if (ev->type == NEUI_EVENT_WIDGET_FOCUS) ++g.focus_events;
  return false;   // never claim the event - the built-in handling must still run
}

neui_widget_client_t g_widget_client = { 1, nullptr, on_event };

void* NEUI_ABI get_iface(void* /*token*/, const char* iface)
{
  if (iface && !std::strcmp(iface, NEUI_API_WIDGETS)) return &g_widget_client;
  return nullptr;
}

// Type one character through the production path (-keyDown:), so the client
// callback is reached the way a real keystroke reaches it rather than by calling
// the host's dispatch directly. Same helper as focus_smoke_macos.mm.
void type_char(NSWindow* win, NSString* ch)
{
  if (!win) return;
  NSEvent* ev = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                 location:NSZeroPoint
                            modifierFlags:0
                                timestamp:0
                             windowNumber:[win windowNumber]
                                  context:nil
                               characters:ch
              charactersIgnoringModifiers:ch
                                isARepeat:NO
                                  keyCode:0];
  // -sendEvent:, not -keyDown: on the content view. The two hosts put different
  // things in the responder chain - the xpl host paints into one NEUIView that
  // handles keys itself, while the native host's dialog content view does not
  // handle them at all and a real keystroke reaches the focused NSTextField
  // through the responder chain. Only sendEvent: reproduces both, and going
  // straight to the content view is why this phase saw zero key events on the
  // native host.
  [win sendEvent:ev];
}

// ---- phases ----------------------------------------------------------------

// Build a dialog owned by the main window, with one focusable control so the
// host's dialog-focus path runs (a dialog with no tab stop takes a different
// branch, which is focus_smoke_macos's business, not this file's).
neui_widget_t make_dialog(const char* title)
{
  neui_widget_t d = g_w->create(g_sess, widget_none, NEUI_W_DIALOG,
                                220, 220, 240, 120, nullptr);
  g_w->set_text(g_sess, d, title);
  g_w->create(g_sess, d, NEUI_W_INPUTBOX, 12, 12, 140, 22, nullptr);
  g_w->set_owner(g_sess, d, g.win);
  return d;
}

// 1. THE ORDINARY ROUTE. A client callback destroys the dialog while the pump is
//    up. This is the documented way a modal show ends, so it is also the route
//    that made the defect reachable from the most ordinary client code there is.
void phase_client_destroy(const char* host_label)
{
  char label[128];
  std::snprintf(label, sizeof(label), "%s: client destroy from inside the pump",
                host_label);
  enter_phase(label, 8000);

  g.dlg = make_dialog("modal 1");
  neui_widget_t dlg = g.dlg;
  [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:NO block:^(NSTimer*){
    g.dlg.id = 0;
    g_w->destroy(g_sess, dlg);     // frees the slot the pump is polling
  }];
  const long long t0 = now_ms();
  g_w->show(g_sess, dlg);          // must RETURN
  const long long elapsed = now_ms() - t0;
  leave_phase();
  check_blocked(elapsed, 200, host_label, "client destroy");

  char what[160];
  std::snprintf(what, sizeof(what),
                "%s show() returned after a client destroy ended the modal",
                host_label);
  check(true, what);
}

// 2. THE USER ROUTE. The window's own close button, with no client destroy at
//    all. The flag is cleared by windowWillClose: on a WidgetData that is still
//    alive, so this route works either way - it is here to prove the fix did not
//    break the path that was already fine.
void phase_user_close(const char* host_label)
{
  char label[128];
  std::snprintf(label, sizeof(label), "%s: user closes the dialog window",
                host_label);
  enter_phase(label, 8000);

  g.dlg = make_dialog("modal 2");
  neui_widget_t dlg = g.dlg;
  const long long t0 = now_ms();
  [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:NO block:^(NSTimer*){
    // -close, not -performClose:. performClose: is the close-BUTTON action and a
    // dialog presented as a sheet has no close button, so it is refused and the
    // pump legitimately never unwinds - a harness bug that looks exactly like the
    // defect under test. -close always fires windowWillClose:.
    for (NSWindow* w in [NSApp windows])
      if ([[w title] isEqualToString:@"modal 2"]) { [w close]; break; }
  }];
  g_w->show(g_sess, dlg);
  const long long elapsed = now_ms() - t0;
  leave_phase();
  check_blocked(elapsed, 200, host_label, "user close");

  char what[160];
  std::snprintf(what, sizeof(what),
                "%s show() returned after the USER closed the dialog",
                host_label);
  check(true, what);
  // The widget outlives the window on this route, so the client still owns it.
  if (g.dlg.id != 0) { g_w->destroy(g_sess, g.dlg); g.dlg.id = 0; }
}

// 3. THE RE-ENTRANT ROUTE - the one that hung outright on win32 rather than
//    surviving by luck. The destroy does not come from a timer at top level; it
//    comes from a client callback the HOST dispatched during the dialog's own
//    focus handling. So the slot is freed from further inside the call stack the
//    pump is running.
void phase_reentrant_destroy(const char* host_label)
{
  char label[160];
  std::snprintf(label, sizeof(label),
                "%s: destroy from inside a host-dispatched focus callback",
                host_label);
  enter_phase(label, 8000);

  g.key_events = 0;
  g.dlg = make_dialog("modal 3");
  neui_widget_t dlg = g.dlg;
  __block bool via_callback = false;
  // Arm from a timer rather than before show(): the dialog's window has to exist
  // before a keystroke can be aimed at it.
  [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:NO block:^(NSTimer*){
    g.destroy_dialog_on_key = true;
    // Name every window as seen from here. "The dialog's window was not up at
    // all" and "the keystroke reached nobody" are different failures with the
    // same symptom, and guessing between them already cost one misdiagnosis.
    std::printf("        [diag] windows now:");
    for (NSWindow* w in [NSApp windows])
      std::printf(" \"%s\"%s", [[w title] UTF8String], [w isVisible] ? "(vis)" : "");
    std::printf("\n");
    std::fflush(stdout);
    // Type into the dialog through the production path. -sendEvent: rather than
    // -keyDown: on the content view: the native host's dialog content view does
    // not handle keys itself, so a real keystroke reaches the focused control
    // through the window's responder chain, and only sendEvent: reproduces that.
    for (NSWindow* w in [NSApp windows])
      if ([[w title] isEqualToString:@"modal 3"]) { type_char(w, @"x"); break; }
    via_callback = (g.dlg.id == 0);
    // Report the route BEFORE anything can hang. Without this a stall is
    // ambiguous: "the pump did not unwind" and "no key event ever reached the
    // client, so nothing destroyed the dialog" look identical from outside, and
    // only one of them is a product defect. That ambiguity already cost one
    // wrong diagnosis on this phase.
    std::printf("        [diag] key events seen: %d; destroyed from %s\n",
                g.key_events, via_callback ? "the client callback" : "the fallback");
    std::fflush(stdout);
    // Fall back to a direct destroy so the phase cannot hang for a reason that
    // is not the defect under test.
    if (g.dlg.id != 0) { neui_widget_t d = g.dlg; g.dlg.id = 0; g_w->destroy(g_sess, d); }
  }];
  const long long t0 = now_ms();
  g_w->show(g_sess, dlg);
  const long long elapsed = now_ms() - t0;
  leave_phase();
  check_blocked(elapsed, 200, host_label, "re-entrant destroy");

  char what[200];
  std::snprintf(what, sizeof(what),
                "%s show() returned when the destroy came from inside the "
                "host's own dispatch", host_label);
  check(true, what);
  // Assert the route was REAL rather than the fallback. Without this the phase
  // passes on a host that dispatched nothing, which is how it managed to look
  // like a product defect when it was testing nothing at all.
  std::snprintf(what, sizeof(what),
                "%s that destroy really came from a host-dispatched callback",
                host_label);
  check(via_callback, what);
}

// 4. THE SESSION IS STILL USABLE. The freed slot is genuinely gone and gets
//    recycled - so if anything still held a stale reference to the dialog's
//    WidgetData, the next widget to take that slot is what it would corrupt.
//    Creating, using and destroying a widget afterwards is the cheap check that
//    the teardown left nothing behind.
void phase_slot_reuse(const char* host_label)
{
  char label[128];
  std::snprintf(label, sizeof(label), "%s: the recycled slot is sound",
                host_label);
  enter_phase(label, 5000);

  neui_widget_t reuse = g_w->create(g_sess, g.win, NEUI_W_INPUTBOX,
                                    10, 200, 120, 22, nullptr);
  g_w->show(g_sess, reuse);
  g_w->set_text(g_sess, reuse, "recycled");
  char buf[64] = {0};
  g_w->get_text(g_sess, reuse, buf, (int)sizeof(buf));
  leave_phase();

  char what[160];
  std::snprintf(what, sizeof(what),
                "%s a widget created in the freed dialog's slot behaves normally",
                host_label);
  check(std::strcmp(buf, "recycled") == 0, what);
  g_w->destroy(g_sess, reuse);
}

// Run the phases against one host. `reentrant` gates phase 3 - see the call site.
void run_host(const char* host_id, const char* host_label, bool reentrant)
{
  std::printf("\n--- %s (%s) ---\n", host_label, host_id);
  std::fflush(stdout);

  g_neui = neui_get_api(host_id);
  if (!g_neui) { check(false, "host is registered"); return; }

  static neui_client_t client = { 1, get_iface };
  g_sess = g_neui->create_session(&client, nullptr);
  g_w = (neui_widget_api_t*)g_neui->get_interface(g_sess, NEUI_API_WIDGETS);
  if (!g_w) { check(false, "NEUI_API_WIDGETS is available"); return; }

  g.win = g_w->create(g_sess, widget_none, NEUI_W_APPWINDOW, 80, 80, 420, 320,
                      nullptr);
  g_w->set_text(g_sess, g.win, host_label);
  g_w->show(g_sess, g.win);

  phase_client_destroy(host_label);
  phase_user_close(host_label);
  if (reentrant) {
    phase_reentrant_destroy(host_label);
  } else {
    // NOT skipped to keep the run green - skipped because the route does not
    // exist here, and saying so is more honest than a phase that passes by
    // reaching its own fallback. Two independent reasons, both measured:
    //   * the native host dispatches NO client callback while a modal dialog is
    //     up (0 key events, whether the event is sent to the content view or
    //     through the window's responder chain), so "destroy from inside the
    //     host's dispatch" has no way to happen;
    //   * it would need a THIRD sequential modal dialog, and on this host the
    //     third one never unwinds - a separate PRE-EXISTING defect, confirmed
    //     against the unfixed code and recorded in docs/deferred-issues.md.
    //     Run it deliberately with the `native-known-defect` argument.
    std::printf("[skip]  %s re-entrant destroy: this host dispatches no client\n"
                "        callback while modal, so the route does not exist (see\n"
                "        the comment at this skip, and deferred-issues.md)\n",
                host_label);
  }
  phase_slot_reuse(host_label);

  g_w->destroy(g_sess, g.win);
  g_neui->destroy(g_sess);
  g_sess.session = 0;
}

// Run one host in a CHILD PROCESS and return its exit status.
//
// WHY THE HOSTS CANNOT SHARE A PROCESS, which is worth knowing because it is a
// real limitation and not a testing preference. Running the xpl host and then
// the native host in one process SEGFAULTS in -[NEUIView drawRect:] - the xpl
// host's view, painting during a CoreAnimation flush AFTER its session was
// destroyed. drawRect: does guard `if (!session) return`, but that only covers
// "not assigned yet": nothing NILS the back-pointer when the session goes away,
// so a view that outlives its session by even one display pass dereferences
// freed memory. MallocScribble turns that from "usually works" into a reliable
// crash, which is how it surfaced here.
//
// That is a separate defect from the one this file tests (recorded in
// docs/deferred-issues.md). It matters most in exactly the shape neui targets:
// a plugin closing its editor destroys the session while the DAW keeps running
// and keeps painting. Isolating per process keeps it from masking the modal
// coverage, and the isolation is honest anyway - two hosts sharing one NSApp
// would let a failure in the first change how the second behaves.
int run_host_in_child(const char* self, const char* host_arg)
{
  pid_t pid = fork();
  if (pid == 0) {
    char* argv2[] = { (char*)self, (char*)host_arg, nullptr };
    execv(self, argv2);
    _exit(127);                       // exec failed
  }
  if (pid < 0) {
    std::printf("[FAIL]  fork() failed for host \"%s\"\n", host_arg);
    return 1;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (WIFSIGNALED(status)) {
    std::printf("[FAIL]  host \"%s\" died on signal %d\n",
                host_arg, WTERMSIG(status));
    return 1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

} // namespace

int main(int argc, char** argv)
{
  const char* only = (argc > 1) ? argv[1] : nullptr;
  // Re-exec under Guard Malloc, which unmaps freed pages so a read of freed
  // memory FAULTS instead of returning a plausible value. Without it the defect
  // this file exists for passes every phase (see the header on why
  // MallocScribble is not enough). The allocator reads its environment at
  // process start, hence the exec rather than a setenv.
  if (!getenv("NEUI_MODAL_SMOKE_REEXEC")) {
    setenv("DYLD_INSERT_LIBRARIES", "/usr/lib/libgmalloc.dylib", 1);
    setenv("NEUI_MODAL_SMOKE_REEXEC", "1", 1);
    execv(argv[0], argv);
    // Only reached if exec failed. Say so loudly: a pass WITHOUT Guard Malloc
    // proves almost nothing here, because the buggy code passes too.
    std::printf("[warn]  could not re-exec under Guard Malloc (%s):\n"
                "        freed memory will not fault, so a REVERTED fix may pass\n"
                "        this run. Re-run with\n"
                "        DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib\n",
                std::strerror(errno));
  }

  std::printf("modal lifetime harness: freed pages UNMAPPED by Guard Malloc,\n"
              "so a pump polling a destroyed widget faults instead of passing.\n");
  std::fflush(stdout);

  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  neui_init();
  start_watchdog();

  // ONE HOST PER PROCESS, and this is not tidiness - see the block comment on
  // run_host_in_child. Both hosts had the defect in their own copy of the
  // pattern: the xpl host in widgets.cpp (fixed by the win32 focus harness) and
  // the native host in hosts/macos/window.mm (fixed here).
  if (!only) {
    const int rc_xpl    = run_host_in_child(argv[0], "xpl");
    const int rc_native = run_host_in_child(argv[0], "native");
    const int rc = (rc_xpl != 0 || rc_native != 0) ? 1 : 0;
    std::printf("\n%s\n", rc == 0 ? "MODAL OK (both hosts)"
                                  : "MODAL FAILED (see the host section above)");
    return rc;
  }
  const bool want_xpl    = !std::strcmp(only, "xpl");
  const bool want_native = !std::strcmp(only, "native");
  const bool want_defect = !std::strcmp(only, "native-known-defect");
  if (!want_xpl && !want_native && !want_defect) {
    std::printf("[FAIL]  unknown argument \"%s\" "
                "(expected xpl | native | native-known-defect)\n", only);
    return 1;
  }
  if (want_xpl)    run_host("neui.host.crossplatform", "xpl host", true);
  if (want_native) run_host("neui.host.macos",          "native host", false);
  if (want_defect) {
    // Deliberate reproduction of the pre-existing native-host defect: the THIRD
    // sequential modal dialog never unwinds. Kept runnable so whoever fixes it
    // has a one-command repro, and kept OUT of the default run so it does not
    // mask this file's actual subject. Expect the watchdog to fire.
    std::printf("\n=== KNOWN DEFECT REPRO (expected to FAIL) ===\n"
                "Three sequential modal dialogs on the native macOS host. The\n"
                "third does not unwind. Pre-existing - confirmed against the\n"
                "unfixed host - and unrelated to modal_pump_flag.\n");
    std::fflush(stdout);
    run_host("neui.host.macos", "native host [known defect]", true);
  }

  g_done.store(true);
  std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "MODAL OK" : "MODAL FAILED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}

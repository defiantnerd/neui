// Focus + modal-teardown acceptance harness (xpl host, win32).
//
// WHY THIS FILE EXISTS. Every focus and modal check in the a11y 6.6 wave was
// harnessed on macOS (tests/focus_smoke_macos.mm). That covers the PORTABLE half
// properly - focus_next / focus_leave_subtree / end_modal all live in Session and
// are shared by every host, so proving them once proves them everywhere. What it
// cannot cover is the half each platform writes for itself: the native WINDOW
// TEARDOWN that has to call into that portable half at the right moment, with the
// widget tree still intact.
//
// win32 shipped a defect there. Session::end_modal restores the focus the dialog
// took from its owner, and it does that by dispatching WIDGET_FOCUS to the client
// SYNCHRONOUSLY. The win32 WM_DESTROY handler called it and then kept
// dereferencing the widget - so a client handler that destroyed the dialog from
// that event (the ordinary "the dialog closed, clean it up" reaction) freed the
// tree slot and left the handler writing into freed memory. macOS was clean
// because its teardown happens not to touch the widget after the call, which is
// exactly why ten macOS harnesses could not see it.
//
// So these checks are deliberately NOT a port of the macOS traversal checks. They
// target the win32 teardown, and above all they RUN THE RE-ENTRANCY: they get
// client code executing at the one instant a teardown is mid-flight, on both
// routes out of a modal dialog and on both routes into WM_DESTROY.
//
//   A  USER CLOSE          - a posted WM_CLOSE (what the X button does) unwinds
//                            the blocking modal show and gives focus back to the
//                            owner. Before 6.6 win32 cleared the pump flag and
//                            restored nothing, so focus stayed on a control
//                            inside a closed window - a dead keyboard.
//   B  RE-ENTRANT DESTROY  - the load-bearing check. The client destroys the
//                            dialog widget from inside the focus event that
//                            end_modal dispatches during WM_DESTROY. This is the
//                            write-after-free.
//   C  CLIENT DESTROY      - the other route out of a modal dialog, driven the way
//                            a real client does it: a click on the dialog's button
//                            arrives in the nested pump and the handler destroys
//                            the dialog. Asserts the two routes end the same way.
//   D  OWNER CLOSE         - closing the OWNER while its modal dialog is up. Its
//                            WM_DESTROY re-enters WM_DESTROY for each owned frame
//                            via DestroyWindow, so the owned dialog's end_modal -
//                            and the client handler it dispatches to - runs while
//                            the owner's own teardown is still on the stack
//                            holding a pointer to the owner's widget.
//   E  STALE SAVED FOCUS   - prev_focus is a tree slot and slots are recycled.
//                            Destroying the saved-focus widget while the dialog is
//                            up and letting a new widget take its slot must NOT
//                            hand focus to a control the user never touched.
//
// HOW THE RE-ENTRANCY IS DRIVEN, AND WHY THE ARMING IS FUSSY. A modal show()
// blocks in a nested OS pump by design, so client code can only run from a
// callback that pump dispatches. Two triggers are used:
//
//   - a one-shot hook on WIDGET_FOCUS, for B and D. Focus returning to the owner
//     IS the event end_modal fires from inside WM_DESTROY, so arming on the
//     owner's field puts the client's destroy exactly where the defect lived.
//     It is gated on the close having been posted, and that gate is load-bearing
//     rather than decorative: the owner's field also receives a focus-gained
//     event while the dialog's window is being CREATED, and an ungated hook fires
//     there instead - which crashes in create_native_window, because that
//     function keeps using its WidgetData& across CreateWindowExW's synchronous
//     WM_* callbacks. Worth fixing on its own, but it is not what this file is
//     about, so the gate keeps the two apart.
//
//   - a one-shot hook on MOUSE_BUTTON_DOWN, for C and E, fed by a click posted
//     into the dialog. That is a genuine client-destroy: a button press dispatched
//     by the nested pump, the same way any real dialog decides to close itself.
//
// Both the clicks and the closes are posted from a worker thread, because the
// thread under test is blocked inside the modal show. The modal pump is parked in
// GetMessageW, so a posted message is exactly what wakes it - which is also
// exactly what a real click on the X does.
//
// HOW A STRAY WRITE IS ACTUALLY DETECTED. _CrtCheckMemory() on its own is close
// to useless for this: it only notices a write that lands on the debug heap's own
// bookkeeping, and the write under test lands in the middle of a freed
// WidgetData. Verified, not assumed - with the fix reverted, every check below
// still passed. So the harness runs the debug heap in DELAY-FREE mode
// (_CRTDBG_DELAY_FREE_MEM_DF): freed blocks are retained and filled with 0xDD,
// and _CrtCheckMemory() then verifies that fill is intact. Any write into a freed
// block breaks the pattern and is reported at the next check, wherever in the
// block it landed. That turns the headline defect from "invisible" into a
// deterministic failure of check B.
//
// This is only safe because the modal pump no longer polls its keep-going flag
// through the dialog's WidgetData. It used to
// (platform_run_modal_until(&fw->modal_pump_active)), and that pointer dangles
// the moment a client destroys the dialog from a callback - the supported way to
// dismiss one. Under a 0xDD fill the freed byte reads as `true`, so the pump
// would spin forever and the harness would be reporting its own instrumentation
// rather than the framework. The flag now lives in a shared_ptr that widget_show
// keeps alive across the pump, so delay-free is sound here. Check C is what
// exercises that path; if it ever regresses it fails as a TIMEOUT.
//
// For an independent verdict - or to localize a write this cannot attribute - run
// the same sequence under ASan, which reports at the offending instruction:
//
//   cmake -B out/asan -DCMAKE_CXX_FLAGS=/fsanitize=address \
//                     -DCMAKE_C_FLAGS=/fsanitize=address
//   cmake --build out/asan --config Debug --target neui_focus_smoke_win32
//
// Realizes real HWNDs and blocks in real nested modal pumps, so this is built but
// NOT ctest-registered; run tests/<config>/neui_focus_smoke_win32.exe manually.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <neui/neui.h>

#include <atomic>
#include <chrono>
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

int g_failures = 0;

// ---- session handles the event callback needs ------------------------------
neui_session_t     g_sess{};
neui_widget_api_t* g_w   = nullptr;
neui_api_t*        g_api = nullptr;

// ---- observed focus state --------------------------------------------------
uint32_t g_focused   = 0;    // last WIDGET_FOCUS(focused=true)
uint32_t g_unfocused = 0;    // last WIDGET_FOCUS(focused=false)

// ---- the one-shot re-entrancy hooks ---------------------------------------
// What the client does when the armed event arrives. The point is to run tree
// mutation while a teardown is mid-flight - the only way to reach the defect.
enum class Reentry {
  none,
  destroy_victim,      // B / C / D: destroy g_victim from the armed callback
  recycle_saved_focus  // E: free the saved-focus slot and let a new widget take it
};

uint32_t      g_arm_on_gain  = 0;              // widget id whose focus-GAIN triggers
uint32_t      g_arm_on_click = 0;              // widget id whose CLICK triggers
Reentry       g_gain_action  = Reentry::none;
Reentry       g_click_action = Reentry::none;
neui_widget_t g_victim{};
bool          g_hook_fired   = false;

// The focus hook only counts once the close has been posted. See the header note:
// without this it fires during window CREATION instead of during teardown.
std::atomic<bool> g_close_posted{false};
bool              g_gain_needs_close = false;

// E's material.
neui_widget_t g_saved_focus_widget{};
neui_widget_t g_saved_focus_parent{};
neui_widget_t g_squatter{};

// ---- watchdog -------------------------------------------------------------
// A teardown that fails to clear the pump flag does not fail a check, it HANGS
// the harness in GetMessageW. A hang reports nothing, so every blocking call gets
// a deadline and the watchdog turns an overrun into a diagnosis.
std::atomic<unsigned long long> g_deadline{0};
std::atomic<const char*>        g_phase{"startup"};
std::atomic<bool>               g_finished{false};

void watchdog()
{
  for (;;) {
    if (g_finished.load()) return;
    const unsigned long long d = g_deadline.load();
    if (d != 0 && GetTickCount64() > d) {
      std::fprintf(stderr,
        "\n[FAIL] TIMEOUT in phase: %s\n"
        "       The nested modal pump never unwound. Either the native teardown\n"
        "       did not clear modal_pump_active, or the pump is reading that flag\n"
        "       through a pointer into a WidgetData the client has since freed.\n"
        "       Aborting so the harness reports a failure instead of hanging.\n",
        g_phase.load());
      std::fflush(nullptr);
      std::_Exit(3);
    }
    Sleep(50);
  }
}

// A harness whose whole job is memory-lifetime bugs has to report WHERE it died:
// a bare 0xC0000005 exit code names neither the phase nor the frame. This
// symbolizes the faulting stack from the .pdb, so a crash is a diagnosis rather
// than a starting point.
LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
  std::fprintf(stderr,
               "\n[FAIL] CRASH: exception 0x%08lX at %p during phase: %s\n",
               ep->ExceptionRecord->ExceptionCode,
               ep->ExceptionRecord->ExceptionAddress, g_phase.load());

  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(proc, nullptr, TRUE);

  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset    = ctx.Rip; frame.AddrPC.Mode    = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;

  for (int i = 0; i < 40; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &frame,
                     &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                     nullptr))
      break;
    if (frame.AddrPC.Offset == 0) break;

    char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = MAX_SYM_NAME;
    DWORD64 sym_disp = 0;
    const char* name =
        SymFromAddr(proc, frame.AddrPC.Offset, &sym_disp, sym) ? sym->Name : "???";

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD line_disp = 0;
    if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &line_disp, &line))
      std::fprintf(stderr, "  %2d  %-48s %s:%lu\n", i, name,
                   line.FileName ? line.FileName : "?", line.LineNumber);
    else
      std::fprintf(stderr, "  %2d  %s\n", i, name);
  }
  std::fflush(nullptr);
  return EXCEPTION_EXECUTE_HANDLER;   // terminate rather than hand back
}

void begin_phase(const char* name, int budget_ms)
{
  g_phase.store(name);
  g_deadline.store(GetTickCount64() + (unsigned long long)budget_ms);
}

void end_phase() { g_deadline.store(0); }

// ---- reporting ------------------------------------------------------------
void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void check_focus(neui_widget_t want, const char* what)
{
  const bool ok = (g_focused == want.id);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) {
    std::printf("        want focus on id %u, got %u\n", want.id, g_focused);
    ++g_failures;
  }
}

// The debug heap's own consistency check. See the header note: it catches a stray
// write only when it lands on heap bookkeeping, so it is a cheap tripwire, not a
// proof of absence.
bool heap_intact()
{
#ifdef _DEBUG
  return _CrtCheckMemory() != 0;
#else
  return true;
#endif
}

// ---- the mutation the hooks perform ---------------------------------------
void run_reentry(Reentry action)
{
  if (action == Reentry::none || g_w == nullptr) return;
  g_hook_fired = true;
  switch (action) {
    case Reentry::destroy_victim:
      // The mutation that used to corrupt the teardown: free the very slot the
      // running handler still holds a pointer to.
      g_w->destroy(g_sess, g_victim);
      break;
    case Reentry::recycle_saved_focus:
      g_w->destroy(g_sess, g_saved_focus_widget);
      g_squatter = g_w->create(g_sess, g_saved_focus_parent, NEUI_W_INPUTBOX,
                               12, 90, 160, 22, nullptr);
      break;
    case Reentry::none: break;
  }
}

// ---- client callbacks -----------------------------------------------------
bool onevent(void*, neui_event_t* ev)
{
  switch (ev->type) {
    case NEUI_EVENT_APP_QUIT:
      // WM_CLOSE asks the client for permission before DestroyWindow. Allow it -
      // withholding it would veto the very "user closed the window" route the
      // whole harness is built around.
      return true;

    case NEUI_EVENT_WIDGET_FOCUS:
      if (ev->data.focus.focused) {
        g_focused = ev->data.focus.widget.id;
        if (g_arm_on_gain != 0 && ev->data.focus.widget.id == g_arm_on_gain &&
            (!g_gain_needs_close || g_close_posted.load())) {
          const Reentry action = g_gain_action;
          g_arm_on_gain = 0;                     // one-shot
          g_gain_action = Reentry::none;
          run_reentry(action);
        }
      } else {
        g_unfocused = ev->data.focus.widget.id;
      }
      break;

    case NEUI_EVENT_MOUSE_BUTTON_DOWN:
      if (g_arm_on_click != 0 && ev->data.mouse.widget.id == g_arm_on_click) {
        const Reentry action = g_click_action;
        g_arm_on_click = 0;                      // one-shot
        g_click_action = Reentry::none;
        run_reentry(action);
      }
      break;

    default: break;
  }
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };

void* iface(void*, const char* n)
{
  return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc;
}

neui_client_t g_client = { NEUI_VERSION, iface };

// ---- driving the OS -------------------------------------------------------
// Find a top-level window by caption, polling: the window under test is created
// by the thread that is now blocked inside the modal show, so a worker cannot
// know when it appeared.
HWND await_window(const wchar_t* title, int tries = 300)
{
  for (int i = 0; i < tries; ++i) {
    if (HWND h = FindWindowW(nullptr, title)) return h;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return nullptr;
}

// The USER route in every respect that matters: the same message the X button
// sends, arriving in the queue the nested pump is parked on.
void close_titled_async(const wchar_t* title, int delay_ms)
{
  std::thread([title, delay_ms] {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    if (HWND h = await_window(title)) {
      g_close_posted.store(true);     // opens the focus hook's gate
      PostMessageW(h, WM_CLOSE, 0, 0);
    }
  }).detach();
}

// A click in the CENTRE of a window's client area. Every dialog below carries one
// BUTTON covering nearly its whole client rect, so the centre lands on that
// button whatever the monitor's DPI - and it arrives as a real
// MOUSE_BUTTON_DOWN dispatched by the nested modal pump.
void click_centre_async(const wchar_t* title, int delay_ms)
{
  std::thread([title, delay_ms] {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    if (HWND h = await_window(title)) {
      RECT rc = {};
      GetClientRect(h, &rc);
      const LPARAM pos = MAKELPARAM((rc.right - rc.left) / 2,
                                    (rc.bottom - rc.top) / 2);
      PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, pos);
      PostMessageW(h, WM_LBUTTONUP, 0, pos);
    }
  }).detach();
}

// Type one character at a window through the production path, so "where do
// keystrokes actually LAND" is an observable fact rather than an inference from
// what the framework says about focus.
void type_char(HWND h, char ch)
{
  if (h) PostMessageW(h, WM_CHAR, (WPARAM)(unsigned char)ch, 1);
}

void pump_for(int ms)
{
  const unsigned long long end = GetTickCount64() + (unsigned long long)ms;
  while (GetTickCount64() < end) {
    g_api->pump_once(g_sess);
    Sleep(5);
  }
}

bool window_titled_exists(const wchar_t* title)
{
  return FindWindowW(nullptr, title) != nullptr;
}

// Every dialog in this file has the same shape: one BUTTON filling most of the
// client area, so it is both a tab stop (focus_next has somewhere to go) and a
// reliable click target at any DPI.
neui_widget_t make_dialog(neui_widget_t owner, const char* title,
                          int x, int y, neui_widget_t* out_button)
{
  neui_widget_t dlg = g_w->create(g_sess, widget_none, NEUI_W_DIALOG,
                                  x, y, 220, 110, nullptr);
  g_w->set_text(g_sess, dlg, title);
  neui_widget_t btn = g_w->create(g_sess, dlg, NEUI_W_BUTTON,
                                  6, 6, 208, 98, nullptr);
  g_w->set_text(g_sess, btn, "close me");
  g_w->set_owner(g_sess, dlg, owner);
  if (out_button) *out_button = btn;
  return dlg;
}

} // namespace

int main()
{
  // Unbuffered: this harness can crash or be aborted by its own watchdog, and a
  // buffered stdout would take the progress log - the only thing that says WHICH
  // check was running - down with it.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // Retain freed blocks filled with 0xDD so heap_intact() can prove the fill is
  // untouched. This is what makes a write into a freed WidgetData observable at
  // all - see the header note, including why it is only sound now that the modal
  // pump no longer polls a flag inside one.
#ifdef _DEBUG
  _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_ALLOC_MEM_DF |
                 _CRTDBG_DELAY_FREE_MEM_DF);
#endif

  // Keep the debug heap's assertions on stderr: a modal assert dialog would block
  // a harness that is supposed to run unattended.
#ifdef _DEBUG
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif

  SetUnhandledExceptionFilter(crash_filter);
  std::thread(watchdog).detach();

  neui_init();
  g_api = neui_get_api("neui.host.crossplatform");
  if (!g_api) { std::printf("FAIL: no crossplatform host\n"); return 1; }

  void* app = nullptr;
  g_sess = g_api->create_session(&g_client, &app);
  auto* w = (neui_widget_api_t*)g_api->get_interface(g_sess, NEUI_API_WIDGETS);
  g_w = w;
  if (!w) { std::printf("FAIL: no widgets interface\n"); return 1; }

  const neui_session_t sess = g_sess;

  // ---- the long-lived owner ------------------------------------------------
  // Exactly one APPWINDOW must outlive every check: the last one closing posts
  // WM_QUIT, and a queued WM_QUIT makes every later nested pump return
  // immediately, which would turn the remaining checks into vacuous passes.
  neui_widget_t owner = w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                  80, 80, 340, 220, nullptr);
  w->set_text(sess, owner, "neui focus owner A");
  neui_widget_t field = w->create(sess, owner, NEUI_W_INPUTBOX,
                                  12, 12, 160, 22, nullptr);
  neui_widget_t other = w->create(sess, owner, NEUI_W_BUTTON,
                                  12, 48, 160, 26, nullptr);
  w->set_text(sess, other, "owner button");
  w->show(sess, owner);
  pump_for(200);

  HWND owner_hwnd = FindWindowW(nullptr, L"neui focus owner A");
  check(owner_hwnd != nullptr, "setup: the owner frame realized as a real HWND");

  // ---- A. the USER closing a modal dialog ---------------------------------
  {
    w->set_focus(sess, field);
    check_focus(field, "A the owner's field holds focus before the dialog");

    neui_widget_t dlg_btn{};
    neui_widget_t dlg = make_dialog(owner, "neui dlg A", 200, 340, &dlg_btn);

    g_focused = 0; g_unfocused = 0;
    g_close_posted.store(false);
    close_titled_async(L"neui dlg A", 250);

    // Reaching the line after show() at all is the first assertion: a teardown
    // that never clears the pump flag hangs here, and the watchdog reports it.
    begin_phase("A: modal show, user close", 8000);
    w->show(sess, dlg);
    end_phase();
    check(true, "A a posted WM_CLOSE unwound the blocking modal show");

    pump_for(150);
    check_focus(field, "A the user close restored the OWNER's focus");
    check(heap_intact(), "A heap intact after the user-close teardown");
    w->destroy(sess, dlg);
    pump_for(100);
  }

  // ---- B. the client destroying the dialog from the teardown's own event ---
  // THE write-after-free. end_modal restores the owner's focus from inside
  // WM_DESTROY; this client reacts to that focus event by destroying the dialog
  // widget, freeing the slot the handler is still holding.
  {
    w->set_focus(sess, field);
    neui_widget_t dlg_btn{};
    neui_widget_t dlg = make_dialog(owner, "neui dlg B", 240, 340, &dlg_btn);

    g_focused = 0; g_unfocused = 0;
    g_hook_fired = false;
    g_victim     = dlg;
    g_close_posted.store(false);
    g_arm_on_gain      = field.id;          // fires from inside WM_DESTROY
    g_gain_action      = Reentry::destroy_victim;
    g_gain_needs_close = true;              // ...and only then, see header
    close_titled_async(L"neui dlg B", 250);

    begin_phase("B: modal show, re-entrant destroy from the teardown's focus event",
                8000);
    w->show(sess, dlg);
    end_phase();

    check(g_hook_fired,
          "B the client's focus handler ran during the dialog's WM_DESTROY");
    check(true, "B ...and destroying the dialog from it did not crash");
    pump_for(150);
    check(heap_intact(), "B no detectable write into the freed dialog slot");
    check(!window_titled_exists(L"neui dlg B"), "B the dialog window is gone");
    check_focus(field, "B focus still came back to the owner's field");
    g_gain_needs_close = false;
  }

  // ---- C. the CLIENT-destroy route ends the same way ----------------------
  // Driven the way a real dialog closes itself: a click on its button, dispatched
  // by the nested pump, whose handler destroys the dialog. This is also the check
  // that exercises the pump reading modal_pump_active through a pointer into the
  // WidgetData the handler just freed (see the header note) - if that ever starts
  // failing, it fails here as a TIMEOUT rather than a wrong answer.
  {
    w->set_focus(sess, field);
    neui_widget_t dlg_btn{};
    neui_widget_t dlg = make_dialog(owner, "neui dlg C", 280, 340, &dlg_btn);

    g_focused = 0; g_unfocused = 0;
    g_hook_fired   = false;
    g_victim       = dlg;
    g_arm_on_click = dlg_btn.id;
    g_click_action = Reentry::destroy_victim;
    click_centre_async(L"neui dlg C", 300);

    begin_phase("C: modal show, client destroy from a click", 8000);
    w->show(sess, dlg);
    end_phase();

    check(g_hook_fired, "C the client destroyed the dialog from a click handler");
    check(true, "C ...and the modal show returned rather than hanging");
    pump_for(150);
    check(heap_intact(), "C heap intact after the client-destroy teardown");
    check_focus(field,
                "C the client-destroy route also restored the owner's focus");
  }

  // ---- D. closing the OWNER while its modal dialog is up ------------------
  // A second window, because this check destroys the owner it uses. Its
  // WM_DESTROY walks its owned frames and DestroyWindow()s each, which re-enters
  // WM_DESTROY synchronously - so the owned dialog's end_modal, and the client
  // handler it dispatches to, run while the OWNER's own teardown is still on the
  // stack holding a pointer to the owner's widget.
  {
    neui_widget_t owner_d = w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                      460, 80, 300, 160, nullptr);
    w->set_text(sess, owner_d, "neui focus owner D");
    neui_widget_t field_d = w->create(sess, owner_d, NEUI_W_INPUTBOX,
                                      12, 12, 160, 22, nullptr);
    w->show(sess, owner_d);
    pump_for(200);
    check(window_titled_exists(L"neui focus owner D"),
          "D the second owner frame realized");

    w->set_focus(sess, field_d);
    neui_widget_t dlg_btn{};
    neui_widget_t dlg = make_dialog(owner_d, "neui dlg D", 520, 340, &dlg_btn);

    g_hook_fired = false;
    g_victim     = owner_d;                 // destroy the OWNER, mid-teardown
    g_close_posted.store(false);
    g_arm_on_gain      = field_d.id;
    g_gain_action      = Reentry::destroy_victim;
    g_gain_needs_close = true;
    close_titled_async(L"neui focus owner D", 250);   // close the OWNER

    begin_phase("D: owner close with an owned modal dialog up", 8000);
    w->show(sess, dlg);
    end_phase();

    check(g_hook_fired,
          "D the owned dialog's teardown dispatched to the client");
    check(true, "D ...and destroying the OWNER from it did not crash");
    pump_for(150);
    check(heap_intact(), "D heap intact after the owner-close cascade");
    check(!window_titled_exists(L"neui focus owner D"),
          "D the owner window is gone");
    check(!window_titled_exists(L"neui dlg D"),
          "D ...and its owned dialog was auto-closed with it");
    g_gain_needs_close = false;
    w->destroy(sess, dlg);
    pump_for(100);
  }

  // ---- E. the saved focus going stale ------------------------------------
  // prev_focus is a tree slot, and slots are recycled. Destroy the saved-focus
  // widget while the dialog is up, let a new widget take its slot, and a restore
  // by slot alone hands focus to a control the user never touched. The save is
  // stamped with the widget's instance id, so the restore must refuse.
  {
    neui_widget_t victim_field = w->create(sess, owner, NEUI_W_INPUTBOX,
                                           12, 90, 160, 22, nullptr);
    w->set_focus(sess, victim_field);
    check_focus(victim_field, "E the doomed field holds focus before the dialog");

    neui_widget_t dlg_btn{};
    neui_widget_t dlg = make_dialog(owner, "neui dlg E", 320, 340, &dlg_btn);

    g_squatter = neui_widget_t{};
    g_saved_focus_widget = victim_field;
    g_saved_focus_parent = owner;
    g_hook_fired   = false;
    // The click recycles the saved slot while the dialog is up; the close then
    // asks end_modal to restore into it.
    g_arm_on_click = dlg_btn.id;
    g_click_action = Reentry::recycle_saved_focus;
    g_close_posted.store(false);
    click_centre_async(L"neui dlg E", 300);
    close_titled_async(L"neui dlg E", 900);

    g_focused = 0;
    begin_phase("E: modal show, saved-focus slot recycled", 10000);
    w->show(sess, dlg);
    end_phase();
    pump_for(150);

    check(g_hook_fired, "E the saved-focus widget was destroyed while up");
    // The check only means anything if the slot really was reused - the low 16
    // bits of a widget id ARE the tree slot, so assert it rather than assume it.
    check(g_squatter.id != 0 &&
          (g_squatter.id & 0xffff) == (victim_field.id & 0xffff),
          "E the new widget took the saved-focus widget's tree slot");
    check(g_focused != g_squatter.id,
          "E no focus-gained event named the recycled slot");
    // Checking for the absence of an EVENT is not enough on its own: the bug is
    // that _focused_widget silently names the slot, with no event either way. So
    // TYPE - with focus left dangling the character lands in a field the user
    // never selected.
    type_char(owner_hwnd, 'q');
    pump_for(150);
    {
      char buf[64] = {0};
      w->get_text(sess, g_squatter, buf, (int)sizeof(buf));
      check(std::strlen(buf) == 0,
            "E a recycled slot does NOT inherit the saved focus");
    }
    check(heap_intact(), "E heap intact after the stale-slot teardown");
    w->destroy(sess, dlg);
    pump_for(100);
  }

  w->destroy(sess, owner);

  g_finished.store(true);
  end_phase();

  std::printf(g_failures ? "\nFOCUS/WIN32 FAILED (%d)\n" : "\nFOCUS/WIN32 OK\n",
              g_failures);
  return g_failures ? 1 : 0;
}

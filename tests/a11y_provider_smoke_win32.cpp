// win32 UI Automation PROVIDER harness - the first execution of a11y_win32.cpp.
//
// The provider shipped with "SHIPS UNVERIFIED" in its own header: written on a
// machine that could not compile or run it, carried instead by a Tier-1-tested
// mapping table, ~40 static_asserts against the real SDK headers, and a
// stub-header parse check. Its commit message names the risk areas it wanted a
// Windows session to look at. This is that session, and this file is what closes
// the gap the other three safeguards explicitly could not: nothing here trusts
// the provider's own bookkeeping, because every query goes through the real
// UI Automation CLIENT stack, the same way Narrator's would.
//
// WHY A SECOND THREAD, which is the whole shape of this file. The provider
// declares ProviderOptions_UseComThreading precisely so UIA cannot touch the
// widget tree from its own threads: COM marshals every method call onto the
// thread that owns the HWND. A client living on the UI thread would never
// exercise that, so it would test the one arrangement no assistive technology
// ever uses. Here the UI thread pumps messages in its usual STA (the host
// already CoInitializeEx's apartment-threaded for WIC), and the UIA client runs
// on a separate MTA thread - so every check below crosses an apartment boundary
// and is marshalled back, exactly as a real AT's would be. If UseComThreading is
// not sufficient, this is the arrangement that shows it.
//
// The checks, and what each one is actually worth:
//
//   1  ROOT           - ElementFromHandle on the frame's HWND yields a fragment
//                       root at all. Nothing else can be true if this is not.
//   2  TREE + ROLES   - the children UIA sees, their control types and names.
//                       Includes a NEGATIVE: a LABEL consumed as another
//                       control's name must NOT appear as its own element, or a
//                       screen reader reads the same words twice.
//   3  NAME SOURCES   - set_name wins; labelled_by supplies a name from a
//                       separate LABEL widget.
//   4  RANGE VALUE    - a declared real-world range is reported in real units,
//                       not as a 0..1 fraction.
//   5  GEOMETRY       - THE load-bearing check. The element's screen rectangle
//                       is cross-validated by posting a real mouse click at its
//                       reported centre and asserting the PRODUCTION hit-test
//                       reports the same widget. Rect and hit-test derived from
//                       the same cache agree with each other even when both are
//                       wrong; only the input path is an independent witness.
//   6  POINT          - ElementProviderFromPoint returns the innermost element
//                       at that same point.
//   7  NAVIGATE       - the sibling walk (first child, next, previous) agrees
//                       with FindAll, both in membership and order. Called out
//                       as a risk area.
//   8  FOCUS          - GetFocusedElement follows the framework's focus.
//   9  ACTIONS        - Invoke on a button, Toggle on a checkbox, and
//                       RangeValue::SetValue on a slider each reach the client.
//                       SetValue must raise the GESTURE_BEGIN / VALUE_CHANGED /
//                       GESTURE_END triple a drag does, so a DAW records an AT
//                       edit as an automation gesture like any other.
//  10  LIFETIME       - the other named risk area: UIA elements outlive the
//                       provider by design and reach it through a nulled
//                       indirection. An element held across the frame's destroy
//                       must answer "gone" rather than reach into a dead
//                       Session. Runs last, because it destroys the frame.
//
// Realizes a real HWND, spins a real UIA client, and posts real input, so this is
// built but NOT ctest-registered; run
// tests/<config>/neui_a11y_provider_smoke_win32.exe manually. Being a console
// target is deliberate - the checks report on stdout.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <uiautomation.h>

#include <neui/neui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- reporting (both threads print, so serialize) --------------------------
std::mutex       g_out;
std::atomic<int> g_failures{0};

void check(bool ok, const char* what)
{
  std::lock_guard<std::mutex> lock(g_out);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void note(const char* fmt, ...)
{
  std::lock_guard<std::mutex> lock(g_out);
  va_list ap;
  va_start(ap, fmt);
  std::printf("        ");
  std::vprintf(fmt, ap);
  va_end(ap);
  std::printf("\n");
}

// ---- session state --------------------------------------------------------
neui_api_t*        g_api   = nullptr;
neui_widget_api_t* g_w     = nullptr;
neui_session_t     g_sess{};
HWND               g_hwnd  = nullptr;

struct Ids {
  neui_widget_t frame{}, label{}, slider{}, button{}, checkbox{}, edit{}, list{},
                custom{};
} g;

// ---- what the client observed, written on the UI thread -------------------
// Read from the UIA thread only AFTER a marshalled call has returned, so the
// UI-thread work is already complete; atomics for visibility, not for ordering.
std::atomic<int>      g_gesture_begin{0};
std::atomic<int>      g_gesture_end{0};
std::atomic<int>      g_value_changed{0};
std::atomic<float>    g_last_value{-1.0f};
std::atomic<int>      g_checkbox_changed{0};
std::atomic<int>      g_checkbox_state{-1};
std::atomic<uint32_t> g_click_widget{0};
std::atomic<uint32_t> g_mouse_down_widget{0};

// ---- cross-thread handshake ----------------------------------------------
std::atomic<bool> g_uia_done{false};
std::atomic<bool> g_frame_destroyed{false};

// Anything that has to happen ON the UI thread mid-run is REQUESTED rather than
// done from the client thread: the widget API is not thread-safe, and calling it
// from here would be testing an arrangement no AT ever produces.
enum class Req { none = 0, focus_edit, announce, bump_value, destroy_frame };
std::atomic<int>  g_req{(int)Req::none};
std::atomic<bool> g_req_done{false};

bool ui_do(Req r, int timeout_ms = 8000)
{
  g_req_done.store(false);
  g_req.store((int)r);
  for (int i = 0; i * 10 < timeout_ms; ++i) {
    if (g_req_done.load()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// ---- watchdog -------------------------------------------------------------
// A marshalled call that never completes hangs rather than fails - and a hang
// reports nothing. This is also the shape a UseComThreading problem would take,
// so the timeout message says so.
std::atomic<unsigned long long> g_deadline{0};
std::atomic<const char*>        g_phase{"startup"};

void watchdog()
{
  for (;;) {
    if (g_uia_done.load() && g_frame_destroyed.load()) return;
    const unsigned long long d = g_deadline.load();
    if (d != 0 && GetTickCount64() > d) {
      std::fprintf(stderr,
        "\n[FAIL] TIMEOUT in phase: %s\n"
        "       A marshalled UIA call never came back. Either the UI thread\n"
        "       stopped pumping (COM cannot deliver to an STA that is not\n"
        "       dispatching messages) or the provider deadlocked against the\n"
        "       widget tree. Aborting so this reports instead of hanging.\n",
        g_phase.load());
      std::fflush(nullptr);
      std::_Exit(3);
    }
    Sleep(50);
  }
}

void phase(const char* name, int budget_ms)
{
  g_phase.store(name);
  g_deadline.store(GetTickCount64() + (unsigned long long)budget_ms);
}

// ---- client callbacks -----------------------------------------------------
bool onevent(void*, neui_event_t* ev)
{
  switch (ev->type) {
    case NEUI_EVENT_APP_QUIT: return true;
    case NEUI_EVENT_VALUE_CHANGED:
      ++g_value_changed;
      g_last_value.store(ev->data.value.value);
      break;
    case NEUI_EVENT_GESTURE_BEGIN: ++g_gesture_begin; break;
    case NEUI_EVENT_GESTURE_END:   ++g_gesture_end;   break;
    case NEUI_EVENT_CHECKBOX_CHANGED:
      ++g_checkbox_changed;
      g_checkbox_state.store((int)ev->data.checkbox.state);
      break;
    case NEUI_EVENT_MOUSE_BUTTON_CLICK:
      g_click_widget.store(ev->data.mouse.widget.id);
      break;
    case NEUI_EVENT_MOUSE_BUTTON_DOWN:
      g_mouse_down_widget.store(ev->data.mouse.widget.id);
      break;
    default: break;
  }
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

// ---- small COM helpers ----------------------------------------------------
template <class T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::wstring bstr_to_wstring(BSTR b)
{
  std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
  if (b) SysFreeString(b);
  return s;
}

std::string narrow(const std::wstring& w)
{
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                              nullptr, 0, nullptr, nullptr);
  std::string out((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                      out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring name_of(IUIAutomationElement* e)
{
  BSTR b = nullptr;
  if (!e || FAILED(e->get_CurrentName(&b))) return {};
  return bstr_to_wstring(b);
}

CONTROLTYPEID type_of(IUIAutomationElement* e)
{
  CONTROLTYPEID t = 0;
  if (e) e->get_CurrentControlType(&t);
  return t;
}

const char* type_name(CONTROLTYPEID t)
{
  switch (t) {
    case UIA_WindowControlTypeId:      return "Window";
    case UIA_ButtonControlTypeId:      return "Button";
    case UIA_CheckBoxControlTypeId:    return "CheckBox";
    case UIA_EditControlTypeId:        return "Edit";
    case UIA_SliderControlTypeId:      return "Slider";
    case UIA_TextControlTypeId:        return "Text";
    case UIA_ListControlTypeId:        return "List";
    case UIA_ListItemControlTypeId:    return "ListItem";
    case UIA_GroupControlTypeId:       return "Group";
    case UIA_PaneControlTypeId:        return "Pane";
    case UIA_CustomControlTypeId:      return "Custom";
    default:                           return "<other>";
  }
}

// The children UIA reports, in its own order.
std::vector<IUIAutomationElement*> children_of(IUIAutomation* uia,
                                               IUIAutomationElement* parent)
{
  std::vector<IUIAutomationElement*> out;
  IUIAutomationCondition* all = nullptr;
  if (FAILED(uia->CreateTrueCondition(&all)) || !all) return out;
  IUIAutomationElementArray* arr = nullptr;
  if (SUCCEEDED(parent->FindAll(TreeScope_Children, all, &arr)) && arr) {
    int n = 0;
    arr->get_Length(&n);
    for (int i = 0; i < n; ++i) {
      IUIAutomationElement* e = nullptr;
      if (SUCCEEDED(arr->GetElement(i, &e)) && e) out.push_back(e);
    }
  }
  safe_release(arr);
  safe_release(all);
  return out;
}

IUIAutomationElement* find_child_by_type(std::vector<IUIAutomationElement*>& kids,
                                         CONTROLTYPEID want)
{
  for (auto* k : kids) if (type_of(k) == want) return k;
  return nullptr;
}

// Post a real click at a SCREEN point, through the production input path, and
// report which widget the client saw. This is what makes check 5 an independent
// witness rather than the provider agreeing with itself.
uint32_t click_screen_point(POINT screen)
{
  POINT client = screen;
  ScreenToClient(g_hwnd, &client);
  g_mouse_down_widget.store(0);
  const LPARAM pos = MAKELPARAM(client.x, client.y);
  PostMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
  PostMessageW(g_hwnd, WM_LBUTTONUP, 0, pos);
  // The UI thread is pumping; give it a moment to dispatch.
  for (int i = 0; i < 200 && g_mouse_down_widget.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return g_mouse_down_widget.load();
}

// =========================================================================
// The UIA client, on its own MTA thread.
// =========================================================================
void uia_client_thread()
{
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_ok = SUCCEEDED(hr);
  check(com_ok, "0 the client thread joined an MTA apartment");
  if (!com_ok) { g_frame_destroyed.store(true); g_uia_done.store(true); return; }

  IUIAutomation* uia = nullptr;
  phase("CoCreateInstance(CUIAutomation)", 20000);
  hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                        __uuidof(IUIAutomation), (void**)&uia);
  check(SUCCEEDED(hr) && uia != nullptr, "0 the UIA client stack came up");
  if (!uia) { g_frame_destroyed.store(true); g_uia_done.store(true);
              CoUninitialize(); return; }

  // ---- 1. the fragment root ---------------------------------------------
  phase("ElementFromHandle (first WM_GETOBJECT)", 20000);
  IUIAutomationElement* root = nullptr;
  hr = uia->ElementFromHandle(g_hwnd, &root);
  check(SUCCEEDED(hr) && root != nullptr,
        "1 ElementFromHandle returned a provider for the frame's HWND");
  if (!root) { g_frame_destroyed.store(true); g_uia_done.store(true);
               safe_release(uia); CoUninitialize(); return; }

  check(type_of(root) == UIA_WindowControlTypeId,
        "1 the fragment root reports control type Window");
  note("root name = \"%s\", control type = %s",
       narrow(name_of(root)).c_str(), type_name(type_of(root)));

  // ---- 2/3. the tree, its roles and its names ---------------------------
  phase("walk the frame's children", 20000);
  auto kids = children_of(uia, root);
  check(!kids.empty(), "2 the root reports children");
  {
    std::lock_guard<std::mutex> lock(g_out);
    std::printf("        UIA sees %d children:\n", (int)kids.size());
    for (auto* k : kids)
      std::printf("          %-10s \"%s\"\n", type_name(type_of(k)),
                  narrow(name_of(k)).c_str());
  }

  auto* e_button = find_child_by_type(kids, UIA_ButtonControlTypeId);
  auto* e_check  = find_child_by_type(kids, UIA_CheckBoxControlTypeId);
  auto* e_edit   = find_child_by_type(kids, UIA_EditControlTypeId);
  auto* e_slider = find_child_by_type(kids, UIA_SliderControlTypeId);
  auto* e_list   = find_child_by_type(kids, UIA_ListControlTypeId);

  check(e_button != nullptr, "2 the BUTTON maps to control type Button");
  check(e_check  != nullptr, "2 the CHECKBOX maps to control type CheckBox");
  check(e_edit   != nullptr, "2 the INPUTBOX maps to control type Edit");
  check(e_slider != nullptr, "2 the SLIDER maps to control type Slider");
  check(e_list   != nullptr, "2 the LISTBOX maps to control type List");

  // The negative. The LABEL is declared as the slider's labelled_by, so the model
  // consumes it: publishing it too would have a screen reader read "Cutoff" once
  // as the slider's name and again as a standalone text element.
  int text_kids = 0;
  for (auto* k : kids)
    if (type_of(k) == UIA_TextControlTypeId &&
        name_of(k) == L"Cutoff") ++text_kids;
  check(text_kids == 0,
        "2 the consumed LABEL is NOT published as its own element");

  check(e_button && name_of(e_button) == L"Save",
        "3 the button's name comes from its widget text");
  check(e_edit && name_of(e_edit) == L"Preset name",
        "3 set_name overrides the widget's own text");
  check(e_slider && name_of(e_slider) == L"Cutoff",
        "3 labelled_by gives the slider the LABEL's text as its name");

  // ---- 4. the declared real-world range --------------------------------
  phase("RangeValue pattern", 20000);
  IUIAutomationRangeValuePattern* rvp = nullptr;
  if (e_slider)
    e_slider->GetCurrentPatternAs(UIA_RangeValuePatternId,
                                  __uuidof(IUIAutomationRangeValuePattern),
                                  (void**)&rvp);
  check(rvp != nullptr, "4 the slider exposes the RangeValue pattern");
  if (rvp) {
    double lo = -1, hi = -1, val = -1;
    rvp->get_CurrentMinimum(&lo);
    rvp->get_CurrentMaximum(&hi);
    rvp->get_CurrentValue(&val);
    note("range = %.1f .. %.1f, value = %.1f", lo, hi, val);
    check(lo == 20.0 && hi == 20000.0,
          "4 the declared range is reported in real units");
    // Value was set to normalized 0.5 before the client started, so the real
    // value is the midpoint - NOT 0.5, which is what a provider that forgot to
    // map through the range would answer.
    check(val > 9000.0 && val < 11000.0,
          "4 the normalized value is mapped INTO the range, not passed through");
  }

  // ---- 5. geometry, cross-validated against the production hit-test -----
  phase("bounding rectangle vs the real hit-test", 20000);
  if (e_button) {
    RECT r = {};
    if (SUCCEEDED(e_button->get_CurrentBoundingRectangle(&r))) {
      POINT centre = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
      note("button screen rect = (%ld,%ld)-(%ld,%ld), centre = (%ld,%ld)",
           r.left, r.top, r.right, r.bottom, centre.x, centre.y);
      check(r.right > r.left && r.bottom > r.top,
            "5 the button reports a non-degenerate screen rectangle");
      const uint32_t hit = click_screen_point(centre);
      check(hit == g.button.id,
            "5 a real click at that centre hits the BUTTON (rect agrees with "
            "the production hit-test)");
      if (hit != g.button.id)
        note("clicked widget id = %u, expected %u", hit, g.button.id);

      // ---- 6. and the provider's own point lookup agrees --------------
      phase("ElementProviderFromPoint", 20000);
      IUIAutomationElement* at_point = nullptr;
      if (SUCCEEDED(uia->ElementFromPoint(centre, &at_point)) && at_point) {
        BOOL same = FALSE;
        uia->CompareElements(at_point, e_button, &same);
        check(same == TRUE,
              "6 ElementFromPoint at the same centre returns the button");
        if (!same)
          note("got %s \"%s\" instead", type_name(type_of(at_point)),
               narrow(name_of(at_point)).c_str());
      } else {
        check(false, "6 ElementFromPoint returned an element");
      }
      safe_release(at_point);
    } else {
      check(false, "5 get_CurrentBoundingRectangle succeeded");
    }
  }

  // ---- 7. the sibling walk agrees with FindAll -------------------------
  phase("Navigate sibling walk", 20000);
  {
    IUIAutomationTreeWalker* walker = nullptr;
    uia->get_ControlViewWalker(&walker);
    check(walker != nullptr, "7 got the control-view tree walker");
    if (walker) {
      std::vector<IUIAutomationElement*> walked;
      IUIAutomationElement* cur = nullptr;
      walker->GetFirstChildElement(root, &cur);
      while (cur) {
        walked.push_back(cur);
        IUIAutomationElement* next = nullptr;
        walker->GetNextSiblingElement(cur, &next);
        cur = next;
        if (walked.size() > 64) break;   // a cycle must not hang the harness
      }
      check(walked.size() == kids.size(),
            "7 the sibling walk finds the same number of children as FindAll");
      note("FindAll = %d, sibling walk = %d",
           (int)kids.size(), (int)walked.size());
      bool order_ok = walked.size() == kids.size();
      for (size_t i = 0; i < walked.size() && i < kids.size() && order_ok; ++i) {
        BOOL same = FALSE;
        uia->CompareElements(walked[i], kids[i], &same);
        if (!same) order_ok = false;
      }
      check(order_ok, "7 ...and in the same ORDER");
      // Walking back must land on the previous one, not on nothing.
      if (walked.size() >= 2) {
        IUIAutomationElement* prev = nullptr;
        walker->GetPreviousSiblingElement(walked[1], &prev);
        BOOL same = FALSE;
        if (prev) uia->CompareElements(prev, walked[0], &same);
        check(same == TRUE, "7 GetPreviousSibling walks back correctly");
        safe_release(prev);
      }
      for (auto* e : walked) safe_release(e);
      safe_release(walker);
    }
  }

  // ---- 8. focus --------------------------------------------------------
  // Focus has to be re-established here rather than relied on from setup: check 5
  // posted a REAL click on the button, and a click legitimately moves focus
  // there. (That is how this check first "failed" - GetFocusedElement correctly
  // answered Button, and the harness was the thing that was wrong.)
  phase("re-focus the INPUTBOX on the UI thread", 20000);
  check(ui_do(Req::focus_edit), "8 the UI thread moved focus to the INPUTBOX");

  // What the PROVIDER reports is the framework's focus, and that is what an AT
  // reads off an element - so assert it on the element itself. Deliberately not
  // via GetFocusedElement: that is a SYSTEM-wide query answered for whichever
  // window owns the foreground, so under an automated shell (where foreground
  // activation is denied) it correctly answers "Desktop", which says nothing
  // about this provider. That distinction is the whole reason this check is
  // split in two.
  phase("HasKeyboardFocus on the focused element", 20000);
  if (e_edit) {
    BOOL kb = FALSE;
    for (int i = 0; i < 40 && !kb; ++i) {
      e_edit->get_CurrentHasKeyboardFocus(&kb);
      if (!kb) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    check(kb == TRUE,
          "8 the provider reports HasKeyboardFocus on the focused INPUTBOX");
    BOOL kb_other = TRUE;
    if (e_button) e_button->get_CurrentHasKeyboardFocus(&kb_other);
    check(kb_other == FALSE,
          "8 ...and NOT on an unfocused sibling");
  }

  // The system-level query, only meaningful when this process actually owns the
  // foreground. Reported rather than asserted when it does not.
  phase("GetFocusedElement", 20000);
  {
    const bool fg = (GetForegroundWindow() == g_hwnd);
    IUIAutomationElement* focused = nullptr;
    hr = uia->GetFocusedElement(&focused);
    check(SUCCEEDED(hr) && focused != nullptr, "8 GetFocusedElement answered");
    if (focused) {
      BOOL same = FALSE;
      if (e_edit) uia->CompareElements(focused, e_edit, &same);
      if (fg) {
        check(same == TRUE, "8 ...and it is the focused INPUTBOX");
        if (!same)
          note("focused element is %s \"%s\"", type_name(type_of(focused)),
               narrow(name_of(focused)).c_str());
      } else {
        note("SKIPPED the system focus comparison: this process was denied the "
             "foreground, so GetFocusedElement answers for %s \"%s\" instead. "
             "Run from an interactive desktop to exercise it.",
             type_name(type_of(focused)), narrow(name_of(focused)).c_str());
      }
    }
    safe_release(focused);
  }

  // ---- 9. actions reach the client -------------------------------------
  phase("Invoke on the button", 20000);
  if (e_button) {
    IUIAutomationInvokePattern* inv = nullptr;
    e_button->GetCurrentPatternAs(UIA_InvokePatternId,
                                 __uuidof(IUIAutomationInvokePattern),
                                 (void**)&inv);
    check(inv != nullptr, "9 the button exposes the Invoke pattern");
    if (inv) {
      g_click_widget.store(0);
      hr = inv->Invoke();
      check(SUCCEEDED(hr), "9 Invoke() succeeded");
      for (int i = 0; i < 200 && g_click_widget.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      check(g_click_widget.load() == g.button.id,
            "9 ...and the client saw a click on the BUTTON");
      safe_release(inv);
    }
  }

  phase("Toggle on the checkbox", 20000);
  if (e_check) {
    IUIAutomationTogglePattern* tog = nullptr;
    e_check->GetCurrentPatternAs(UIA_TogglePatternId,
                                __uuidof(IUIAutomationTogglePattern),
                                (void**)&tog);
    check(tog != nullptr, "9 the checkbox exposes the Toggle pattern");
    if (tog) {
      ToggleState before = ToggleState_Indeterminate;
      tog->get_CurrentToggleState(&before);
      check(before == ToggleState_Off,
            "9 the checkbox starts as ToggleState_Off");
      g_checkbox_changed.store(0);
      hr = tog->Toggle();
      check(SUCCEEDED(hr), "9 Toggle() succeeded");
      for (int i = 0; i < 200 && g_checkbox_changed.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      check(g_checkbox_changed.load() == 1,
            "9 ...and the client got exactly one CHECKBOX_CHANGED");
      ToggleState after = ToggleState_Off;
      tog->get_CurrentToggleState(&after);
      check(after == ToggleState_On,
            "9 ...and the reported toggle state followed the widget");
      safe_release(tog);
    }
  }

  phase("RangeValue SetValue on the slider", 20000);
  if (rvp) {
    g_gesture_begin.store(0);
    g_gesture_end.store(0);
    g_value_changed.store(0);
    hr = rvp->SetValue(5000.0);
    check(SUCCEEDED(hr), "9 RangeValue SetValue() succeeded");
    for (int i = 0; i < 200 && g_value_changed.load() == 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(g_value_changed.load() == 1,
          "9 ...and it raised exactly one VALUE_CHANGED");
    // The point of routing an AT write through a11y_set_value_user: a DAW has to
    // record it as an automation gesture like any other edit.
    check(g_gesture_begin.load() == 1 && g_gesture_end.load() == 1,
          "9 ...bracketed by one GESTURE_BEGIN and one GESTURE_END");
    const float v = g_last_value.load();
    note("normalized value delivered to the client = %.4f (expected ~0.2492)",
         v);
    check(v > 0.24f && v < 0.26f,
          "9 ...and the real-world 5000 mapped back to the right normalized value");
    double read_back = 0;
    rvp->get_CurrentValue(&read_back);
    check(read_back > 4900.0 && read_back < 5100.0,
          "9 ...and reading it back reports the new value in real units");
  }
  // ---- 11. a programmatic value change + notify reaches the AT ----------
  // The notify path is where review caught a re-broken macOS defect (it rebuilt
  // the whole tree per event). This asserts what actually matters to a screen
  // reader: after the client changes the value and notifies, a fresh UIA read
  // returns the NEW value rather than a stale cached one.
  //
  // Written against NEUI_PARAM_VALUE, not a11y->set_value, and that is the point
  // rather than an accident: for a native SLIDER the value lives in the attribute
  // bag, and a11y->set_value is documented as the seam for a value that lives in
  // the CLIENT's own state and never passes through that bag. Verified here -
  // set_value(0.75) on this slider left the reported value at its attribute
  // value, which is the framework preferring the real one over a declaration.
  // Check 14 covers set_value on a widget that actually needs it.
  phase("programmatic value change + notify, re-read through UIA", 20000);
  if (rvp) {
    check(ui_do(Req::bump_value), "11 the client changed the value and notified");
    double after = 0;
    HRESULT hr_v = rvp->get_CurrentValue(&after);
    note("value after set_float(NEUI_PARAM_VALUE, 0.75) = %.1f (expected ~15005)",
         after);
    check(SUCCEEDED(hr_v) && after > 14000.0 && after < 16000.0,
          "11 the AT re-reads the new value, not a stale cache");
  }
  safe_release(rvp);

  // ---- 14. a11y->set_value on a CLIENT-valued control ------------------
  // The documented purpose of set_value: a CUSTOMDRAW whose value lives entirely
  // in client state, so there is no attribute for the framework to prefer. This
  // is the path check 11 deliberately does not use.
  phase("client-declared value on a CUSTOMDRAW", 20000);
  {
    auto kids2 = children_of(uia, root);
    IUIAutomationElement* e_custom = nullptr;
    for (auto* k : kids2)
      if (name_of(k) == L"Client Gain") { e_custom = k; break; }
    check(e_custom != nullptr,
          "14 the CUSTOMDRAW is published under its declared name");
    if (e_custom) {
      note("declared control type = %s", type_name(type_of(e_custom)));
      IUIAutomationRangeValuePattern* crv = nullptr;
      e_custom->GetCurrentPatternAs(UIA_RangeValuePatternId,
                                    __uuidof(IUIAutomationRangeValuePattern),
                                    (void**)&crv);
      check(crv != nullptr,
            "14 a declared SLIDER role carries the RangeValue pattern");
      if (crv) {
        double lo = 0, hi = 0, v = 0;
        crv->get_CurrentMinimum(&lo);
        crv->get_CurrentMaximum(&hi);
        crv->get_CurrentValue(&v);
        note("client-valued range = %.1f .. %.1f, value = %.2f "
             "(declared 0.6 of -60..0)", lo, hi, v);
        check(lo == -60.0 && hi == 0.0,
              "14 the declared range is reported in real units");
        check(v > -25.0 && v < -23.0,
              "14 a11y->set_value drives the reported value for a "
              "client-valued control");
        safe_release(crv);
      }
    }
    for (auto* k : kids2) safe_release(k);
  }

  // ---- 12. announce(): the UiaRaiseNotificationEvent guard -------------
  // Named as a risk area because the API is SDK-version gated. There is nothing
  // to assert about what a screen reader says, so this asserts what can be
  // asserted: that the call is wired, survives both severities, and does not
  // take the process down on this SDK.
  phase("announce (UiaRaiseNotificationEvent)", 20000);
  check(ui_do(Req::announce),
        "12 announce() completed for both polite and assertive");

  // ---- 13. a point over the frame's own background ---------------------
  // The last named risk: ElementProviderFromPoint returns nullptr for the frame
  // itself, and the open question was whether that is what UIA wants. It is -
  // UIA falls back to the HOST provider for the HWND - but "no child here" must
  // resolve to the window rather than to nothing or a fault.
  phase("ElementFromPoint over empty frame background", 20000);
  {
    RECT wr = {};
    GetWindowRect(g_hwnd, &wr);
    // Bottom-right inset: past every widget laid out above, still inside the
    // client area.
    POINT empty = { wr.right - 20, wr.bottom - 20 };
    IUIAutomationElement* bg = nullptr;
    HRESULT hr_bg = uia->ElementFromPoint(empty, &bg);
    check(SUCCEEDED(hr_bg) && bg != nullptr,
          "13 a point over bare frame background still resolves to an element");
    if (bg) {
      note("background point (%ld,%ld) -> %s \"%s\"", empty.x, empty.y,
           type_name(type_of(bg)), narrow(name_of(bg)).c_str());
      check(type_of(bg) == UIA_WindowControlTypeId ||
            type_of(bg) == UIA_PaneControlTypeId,
            "13 ...and it is the frame/window, not a stray child");
    }
    safe_release(bg);
  }

  // ---- 10. elements must outlive the provider safely -------------------
  // Keep a reference, let the UI thread destroy the frame, then query it. The
  // provider's own commit flagged this: elements reach it through an
  // indirection that gets nulled, and answering "gone" is the contract.
  phase("hold an element across the frame's destroy", 20000);
  IUIAutomationElement* survivor = nullptr;
  if (e_button) { survivor = e_button; survivor->AddRef(); }

  for (auto* k : kids) safe_release(k);
  safe_release(root);

  check(ui_do(Req::destroy_frame, 20000) && g_frame_destroyed.load(),
        "10 the UI thread destroyed the frame");

  if (survivor) {
    // Reaching the next line at all is the assertion that matters most: a
    // provider that reached into the dead Session would fault here rather than
    // answer.
    CONTROLTYPEID t = 0;
    HRESULT hr_t = survivor->get_CurrentControlType(&t);
    BSTR b = nullptr;
    HRESULT hr_n = survivor->get_CurrentName(&b);
    if (b) SysFreeString(b);
    note("post-destroy property reads: control type hr=0x%08lX, name hr=0x%08lX",
         hr_t, hr_n);
    check(true, "10 reading a held element after the destroy did not crash");

    // Property reads alone are a weak witness - UIA can satisfy them from its own
    // side without ever calling us, so an S_OK here proves nothing about the
    // provider. An ACTION cannot be faked: it either reaches the widget tree or
    // it does not. Invoking a button whose frame is gone must fail AND must not
    // deliver a click, or an AT could act on a destroyed window.
    IUIAutomationInvokePattern* dead_inv = nullptr;
    survivor->GetCurrentPatternAs(UIA_InvokePatternId,
                                 __uuidof(IUIAutomationInvokePattern),
                                 (void**)&dead_inv);
    if (dead_inv) {
      g_click_widget.store(0);
      HRESULT hr_i = dead_inv->Invoke();
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      note("post-destroy Invoke hr=0x%08lX, click delivered to widget %u",
           hr_i, g_click_widget.load());
      check(FAILED(hr_i), "10 Invoke on a destroyed frame's element FAILS");
      check(g_click_widget.load() == 0,
            "10 ...and no click reached the client");
      safe_release(dead_inv);
    } else {
      note("post-destroy: the Invoke pattern is no longer offered at all");
      check(true, "10 the dead element stopped offering the Invoke pattern");
    }
    safe_release(survivor);
  }

  // And the HWND itself must no longer resolve to a provider.
  {
    IUIAutomationElement* gone = nullptr;
    HRESULT hr_g = uia->ElementFromHandle(g_hwnd, &gone);
    note("post-destroy ElementFromHandle hr=0x%08lX, element=%s",
         hr_g, gone ? "non-null" : "null");
    check(FAILED(hr_g) || gone == nullptr,
          "10 ElementFromHandle on the destroyed HWND no longer yields a provider");
    safe_release(gone);
  }

  safe_release(uia);
  CoUninitialize();
  g_deadline.store(0);
  g_uia_done.store(true);
}

} // namespace

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // Match the host's apartment: it CoInitializeEx's apartment-threaded for WIC,
  // and the UIA client thread below needs a real STA on this side to marshal to.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  std::thread(watchdog).detach();

  neui_init();
  g_api = neui_get_api("neui.host.crossplatform");
  if (!g_api) { std::printf("FAIL: no crossplatform host\n"); return 1; }

  void* app = nullptr;
  g_sess = g_api->create_session(&g_client, &app);
  g_w = (neui_widget_api_t*)g_api->get_interface(g_sess, NEUI_API_WIDGETS);
  auto* attrs = (neui_attr_api_t*)g_api->get_interface(g_sess, NEUI_API_ATTRS);
  auto* a11y  = (neui_a11y_api_t*)g_api->get_interface(g_sess, NEUI_API_A11Y);
  auto* items = (neui_items_api_t*)g_api->get_interface(g_sess, NEUI_API_ITEMS);
  check(g_w != nullptr && attrs != nullptr, "0 widgets + attrs interfaces present");
  check(a11y != nullptr,
        "0 NEUI_API_A11Y is exposed by the crossplatform host");
  if (!g_w || !attrs || !a11y) { std::printf("\nUIA FAILED (setup)\n"); return 1; }

  // ---- the frame under test ---------------------------------------------
  // Sized from the content with margins, per the house rule.
  g.frame = g_w->create(g_sess, widget_none, NEUI_W_APPWINDOW,
                        100, 100, 440, 260, nullptr);
  g_w->set_text(g_sess, g.frame, "neui uia frame");

  // A LABEL that NAMES the slider rather than standing on its own.
  g.label = g_w->create(g_sess, g.frame, NEUI_W_LABEL, 12, 12, 120, 20, nullptr);
  g_w->set_text(g_sess, g.label, "Cutoff");

  g.slider = g_w->create(g_sess, g.frame, NEUI_W_SLIDER, 12, 36, 200, 24, nullptr);
  attrs->set_float(g_sess, g.slider, NEUI_PARAM_VALUE, 0.5f);
  a11y->set_value_range(g_sess, g.slider, 20.0f, 20000.0f, 1.0f);
  a11y->set_labelled_by(g_sess, g.slider, g.label);

  g.button = g_w->create(g_sess, g.frame, NEUI_W_BUTTON, 12, 74, 120, 28, nullptr);
  g_w->set_text(g_sess, g.button, "Save");

  g.checkbox = g_w->create(g_sess, g.frame, NEUI_W_CHECKBOX, 12, 112, 140, 22, nullptr);
  g_w->set_text(g_sess, g.checkbox, "Bypass");

  g.edit = g_w->create(g_sess, g.frame, NEUI_W_INPUTBOX, 12, 146, 200, 22, nullptr);
  a11y->set_name(g_sess, g.edit, "Preset name");

  // A CUSTOMDRAW whose value lives only in client state - the case a11y->set_value
  // exists for, and the one a native SLIDER is not (check 14).
  g.custom = g_w->create(g_sess, g.frame, NEUI_W_CUSTOMDRAW, 12, 180, 200, 24, nullptr);
  a11y->set_role(g_sess, g.custom, NEUI_A11Y_ROLE_SLIDER);
  a11y->set_name(g_sess, g.custom, "Client Gain");
  a11y->set_value_range(g_sess, g.custom, -60.0f, 0.0f, 0.5f);
  a11y->set_value(g_sess, g.custom, 0.6f);
  note("declared a11y value stored: has=%d, value=%.3f",
       attrs->has(g_sess, g.custom, NEUI_ATTR_A11Y_VALUE),
       attrs->get_float(g_sess, g.custom, NEUI_ATTR_A11Y_VALUE, -1.0f));

  g.list = g_w->create(g_sess, g.frame, NEUI_W_LISTBOX, 240, 36, 180, 132, nullptr);
  if (items) {
    items->add(g_sess, g.list, "Sine",   nullptr);
    items->add(g_sess, g.list, "Square", nullptr);
    items->add(g_sess, g.list, "Saw",    nullptr);
  }

  g_w->show(g_sess, g.frame);

  // Let the frame realize and paint: the adapter's rectangles come out of the
  // layout the paint pass produces, and check 5 is about those rectangles.
  for (int i = 0; i < 80; ++i) { g_api->pump_once(g_sess); Sleep(5); }

  g_hwnd = FindWindowW(nullptr, L"neui uia frame");
  check(g_hwnd != nullptr, "0 the frame realized as a real HWND");
  if (!g_hwnd) { std::printf("\nUIA FAILED (no window)\n"); return 1; }

  // Focus the INPUTBOX before the client starts, so check 8 has an answer.
  g_w->set_focus(g_sess, g.edit);
  for (int i = 0; i < 20; ++i) { g_api->pump_once(g_sess); Sleep(5); }

  // is_active is advisory and answers from UiaClientsAreListening, so it is only
  // meaningful once something is actually listening - checked again below.
  const bool active_before = a11y->is_active(g_sess);
  note("is_active before any UIA client attached = %s",
       active_before ? "true" : "false");

  std::thread client(uia_client_thread);

  // ---- pump until the client is done -------------------------------------
  // This loop is not incidental: ProviderOptions_UseComThreading marshals every
  // provider call onto THIS thread, so the client's queries only complete while
  // this keeps dispatching messages.
  while (!g_uia_done.load()) {
    g_api->pump_once(g_sess);
    const int req = g_req.load();
    if (req != (int)Req::none) {
      switch ((Req)req) {
        case Req::focus_edit:
          // ACTIVATE, then focus. UIA's HasKeyboardFocus is about the real
          // keyboard, so it is only true when the window is actually active -
          // without this the check passes or fails depending on what else has the
          // foreground, which is how it first came out flaky.
          SetForegroundWindow(g_hwnd);
          SetActiveWindow(g_hwnd);
          g_w->set_focus(g_sess, g.edit);
          break;
        case Req::announce:
          a11y->announce(g_sess, "polite announcement", false);
          a11y->announce(g_sess, "assertive announcement", true);
          break;
        case Req::bump_value:
          // The real widget value, then notify - the pairing a client is told to
          // use after changing something an AT may be reading.
          attrs->set_float(g_sess, g.slider, NEUI_PARAM_VALUE, 0.75f);
          a11y->notify(g_sess, g.slider, NEUI_A11Y_CHANGE_VALUE);
          break;
        case Req::destroy_frame:
          g_w->destroy(g_sess, g.frame);
          g_frame_destroyed.store(true);
          break;
        case Req::none: break;
      }
      for (int i = 0; i < 30; ++i) { g_api->pump_once(g_sess); Sleep(5); }
      g_req.store((int)Req::none);
      g_req_done.store(true);
    }
    Sleep(2);
  }
  client.join();

  std::printf(g_failures.load() ? "\nUIA PROVIDER FAILED (%d)\n"
                                : "\nUIA PROVIDER OK\n", g_failures.load());
  return g_failures.load() ? 1 : 0;
}

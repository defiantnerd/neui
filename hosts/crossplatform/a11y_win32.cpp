// win32 UI Automation provider. See a11y_win32.h for what "unverified" means
// here and what stands in for execution.
//
// SHAPE. One provider per frame HWND, holding the node tree the shared adapter
// builds. One COM class for every element, root included: UIA discovers patterns
// through IRawElementProviderSimple::GetPatternProvider rather than through
// QueryInterface, so a single class that implements every pattern and gates
// GetPatternProvider on the node's PatternSet is both simpler and less
// error-prone than a class hierarchy. (QI still succeeds for the interfaces we
// implement - UIA is allowed to ask - but GetPatternProvider is what decides
// what a client is told the element can do.)
//
// EVERYTHING STRUCTURAL IS SHARED WITH macOS. The tree, the revision-based cache,
// the dead-frame/dead-session revalidation, the client-first action dispatch, the
// declared-step handling, the "notify does not rebuild" rule - all of it exists
// because review found each one as a defect in the macOS provider. They are not
// re-derived here; they are the same decisions, so a fix there is a fix here.
//
// COORDINATES. UIA wants PHYSICAL pixels in screen space, top-left origin - which
// is the same orientation as ours, so unlike macOS there is no flip. Nodes are
// frame-local LOGICAL px, so the conversion is one multiply by the frame's
// logical->physical factor (DPI ratio AND user zoom) plus ClientToScreen.

#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "a11y_win32.h"
#include "a11y_adapter.h"
#include "host.h"
#include "platform.h"
#include "../shared/a11y_uia_map.h"

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleaut32.lib")

using neui_detail::A11yNode;
using neui_detail::A11yNodeId;
using neui_detail::A11ySubKind;
using neui_detail::a11y_hit_test;
using neui_detail::a11y_id_equal;
namespace uia = neui_detail::uia;

// ---------------------------------------------------------------------------
// THE CONSTANT AUDIT.
//
// hosts/shared/a11y_uia_map.h duplicates the UIA ids so the mapping tables can
// be Tier-1 tested with no Windows headers. That duplication is only safe if
// something checks it against the real SDK - a unit test can confirm the number
// I wrote down, not that it is the number Windows uses. These do, at compile
// time, so a wrong id is a build break rather than a screen reader quietly
// announcing the wrong control type.

static_assert(uia::kButtonControlType      == UIA_ButtonControlTypeId, "UIA id drift");
static_assert(uia::kCheckBoxControlType    == UIA_CheckBoxControlTypeId, "UIA id drift");
static_assert(uia::kComboBoxControlType    == UIA_ComboBoxControlTypeId, "UIA id drift");
static_assert(uia::kEditControlType        == UIA_EditControlTypeId, "UIA id drift");
static_assert(uia::kImageControlType       == UIA_ImageControlTypeId, "UIA id drift");
static_assert(uia::kListItemControlType    == UIA_ListItemControlTypeId, "UIA id drift");
static_assert(uia::kListControlType        == UIA_ListControlTypeId, "UIA id drift");
static_assert(uia::kMenuControlType        == UIA_MenuControlTypeId, "UIA id drift");
static_assert(uia::kMenuBarControlType     == UIA_MenuBarControlTypeId, "UIA id drift");
static_assert(uia::kMenuItemControlType    == UIA_MenuItemControlTypeId, "UIA id drift");
static_assert(uia::kProgressBarControlType == UIA_ProgressBarControlTypeId, "UIA id drift");
static_assert(uia::kRadioButtonControlType == UIA_RadioButtonControlTypeId, "UIA id drift");
static_assert(uia::kSliderControlType      == UIA_SliderControlTypeId, "UIA id drift");
static_assert(uia::kTabControlType         == UIA_TabControlTypeId, "UIA id drift");
static_assert(uia::kTabItemControlType     == UIA_TabItemControlTypeId, "UIA id drift");
static_assert(uia::kTextControlType        == UIA_TextControlTypeId, "UIA id drift");
static_assert(uia::kTreeControlType        == UIA_TreeControlTypeId, "UIA id drift");
static_assert(uia::kTreeItemControlType    == UIA_TreeItemControlTypeId, "UIA id drift");
static_assert(uia::kCustomControlType      == UIA_CustomControlTypeId, "UIA id drift");
static_assert(uia::kGroupControlType       == UIA_GroupControlTypeId, "UIA id drift");
static_assert(uia::kDataItemControlType    == UIA_DataItemControlTypeId, "UIA id drift");
static_assert(uia::kWindowControlType      == UIA_WindowControlTypeId, "UIA id drift");
static_assert(uia::kPaneControlType        == UIA_PaneControlTypeId, "UIA id drift");
static_assert(uia::kHeaderItemControlType  == UIA_HeaderItemControlTypeId, "UIA id drift");
static_assert(uia::kTableControlType       == UIA_TableControlTypeId, "UIA id drift");
static_assert(uia::kSeparatorControlType   == UIA_SeparatorControlTypeId, "UIA id drift");

static_assert(uia::kInvokePattern         == UIA_InvokePatternId, "UIA id drift");
static_assert(uia::kValuePattern          == UIA_ValuePatternId, "UIA id drift");
static_assert(uia::kRangeValuePattern     == UIA_RangeValuePatternId, "UIA id drift");
static_assert(uia::kExpandCollapsePattern == UIA_ExpandCollapsePatternId, "UIA id drift");
static_assert(uia::kSelectionItemPattern  == UIA_SelectionItemPatternId, "UIA id drift");
static_assert(uia::kTogglePattern         == UIA_TogglePatternId, "UIA id drift");
static_assert(uia::kScrollItemPattern     == UIA_ScrollItemPatternId, "UIA id drift");

static_assert(static_cast<int32_t>(uia::kToggleOff)           == ToggleState_Off, "UIA id drift");
static_assert(static_cast<int32_t>(uia::kToggleOn)            == ToggleState_On, "UIA id drift");
static_assert(static_cast<int32_t>(uia::kToggleIndeterminate) == ToggleState_Indeterminate, "UIA id drift");
static_assert(static_cast<int32_t>(uia::kCollapsed)           == ExpandCollapseState_Collapsed, "UIA id drift");
static_assert(static_cast<int32_t>(uia::kExpanded)            == ExpandCollapseState_Expanded, "UIA id drift");
static_assert(static_cast<int32_t>(uia::kPartiallyExpanded)   == ExpandCollapseState_PartiallyExpanded, "UIA id drift");
static_assert(static_cast<int32_t>(uia::kLeafNode)            == ExpandCollapseState_LeafNode, "UIA id drift");

namespace xpl_host
{
namespace
{

// UTF-8 -> BSTR. Empty / null becomes an empty BSTR rather than null: a null
// BSTR in a VARIANT is legal but several clients treat it as "property not
// supported" instead of "empty", which is a different statement.
BSTR bstr_from_utf8(const char* s)
{
  if (!s) s = "";
  const int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (need <= 0) return SysAllocString(L"");
  std::wstring w(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], need);
  if (!w.empty() && w.back() == L'\0') w.pop_back();
  return SysAllocString(w.c_str());
}

// A provider outlives the Session and the frame it describes - UIA holds element
// references for as long as it likes. Elements therefore reach their provider
// through this indirection, and every provider nulls it in its destructor, so a
// late query answers "gone" instead of walking freed memory. (The macOS provider
// gets the same property from an ARC weak reference.)
class UiaProvider;
struct ProviderLink
{
  UiaProvider* p = nullptr;
};

class UiaElement;

// ---------------------------------------------------------------------------

class UiaProvider
{
public:
  UiaProvider(HWND hwnd, Session* s, uint32_t frame_index);
  ~UiaProvider();

  UiaProvider(const UiaProvider&) = delete;
  UiaProvider& operator=(const UiaProvider&) = delete;

  // Re-establish _session from (session id, frame instance id). Everything the
  // provider answers goes through this first; once dead it stays dead. Same
  // contract as -[NEUIA11yProvider revalidate].
  bool revalidate();
  void refresh();

  const A11yNode* node_for(const A11yNodeId& id);
  // AddRef'd, or null. The element table is keyed by node id, so the SAME node
  // hands back the SAME object across rebuilds - UIA caches element references
  // and compares them.
  UiaElement* element_for(const A11yNodeId& id);
  UiaElement* root_element();

  // Physical screen rect for a node, in UIA's coordinate space.
  bool screen_rect(const A11yNode& nd, UiaRect* out);
  const A11yNode* hit_test_screen(double x, double y);
  const A11yNode* focused_node();

  Session* session() { return _session; }
  uint32_t frame_index() const { return _frame_index; }
  HWND hwnd() const { return _hwnd; }
  bool alive() const { return !_dead && _session != nullptr; }
  std::shared_ptr<ProviderLink> link() { return _link; }

  const std::vector<A11yNode>& nodes() const { return _nodes; }
  A11yNodeId frame_node_id();

  // Actions, all routed through the same paths a user has. See the macOS
  // provider's -sendKey:to: for why the CLIENT gets first refusal.
  bool send_key(uint32_t slot, uint32_t keycode);
  bool press(const A11yNodeId& id);
  bool set_normalized(const A11yNodeId& id, float v);
  bool focus_node(const A11yNodeId& id);
  bool select_item(const A11yNodeId& id);
  bool scroll_into_view(const A11yNodeId& id);
  bool set_expanded(const A11yNodeId& id, bool expand);

private:
  HWND        _hwnd;
  Session*    _session;
  uint32_t    _session_id;
  uint32_t    _frame_index;
  uint32_t    _frame_generation;
  bool        _dead = false;
  std::vector<A11yNode> _nodes;
  std::unordered_map<std::string, UiaElement*> _elements;   // owned (one ref each)
  uint32_t    _built_revision = 0;
  bool        _built = false;
  std::shared_ptr<ProviderLink> _link;
};

std::string key_of(const A11yNodeId& id)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%u.%u.%d.%d", id.widget_id, id.generation,
                static_cast<int>(id.sub_kind), static_cast<int>(id.sub_index));
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// One element. Root and non-root are the same class; `is_root` gates the
// FragmentRoot interface and the host provider.

class UiaElement final : public IRawElementProviderSimple,
                         public IRawElementProviderFragment,
                         public IRawElementProviderFragmentRoot,
                         public IInvokeProvider,
                         public IToggleProvider,
                         public IValueProvider,
                         public IRangeValueProvider,
                         public ISelectionItemProvider,
                         public IExpandCollapseProvider,
                         public IScrollItemProvider
{
public:
  UiaElement(std::shared_ptr<ProviderLink> link, const A11yNodeId& id, bool is_root)
    : _link(std::move(link)), _id(id), _is_root(is_root) {}

  // ---- IUnknown ----------------------------------------------------------
  // Interlocked rather than a plain ++: ProviderOptions_UseComThreading marshals
  // our METHOD CALLS onto the UI thread, but COM itself may AddRef/Release from
  // its own threads while marshalling, so the count has to be atomic.
  IFACEMETHODIMP_(ULONG) AddRef() override
  { return static_cast<ULONG>(InterlockedIncrement(&_ref)); }
  IFACEMETHODIMP_(ULONG) Release() override
  {
    const LONG r = InterlockedDecrement(&_ref);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }
  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
  {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
      *ppv = static_cast<IRawElementProviderSimple*>(this);
    else if (riid == __uuidof(IRawElementProviderFragment))
      *ppv = static_cast<IRawElementProviderFragment*>(this);
    // Only the frame is a fragment ROOT. Answering for a child would make UIA
    // treat that child as the top of its own tree.
    else if (riid == __uuidof(IRawElementProviderFragmentRoot) && _is_root)
      *ppv = static_cast<IRawElementProviderFragmentRoot*>(this);
    else if (riid == __uuidof(IInvokeProvider))
      *ppv = static_cast<IInvokeProvider*>(this);
    else if (riid == __uuidof(IToggleProvider))
      *ppv = static_cast<IToggleProvider*>(this);
    else if (riid == __uuidof(IValueProvider))
      *ppv = static_cast<IValueProvider*>(this);
    else if (riid == __uuidof(IRangeValueProvider))
      *ppv = static_cast<IRangeValueProvider*>(this);
    else if (riid == __uuidof(ISelectionItemProvider))
      *ppv = static_cast<ISelectionItemProvider*>(this);
    else if (riid == __uuidof(IExpandCollapseProvider))
      *ppv = static_cast<IExpandCollapseProvider*>(this);
    else if (riid == __uuidof(IScrollItemProvider))
      *ppv = static_cast<IScrollItemProvider*>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }

  const A11yNodeId& node_id() const { return _id; }

  // ---- IRawElementProviderSimple -----------------------------------------
  IFACEMETHODIMP get_ProviderOptions(ProviderOptions* opts) override
  {
    if (!opts) return E_INVALIDARG;
    // Server-side provider, and UseComThreading so UIA marshals our calls onto
    // the UI thread. Without that, a UIA client thread would touch the widget
    // tree concurrently with the message loop - the tree has no locking and is
    // not meant to.
    *opts = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider |
                                         ProviderOptions_UseComThreading);
    return S_OK;
  }

  IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;
    UiaProvider* p = prov();
    const A11yNode* nd = p ? p->node_for(_id) : nullptr;
    if (!nd) return S_OK;
    if (!uia::supports_pattern(pattern_set(*nd, p), static_cast<int32_t>(pattern)))
      return S_OK;                     // "not supported" is a null return, not an error
    *ret = static_cast<IRawElementProviderSimple*>(this);
    AddRef();
    return S_OK;
  }

  IFACEMETHODIMP GetPropertyValue(PROPERTYID prop, VARIANT* out) override
  {
    if (!out) return E_INVALIDARG;
    VariantInit(out);
    UiaProvider* p = prov();
    const A11yNode* nd = p ? p->node_for(_id) : nullptr;
    if (!nd) return S_OK;              // empty VARIANT = "no value", never a lie

    switch (prop) {
      case UIA_ControlTypePropertyId:
        out->vt = VT_I4;
        out->lVal = uia::control_type_for_role(nd->role);
        break;
      case UIA_NamePropertyId:
        out->vt = VT_BSTR;
        out->bstrVal = bstr_from_utf8(nd->name.c_str());
        break;
      case UIA_HelpTextPropertyId:
        if (!nd->description.empty()) {
          out->vt = VT_BSTR;
          out->bstrVal = bstr_from_utf8(nd->description.c_str());
        }
        break;
      case UIA_IsEnabledPropertyId:
        out->vt = VT_BOOL;
        out->boolVal = uia::is_enabled(nd->state) ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      case UIA_HasKeyboardFocusPropertyId:
        out->vt = VT_BOOL;
        out->boolVal = uia::has_keyboard_focus(nd->state) ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      case UIA_IsKeyboardFocusablePropertyId:
        out->vt = VT_BOOL;
        out->boolVal = uia::is_keyboard_focusable(nd->state) ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      case UIA_IsOffscreenPropertyId:
        out->vt = VT_BOOL;
        out->boolVal = uia::is_offscreen(nd->state) ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      case UIA_IsPasswordPropertyId:
        out->vt = VT_BOOL;
        out->boolVal = uia::is_password(nd->state) ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      // Both true for everything we publish: the model has already pruned what
      // an AT should not see (ROLE_NONE and its subtree), so anything still here
      // is content the user can reach.
      case UIA_IsControlElementPropertyId:
      case UIA_IsContentElementPropertyId:
        out->vt = VT_BOOL;
        out->boolVal = VARIANT_TRUE;
        break;
      default:
        break;                          // leave VT_EMPTY
    }
    return S_OK;
  }

  IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;
    // ONLY the root defers to the HWND provider, and it must: that is what gives
    // the window its frame, its window-pattern behaviour and its place in the
    // desktop tree. A child returning it would graft the whole window under
    // itself.
    UiaProvider* p = prov();
    if (!_is_root || !p) return S_OK;
    return UiaHostProviderFromHwnd(p->hwnd(), ret);
  }

  // ---- IRawElementProviderFragment ---------------------------------------
  IFACEMETHODIMP Navigate(NavigateDirection dir, IRawElementProviderFragment** ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;
    UiaProvider* p = prov();
    if (!p) return S_OK;
    const A11yNode* nd = p->node_for(_id);
    if (!nd) return S_OK;

    const A11yNodeId frame_id = p->frame_node_id();
    UiaElement* target = nullptr;

    if (dir == NavigateDirection_Parent) {
      // The frame node IS the root element here (unlike macOS, where the NSView
      // stands for the frame and the frame node is not published at all) - UIA
      // wants a fragment root that owns the HWND, so the frame has to be a real
      // element. A top-level widget's parent is therefore the root, and the
      // root's parent is nobody (UIA reaches it through the HWND).
      if (_is_root) return S_OK;
      target = p->element_for(neui_detail::a11y_id_null(nd->parent) ? frame_id
                                                                   : nd->parent);
    } else if (dir == NavigateDirection_FirstChild ||
               dir == NavigateDirection_LastChild) {
      const std::vector<A11yNodeId>* kids = &nd->children;
      if (kids->empty()) return S_OK;
      target = p->element_for(dir == NavigateDirection_FirstChild
                                ? kids->front() : kids->back());
    } else {
      // Siblings: find this node in the parent's ordered child list.
      const A11yNodeId parent_id =
        neui_detail::a11y_id_null(nd->parent) ? frame_id : nd->parent;
      const A11yNode* pn = p->node_for(parent_id);
      if (!pn) return S_OK;
      const std::vector<A11yNodeId>& kids = pn->children;
      size_t i = 0;
      bool found = false;
      for (; i < kids.size(); ++i)
        if (a11y_id_equal(kids[i], _id)) { found = true; break; }
      if (!found) return S_OK;
      if (dir == NavigateDirection_NextSibling && i + 1 < kids.size())
        target = p->element_for(kids[i + 1]);
      else if (dir == NavigateDirection_PreviousSibling && i > 0)
        target = p->element_for(kids[i - 1]);
    }

    if (!target) return S_OK;
    // element_for already AddRef'd; hand that reference to the caller.
    *ret = static_cast<IRawElementProviderFragment*>(target);
    return S_OK;
  }

  IFACEMETHODIMP GetRuntimeId(SAFEARRAY** ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;
    // UiaAppendRuntimeId tells UIA to prefix the HWND's own id, which is what
    // makes these unique across processes. The rest is the node id - INCLUDING
    // the per-widget-instance generation, so a recycled tree slot never produces
    // a runtime id that a client would match against the widget that used to
    // live there.
    int ids[5] = { UiaAppendRuntimeId,
                   static_cast<int>(_id.widget_id),
                   static_cast<int>(_id.generation),
                   static_cast<int>(_id.sub_kind),
                   static_cast<int>(_id.sub_index) };
    SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 5);
    if (!sa) return E_OUTOFMEMORY;
    for (LONG i = 0; i < 5; ++i) {
      HRESULT hr = SafeArrayPutElement(sa, &i, &ids[i]);
      if (FAILED(hr)) { SafeArrayDestroy(sa); return hr; }
    }
    *ret = sa;
    return S_OK;
  }

  IFACEMETHODIMP get_BoundingRectangle(UiaRect* ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = UiaRect{ 0, 0, 0, 0 };
    UiaProvider* p = prov();
    const A11yNode* nd = p ? p->node_for(_id) : nullptr;
    if (!nd) return S_OK;
    p->screen_rect(*nd, ret);
    return S_OK;
  }

  IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;                    // no embedded roots
    return S_OK;
  }

  IFACEMETHODIMP SetFocus() override
  {
    UiaProvider* p = prov();
    if (!p) return UIA_E_ELEMENTNOTAVAILABLE;
    return p->focus_node(_id) ? S_OK : UIA_E_INVALIDOPERATION;
  }

  IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;
    UiaProvider* p = prov();
    if (!p) return S_OK;
    UiaElement* root = p->root_element();
    if (!root) return S_OK;
    *ret = static_cast<IRawElementProviderFragmentRoot*>(root);
    return S_OK;
  }

  // ---- IRawElementProviderFragmentRoot (root only) ------------------------
  IFACEMETHODIMP ElementProviderFromPoint(double x, double y,
                                          IRawElementProviderFragment** ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;
    UiaProvider* p = prov();
    if (!p) return S_OK;
    const A11yNode* hit = p->hit_test_screen(x, y);
    if (!hit) return S_OK;             // null = "the root itself", per UIA
    UiaElement* el = p->element_for(hit->id);
    if (!el) return S_OK;
    *ret = static_cast<IRawElementProviderFragment*>(el);
    return S_OK;
  }

  IFACEMETHODIMP GetFocus(IRawElementProviderFragment** ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;
    UiaProvider* p = prov();
    if (!p) return S_OK;
    const A11yNode* f = p->focused_node();
    if (!f) return S_OK;
    UiaElement* el = p->element_for(f->id);
    if (!el) return S_OK;
    *ret = static_cast<IRawElementProviderFragment*>(el);
    return S_OK;
  }

  // ---- IInvokeProvider ---------------------------------------------------
  IFACEMETHODIMP Invoke() override
  {
    UiaProvider* p = prov();
    if (!p) return UIA_E_ELEMENTNOTAVAILABLE;
    return p->press(_id) ? S_OK : UIA_E_INVALIDOPERATION;
  }

  // ---- IToggleProvider --------------------------------------------------
  IFACEMETHODIMP get_ToggleState(::ToggleState* ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = ToggleState_Off;
    UiaProvider* p = prov();
    const A11yNode* nd = p ? p->node_for(_id) : nullptr;
    if (!nd) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = static_cast<::ToggleState>(uia::toggle_state(nd->state));
    return S_OK;
  }
  IFACEMETHODIMP Toggle() override
  {
    UiaProvider* p = prov();
    if (!p) return UIA_E_ELEMENTNOTAVAILABLE;
    return p->press(_id) ? S_OK : UIA_E_INVALIDOPERATION;
  }

  // ---- IValueProvider ---------------------------------------------------
  IFACEMETHODIMP get_Value(BSTR* ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;
    UiaProvider* p = prov();
    const A11yNode* nd = p ? p->node_for(_id) : nullptr;
    if (!nd) return UIA_E_ELEMENTNOTAVAILABLE;
    // Never hand out a protected field's contents, whatever the model resolved -
    // IsPassword tells a client not to speak it, and this makes sure it cannot.
    if (uia::is_password(nd->state)) { *ret = SysAllocString(L""); return S_OK; }
    *ret = bstr_from_utf8(nd->value_text.c_str());
    return S_OK;
  }
  IFACEMETHODIMP get_IsReadOnly(BOOL* ret) override
  {
    if (!ret) return E_INVALIDARG;
    // ALWAYS read-only: this provider implements no text editing (SetValue below
    // refuses), so reporting a writable value would be an offer it cannot keep.
    // Not derived from NEUI_A11Y_STATE_READONLY, because that bit describes the
    // WIDGET while this describes what UIA can do to it - they only happen to
    // agree today.
    *ret = TRUE;
    return S_OK;
  }
  IFACEMETHODIMP SetValue(LPCWSTR /*val*/) override
  {
    // Text editing through UIA is not implemented (see docs/accessibility.md).
    // Refusing is the honest answer; silently doing nothing would leave a client
    // believing the edit took.
    return UIA_E_NOTSUPPORTED;
  }

  // ---- IRangeValueProvider ----------------------------------------------
  IFACEMETHODIMP get_Value(double* ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = 0.0;
    uia::RangeValues rv;
    if (!range_of(&rv)) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = rv.value;
    return S_OK;
  }
  IFACEMETHODIMP get_Maximum(double* ret) override
  {
    if (!ret) return E_INVALIDARG;
    uia::RangeValues rv;
    if (!range_of(&rv)) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = rv.maximum;
    return S_OK;
  }
  IFACEMETHODIMP get_Minimum(double* ret) override
  {
    if (!ret) return E_INVALIDARG;
    uia::RangeValues rv;
    if (!range_of(&rv)) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = rv.minimum;
    return S_OK;
  }
  IFACEMETHODIMP get_SmallChange(double* ret) override
  {
    if (!ret) return E_INVALIDARG;
    uia::RangeValues rv;
    if (!range_of(&rv)) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = rv.small_change;
    return S_OK;
  }
  IFACEMETHODIMP get_LargeChange(double* ret) override
  {
    if (!ret) return E_INVALIDARG;
    uia::RangeValues rv;
    if (!range_of(&rv)) return UIA_E_ELEMENTNOTAVAILABLE;
    // Ten small steps, matching what the arrow keys do (10 % of the range) when
    // no step was declared.
    *ret = rv.small_change * 10.0;
    return S_OK;
  }
  IFACEMETHODIMP SetValue(double val) override
  {
    UiaProvider* p = prov();
    if (!p) return UIA_E_ELEMENTNOTAVAILABLE;
    float vmin = 0.0f, vmax = 1.0f;
    if (!declared_range(&vmin, &vmax)) return UIA_E_INVALIDOPERATION;
    const float n = uia::normalized_from_real(val, vmin, vmax);
    return p->set_normalized(_id, n) ? S_OK : UIA_E_INVALIDOPERATION;
  }

  // ---- ISelectionItemProvider ------------------------------------------
  IFACEMETHODIMP get_IsSelected(BOOL* ret) override
  {
    if (!ret) return E_INVALIDARG;
    UiaProvider* p = prov();
    const A11yNode* nd = p ? p->node_for(_id) : nullptr;
    *ret = (nd && uia::is_selected(nd->state)) ? TRUE : FALSE;
    return S_OK;
  }
  IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = nullptr;
    UiaProvider* p = prov();
    const A11yNode* nd = p ? p->node_for(_id) : nullptr;
    if (!nd || neui_detail::a11y_id_null(nd->parent)) return S_OK;
    UiaElement* el = p->element_for(nd->parent);
    if (!el) return S_OK;
    *ret = static_cast<IRawElementProviderSimple*>(el);
    return S_OK;
  }
  IFACEMETHODIMP Select() override
  {
    UiaProvider* p = prov();
    if (!p) return UIA_E_ELEMENTNOTAVAILABLE;
    return p->select_item(_id) ? S_OK : UIA_E_INVALIDOPERATION;
  }
  // Multi-select is not a thing in any neui container, so these two refuse
  // rather than pretend.
  IFACEMETHODIMP AddToSelection() override { return UIA_E_INVALIDOPERATION; }
  IFACEMETHODIMP RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }

  // ---- IExpandCollapseProvider -----------------------------------------
  IFACEMETHODIMP get_ExpandCollapseState(::ExpandCollapseState* ret) override
  {
    if (!ret) return E_INVALIDARG;
    *ret = ExpandCollapseState_LeafNode;
    UiaProvider* p = prov();
    const A11yNode* nd = p ? p->node_for(_id) : nullptr;
    if (!nd) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = static_cast<::ExpandCollapseState>(uia::expand_collapse_state(nd->state));
    return S_OK;
  }
  IFACEMETHODIMP Expand() override
  {
    UiaProvider* p = prov();
    if (!p) return UIA_E_ELEMENTNOTAVAILABLE;
    return p->set_expanded(_id, true) ? S_OK : UIA_E_INVALIDOPERATION;
  }
  IFACEMETHODIMP Collapse() override
  {
    UiaProvider* p = prov();
    if (!p) return UIA_E_ELEMENTNOTAVAILABLE;
    return p->set_expanded(_id, false) ? S_OK : UIA_E_INVALIDOPERATION;
  }

  // ---- IScrollItemProvider ---------------------------------------------
  IFACEMETHODIMP ScrollIntoView() override
  {
    UiaProvider* p = prov();
    if (!p) return UIA_E_ELEMENTNOTAVAILABLE;
    return p->scroll_into_view(_id) ? S_OK : UIA_E_INVALIDOPERATION;
  }

private:
  UiaProvider* prov()
  {
    if (!_link) return nullptr;
    UiaProvider* p = _link->p;
    return (p && p->alive()) ? p : nullptr;
  }

  // The declared real-world range, straight off the widget's attribute bag.
  bool declared_range(float* lo, float* hi)
  {
    UiaProvider* p = prov();
    if (!p) return false;
    Session* s = p->session();
    const uint32_t slot = a11y_slot_of_node_id(*s, _id);
    if (slot == 0) return false;
    WidgetData* wd = s->get_widget(slot);
    if (!wd || !wd->attrs) return false;
    if (!wd->attrs->has(NEUI_ATTR_A11Y_RANGE_MIN) ||
        !wd->attrs->has(NEUI_ATTR_A11Y_RANGE_MAX)) return false;
    *lo = wd->attrs->get_float(NEUI_ATTR_A11Y_RANGE_MIN, 0.0f);
    *hi = wd->attrs->get_float(NEUI_ATTR_A11Y_RANGE_MAX, 1.0f);
    return true;
  }

  bool range_of(uia::RangeValues* out)
  {
    UiaProvider* p = prov();
    if (!p) return false;
    Session* s = p->session();
    const uint32_t slot = a11y_slot_of_node_id(*s, _id);
    if (slot == 0) return false;
    WidgetData* wd = s->get_widget(slot);
    if (!wd || !wd->attrs) return false;
    float lo = 0.0f, hi = 1.0f;
    if (!declared_range(&lo, &hi)) return false;
    const float step = wd->attrs->get_float(NEUI_ATTR_A11Y_RANGE_STEP, 0.0f);
    const float v    = wd->attrs->get_float(NEUI_PARAM_VALUE, 0.0f);
    *out = uia::range_values(v, lo, hi, step);
    return true;
  }

  // The pattern set for a node. `has_range` has to come from the widget rather
  // than the node, because the model folds the range into the formatted value
  // text and does not carry the bounds through.
  uia::PatternSet pattern_set(const A11yNode& nd, UiaProvider* p)
  {
    bool has_range = false;
    bool in_scrollable = false;
    if (p) {
      Session* s = p->session();
      const uint32_t slot = a11y_slot_of_node_id(*s, _id);
      if (slot != 0) {
        WidgetData* wd = s->get_widget(slot);
        if (wd && wd->attrs)
          has_range = wd->attrs->has(NEUI_ATTR_A11Y_RANGE_MIN) &&
                      wd->attrs->has(NEUI_ATTR_A11Y_RANGE_MAX);
        // "Can an AT ask to bring me into view" - true when any ancestor scrolls.
        for (uint32_t a = slot; a != 0; a = s->_widgets.get_parent(a)) {
          if (!s->_widgets.exists(a)) break;
          if (a != slot && s->_widgets[a].scroll_state_ptr() != nullptr)
          { in_scrollable = true; break; }
        }
      }
    }
    const bool selectable = (nd.state & NEUI_A11Y_STATE_SELECTED) != 0 ||
                            nd.id.sub_kind == static_cast<int32_t>(A11ySubKind::list_row) ||
                            nd.id.sub_kind == static_cast<int32_t>(A11ySubKind::tree_item) ||
                            nd.id.sub_kind == static_cast<int32_t>(A11ySubKind::grid_row) ||
                            nd.id.sub_kind == static_cast<int32_t>(A11ySubKind::tab_chip);
    const bool expandable = (nd.state & (NEUI_A11Y_STATE_EXPANDED |
                                         NEUI_A11Y_STATE_COLLAPSED)) != 0;
    return uia::patterns_for(nd.role, has_range, !nd.value_text.empty(),
                             selectable, expandable, in_scrollable);
  }

  std::shared_ptr<ProviderLink> _link;
  A11yNodeId _id;
  bool       _is_root;
  LONG       _ref = 1;
};

// ---------------------------------------------------------------------------

UiaProvider::UiaProvider(HWND hwnd, Session* s, uint32_t frame_index)
  : _hwnd(hwnd), _session(s),
    _session_id(s ? (s->get_session_id() & 0xffff) : 0),
    _frame_index(frame_index),
    _frame_generation(s ? s->a11y_generation(frame_index) : 0),
    _link(std::make_shared<ProviderLink>())
{
  _link->p = this;
}

UiaProvider::~UiaProvider()
{
  // Elements can outlive us (UIA holds references); nulling the link is what
  // makes them answer "gone" instead of reaching into freed memory.
  if (_link) _link->p = nullptr;
  for (auto& kv : _elements) if (kv.second) kv.second->Release();
  _elements.clear();
}

bool UiaProvider::revalidate()
{
  if (_dead) return false;
  Session* s = a11y_live_session(_session_id);
  if (!s || s != _session ||
      !a11y_frame_is_live(*s, _frame_index, _frame_generation)) {
    _dead = true;
    _session = nullptr;
    _nodes.clear();
    for (auto& kv : _elements) if (kv.second) kv.second->Release();
    _elements.clear();
    _built = false;
    return false;
  }
  return true;
}

void UiaProvider::refresh()
{
  if (!revalidate()) return;
  _session->mark_a11y_queried();
  const uint32_t rev = _session->a11y_revision();
  if (_built && rev == _built_revision) return;

  std::vector<A11yNode> fresh = a11y_build_frame_tree(*_session, _frame_index);
  // Empty means "cannot be described right now" (never painted, or the query
  // arrived mid-paint and the in-paint guard refused), not "the window emptied".
  // Keep the last tree and do NOT record the revision, so the next query retries.
  // Not gated on _built: a refused FIRST query must retry too, or the empty tree
  // gets cached against a revision paint_frame already bumped at its start.
  if (fresh.empty()) return;

  _nodes = std::move(fresh);
  _built_revision = rev;
  _built = true;

  // Rebuild the element table, REUSING objects by node id: UIA compares element
  // references and caches them, so the same node must keep the same object.
  const A11yNodeId frame_id = frame_node_id();
  std::unordered_map<std::string, UiaElement*> next;
  next.reserve(_nodes.size());
  for (const A11yNode& nd : _nodes) {
    const std::string k = key_of(nd.id);
    auto it = _elements.find(k);
    UiaElement* el = nullptr;
    if (it != _elements.end()) { el = it->second; _elements.erase(it); }
    else el = new UiaElement(_link, nd.id, a11y_id_equal(nd.id, frame_id));
    next.emplace(k, el);
  }
  // Whatever is left in _elements no longer exists in the tree.
  for (auto& kv : _elements) if (kv.second) kv.second->Release();
  _elements = std::move(next);
}

A11yNodeId UiaProvider::frame_node_id()
{
  if (!_session) return A11yNodeId{};
  return a11y_widget_node_id(*_session, _frame_index);
}

const A11yNode* UiaProvider::node_for(const A11yNodeId& id)
{
  refresh();
  for (const A11yNode& nd : _nodes)
    if (a11y_id_equal(nd.id, id)) return &nd;
  return nullptr;
}

UiaElement* UiaProvider::element_for(const A11yNodeId& id)
{
  refresh();
  auto it = _elements.find(key_of(id));
  if (it == _elements.end() || !it->second) return nullptr;
  it->second->AddRef();
  return it->second;
}

UiaElement* UiaProvider::root_element()
{
  return element_for(frame_node_id());
}

bool UiaProvider::screen_rect(const A11yNode& nd, UiaRect* out)
{
  if (!out || !_session) return false;
  WidgetData* fw = _session->get_widget(_frame_index);
  if (!fw) return false;
  // Logical -> physical: the frame's factor already folds in BOTH the DPI ratio
  // and the user zoom (NEUI_ATTR_UI_SCALE), which is why this is one multiply
  // and not two.
  const float f = fw->logical_to_physical();
  POINT tl{ static_cast<LONG>(nd.x * f + 0.5f), static_cast<LONG>(nd.y * f + 0.5f) };
  if (!ClientToScreen(_hwnd, &tl)) return false;
  out->left   = static_cast<double>(tl.x);
  out->top    = static_cast<double>(tl.y);
  out->width  = static_cast<double>(nd.w) * static_cast<double>(f);
  out->height = static_cast<double>(nd.h) * static_cast<double>(f);
  return true;
}

const A11yNode* UiaProvider::hit_test_screen(double x, double y)
{
  refresh();
  if (!_session) return nullptr;
  WidgetData* fw = _session->get_widget(_frame_index);
  if (!fw) return nullptr;
  POINT p{ static_cast<LONG>(x), static_cast<LONG>(y) };
  if (!ScreenToClient(_hwnd, &p)) return nullptr;
  const float f = fw->logical_to_physical();
  const int lx = (f > 0.0f) ? static_cast<int>(static_cast<float>(p.x) / f) : static_cast<int>(p.x);
  const int ly = (f > 0.0f) ? static_cast<int>(static_cast<float>(p.y) / f) : static_cast<int>(p.y);
  const A11yNode* hit = a11y_hit_test(_nodes, lx, ly);
  // The frame node is the root here, and UIA's contract is that the root returns
  // NULL for "it is me" - so map it back to null rather than to itself.
  if (hit && a11y_id_equal(hit->id, frame_node_id())) return nullptr;
  return hit;
}

const A11yNode* UiaProvider::focused_node()
{
  refresh();
  if (!_session) return nullptr;
  const uint32_t focused = _session->_focused_widget;
  if (focused == 0) return nullptr;
  // Per frame: focus is one value per session, and a provider that answered for
  // another window's control would send the client to the wrong window.
  if (_session->frame_of(focused) != _frame_index) return nullptr;
  return node_for(a11y_widget_node_id(*_session, focused));
}

// ---- Actions ------------------------------------------------------------

bool UiaProvider::send_key(uint32_t slot, uint32_t keycode)
{
  if (!_session) return false;
  WidgetData* wd = _session->get_widget(slot);
  if (!wd) return false;
  // The CLIENT gets first refusal, exactly as it does for a real keystroke (see
  // the WM_KEYDOWN path in platform_win32.cpp). This is what makes an AT action
  // work on a CUSTOMDRAW whose activation lives in the client's key handler -
  // the case <neui/d/a11y.h> calls the highest-value declaration a client can
  // make. Dispatching straight to the widget would offer the action and do
  // nothing.
  if (wd->emit_events) {
    neui_event_t ev{};
    ev.type     = NEUI_EVENT_KEYDOWN;
    ev.data.key = { { wd->widget_id }, keycode, 0 };
    if (_session->dispatch_event(&ev)) return true;
  }
  return wd->on_keydown(keycode, 0);
}

bool UiaProvider::press(const A11yNodeId& id)
{
  const A11yNode* nd = node_for(id);
  if (!nd || !_session) return false;
  if (nd->state & (NEUI_A11Y_STATE_DISABLED | NEUI_A11Y_STATE_OFFSCREEN)) return false;
  const uint32_t slot = a11y_slot_of_node_id(*_session, id);
  if (slot == 0) return false;
  WidgetData* wd = _session->get_widget(slot);
  if (!wd || !wd->enabled) return false;

  // Any action changes state the cached tree does not know about, and a client
  // reads the value straight back. Bump before the mutation: platform_invalidate
  // only schedules a paint, so the follow-up query can arrive first.
  _session->bump_a11y_revision();

  if (id.sub_index < 0) return send_key(slot, NEUI_KEY_SPACE);
  return select_item(id);
}

bool UiaProvider::select_item(const A11yNodeId& id)
{
  const A11yNode* nd = node_for(id);
  if (!nd || !_session) return false;
  if (nd->state & (NEUI_A11Y_STATE_DISABLED | NEUI_A11Y_STATE_OFFSCREEN)) return false;
  if (id.sub_index < 0) return false;
  if (static_cast<A11ySubKind>(id.sub_kind) == A11ySubKind::menu_item) return false;
  const uint32_t owner = a11y_slot_of_node_id(*_session, id);
  if (owner == 0) return false;
  // An open COMBOBOX's rows are hit-tested at FRAME level by the platform layer,
  // not by the owning widget, so a click synthesised into the widget lands
  // nowhere. Refuse rather than offer an action that does nothing; the control is
  // still fully keyboard-operable.
  if (_session->_open_combo == owner) return false;
  WidgetData* wd = _session->get_widget(owner);
  if (!wd || !wd->enabled || !wd->emit_events) return false;

  _session->bump_a11y_revision();
  // Click the row's own reported centre. Frame-local px is what
  // dispatch_mouse_event takes, and the rect is the one this element just handed
  // the client - so the press lands exactly where a user would have clicked.
  const int cx = nd->x + nd->w / 2;
  const int cy = nd->y + nd->h / 2;
  neui_event_t ev{};
  ev.data.mouse.widget.id = wd->widget_id;
  ev.data.mouse.x = cx; ev.data.mouse.y = cy;
  ev.data.mouse.buttonmap = NEUI_MK_LBUTTON;
  ev.type = NEUI_EVENT_MOUSE_BUTTON_DOWN;
  _session->dispatch_mouse_event(owner, &ev);
  ev.data.mouse.x = cx; ev.data.mouse.y = cy;
  ev.type = NEUI_EVENT_MOUSE_BUTTON_UP;
  _session->dispatch_mouse_event(owner, &ev);
  // ...and the CLICK a real release produces, which is what a client keyed on
  // CLICK rather than on the widget's internal handler is waiting for.
  ev.data.mouse.x = cx; ev.data.mouse.y = cy;
  ev.data.mouse.buttonmap = 0;
  ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
  _session->dispatch_mouse_event(owner, &ev);
  return true;
}

bool UiaProvider::set_normalized(const A11yNodeId& id, float v)
{
  const A11yNode* nd = node_for(id);
  if (!nd || !_session) return false;
  if (nd->state & NEUI_A11Y_STATE_DISABLED) return false;
  if (id.sub_index >= 0) return false;
  const uint32_t slot = a11y_slot_of_node_id(*_session, id);
  if (slot == 0) return false;
  _session->bump_a11y_revision();
  // Through the host helper, so the GESTURE_BEGIN / VALUE_CHANGED / GESTURE_END
  // triple is identical to a drag or a keypress - which is what a DAW needs to
  // record the edit.
  return _session->a11y_set_value_user(slot, v);
}

bool UiaProvider::focus_node(const A11yNodeId& id)
{
  const A11yNode* nd = node_for(id);
  if (!nd || !_session) return false;
  if (nd->tab_index < 0) return false;      // not focusable: refuse rather than lie
  if (id.sub_index >= 0) return false;
  const uint32_t slot = a11y_slot_of_node_id(*_session, id);
  if (slot == 0) return false;
  _session->set_focus(slot);
  return true;
}

bool UiaProvider::scroll_into_view(const A11yNodeId& id)
{
  if (!_session) return false;
  if (id.sub_index >= 0) return false;      // sub-element scrolling is deferred
  const uint32_t slot = a11y_slot_of_node_id(*_session, id);
  if (slot == 0) return false;
  _session->ensure_widget_visible(slot);
  _session->bump_a11y_revision();
  return true;
}

bool UiaProvider::set_expanded(const A11yNodeId& id, bool expand)
{
  const A11yNode* nd = node_for(id);
  if (!nd || !_session) return false;
  if (nd->state & NEUI_A11Y_STATE_DISABLED) return false;
  const uint32_t slot = a11y_slot_of_node_id(*_session, id);
  if (slot == 0) return false;
  WidgetData* wd = _session->get_widget(slot);
  if (!wd) return false;
  _session->bump_a11y_revision();

  // A COMBOBOX: SPACE opens the overlay, ESCAPE dismisses it (ComboBoxWidget::
  // on_keydown). A TREE ITEM: the treeview expands / collapses its SELECTED item
  // with RIGHT / LEFT, so the item has to be selected first.
  if (id.sub_index < 0)
    return send_key(slot, expand ? NEUI_KEY_SPACE : NEUI_KEY_ESCAPE);
  if (static_cast<A11ySubKind>(id.sub_kind) != A11ySubKind::tree_item) return false;
  if (!select_item(id)) return false;
  return send_key(slot, expand ? NEUI_KEY_RIGHT : NEUI_KEY_LEFT);
}

// ---------------------------------------------------------------------------
// One provider per frame HWND.

std::unordered_map<HWND, UiaProvider*>& providers()
{
  static std::unordered_map<HWND, UiaProvider*>* m =
    new std::unordered_map<HWND, UiaProvider*>();
  return *m;
}

UiaProvider* provider_for(HWND hwnd, Session* s, uint32_t frame_index, bool create)
{
  auto& m = providers();
  auto it = m.find(hwnd);
  if (it != m.end()) return it->second;
  if (!create || !hwnd || !s) return nullptr;
  UiaProvider* p = new UiaProvider(hwnd, s, frame_index);
  m.emplace(hwnd, p);
  return p;
}

} // namespace

// ---------------------------------------------------------------------------
// The seams platform_win32.cpp calls.

bool a11y_win32_handle_get_object(void* hwnd_v, Session* s, uint32_t frame_index,
                                  intptr_t wparam, intptr_t lparam,
                                  intptr_t* out_lresult)
{
  if (!out_lresult) return false;
  HWND hwnd = static_cast<HWND>(hwnd_v);
  // UIA requests carry a negative lParam object id. Anything else is MSAA
  // (or an unrecognised id), and there is no MSAA bridge here - so fall through
  // to DefWindowProc rather than answering with something we do not implement.
  if (static_cast<LONG>(lparam) != static_cast<LONG>(UiaRootObjectId)) return false;

  UiaProvider* p = provider_for(hwnd, s, frame_index, true);
  if (!p) return false;
  UiaElement* root = p->root_element();     // AddRef'd
  if (!root) return false;
  *out_lresult = static_cast<intptr_t>(
    UiaReturnRawElementProvider(hwnd, static_cast<WPARAM>(wparam),
                                static_cast<LPARAM>(lparam),
                                static_cast<IRawElementProviderSimple*>(root)));
  // UiaReturnRawElementProvider takes its own reference.
  root->Release();
  return true;
}

void a11y_win32_notify(void* hwnd_v, uint32_t widget_id, int change)
{
  HWND hwnd = static_cast<HWND>(hwnd_v);
  if (!hwnd) return;
  // Deliberately does NOT create a provider: none means nothing has queried this
  // frame, so there is no client to tell. Also skipped when no UIA client is
  // listening at all, which is the cheap common case.
  auto& m = providers();
  auto it = m.find(hwnd);
  if (it == m.end() || !it->second) return;
  if (!UiaClientsAreListening()) return;
  UiaProvider* p = it->second;
  if (!p->alive()) return;

  if (change == a11y_notify_structure) {
    UiaElement* root = p->root_element();
    if (!root) return;
    UiaRaiseStructureChangedEvent(static_cast<IRawElementProviderSimple*>(root),
                                  StructureChangeType_ChildrenBulkAdded, nullptr, 0);
    root->Release();
    return;
  }

  Session* s = p->session();
  if (!s) return;
  // No rebuild here: a notification only says "re-ask about this element", so it
  // just has to exist. Rebuilding would put a full tree build (and possibly a
  // forced paint) on every knob-drag event once a client is attached.
  UiaElement* el = p->element_for(a11y_widget_node_id(*s, widget_id & 0xffff));
  if (!el) return;
  auto* simple = static_cast<IRawElementProviderSimple*>(el);

  switch (change) {
    case a11y_notify_focus:
      UiaRaiseAutomationEvent(simple, UIA_AutomationFocusChangedEventId);
      break;
    case a11y_notify_selection:
      UiaRaiseAutomationEvent(simple, UIA_SelectionItem_ElementSelectedEventId);
      break;
    case a11y_notify_name: {
      // The property-changed event wants old and new values. We do not keep the
      // old one (the tree is rebuilt wholesale), and UIA accepts VT_EMPTY for
      // "unknown" - a client re-reads the property either way.
      VARIANT old_v; VariantInit(&old_v);
      VARIANT new_v; VariantInit(&new_v);
      UiaRaiseAutomationPropertyChangedEvent(simple, UIA_NamePropertyId, old_v, new_v);
      break;
    }
    case a11y_notify_value:
    case a11y_notify_state:
    default: {
      VARIANT old_v; VariantInit(&old_v);
      VARIANT new_v; VariantInit(&new_v);
      // State rides on the value property, as it does on macOS: for the roles
      // where state matters (checkbox, toggle, tab) the state IS the value.
      UiaRaiseAutomationPropertyChangedEvent(simple, UIA_ValueValuePropertyId,
                                             old_v, new_v);
      break;
    }
  }
  el->Release();
}

void a11y_win32_announce(void* hwnd_v, const char* utf8, bool assertive)
{
  if (!utf8 || !*utf8) return;
  HWND hwnd = static_cast<HWND>(hwnd_v);
  if (!hwnd || !UiaClientsAreListening()) return;
  auto& m = providers();
  auto it = m.find(hwnd);
  if (it == m.end() || !it->second || !it->second->alive()) return;
  UiaElement* root = it->second->root_element();
  if (!root) return;
#if defined(NTDDI_VERSION) && defined(NTDDI_WIN10_RS3) && \
    (NTDDI_VERSION >= NTDDI_WIN10_RS3)
  // UiaRaiseNotificationEvent needs Windows 10 1709 / the matching SDK. Guarded
  // rather than assumed: this file cannot be compiled here, so an SDK-version
  // build break would be discovered by whoever builds it, not by me.
  BSTR text     = bstr_from_utf8(utf8);
  BSTR activity = SysAllocString(L"neui");
  // NotificationProcessing_ImportantAll interrupts; _All queues behind whatever
  // is being spoken. Same distinction `assertive` carries on macOS.
  UiaRaiseNotificationEvent(
    static_cast<IRawElementProviderSimple*>(root),
    NotificationKind_Other,
    assertive ? NotificationProcessing_ImportantAll : NotificationProcessing_All,
    text, activity);
  SysFreeString(text);
  SysFreeString(activity);
#else
  // Older SDK: there is no notification API, and there is no sensible element to
  // hang a transient message on. Doing nothing beats raising an event about the
  // window that a client would read as a structural change.
  (void)assertive;
#endif
  root->Release();
}

void a11y_win32_window_destroyed(void* hwnd_v)
{
  HWND hwnd = static_cast<HWND>(hwnd_v);
  auto& m = providers();
  auto it = m.find(hwnd);
  if (it == m.end()) return;
  UiaProvider* p = it->second;
  m.erase(it);
  // UIA may hold element references past this point; deleting the provider nulls
  // their link so they answer "gone" instead of reaching into a dead Session.
  delete p;
  // Tell UIA the HWND's providers are gone, so it drops its own caches.
  UiaReturnRawElementProvider(hwnd, 0, 0, nullptr);
}

bool a11y_win32_clients_listening()
{
  return UiaClientsAreListening() ? true : false;
}

} // namespace xpl_host

#endif  // _WIN32

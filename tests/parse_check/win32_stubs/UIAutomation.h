// Minimal UI Automation provider-side stub, for a PARSE check only.
#pragma once
#include <windows.h>

typedef int PROPERTYID;
typedef int PATTERNID;

// ---- constants (the real values; the provider static_asserts against these) --
enum {
  UIA_ButtonControlTypeId = 50000, UIA_CheckBoxControlTypeId = 50002,
  UIA_ComboBoxControlTypeId = 50003, UIA_EditControlTypeId = 50004,
  UIA_ImageControlTypeId = 50006, UIA_ListItemControlTypeId = 50007,
  UIA_ListControlTypeId = 50008, UIA_MenuControlTypeId = 50009,
  UIA_MenuBarControlTypeId = 50010, UIA_MenuItemControlTypeId = 50011,
  UIA_ProgressBarControlTypeId = 50012, UIA_RadioButtonControlTypeId = 50013,
  UIA_SliderControlTypeId = 50015, UIA_TabControlTypeId = 50018,
  UIA_TabItemControlTypeId = 50019, UIA_TextControlTypeId = 50020,
  UIA_TreeControlTypeId = 50023, UIA_TreeItemControlTypeId = 50024,
  UIA_CustomControlTypeId = 50025, UIA_GroupControlTypeId = 50026,
  UIA_DataItemControlTypeId = 50029, UIA_WindowControlTypeId = 50032,
  UIA_PaneControlTypeId = 50033, UIA_HeaderItemControlTypeId = 50035,
  UIA_TableControlTypeId = 50036, UIA_SeparatorControlTypeId = 50038
};
enum {
  UIA_InvokePatternId = 10000, UIA_ValuePatternId = 10002,
  UIA_RangeValuePatternId = 10003, UIA_ExpandCollapsePatternId = 10005,
  UIA_SelectionItemPatternId = 10010, UIA_TogglePatternId = 10015,
  UIA_ScrollItemPatternId = 10017
};
enum {
  UIA_ControlTypePropertyId = 30003, UIA_NamePropertyId = 30005,
  UIA_HasKeyboardFocusPropertyId = 30008, UIA_IsKeyboardFocusablePropertyId = 30009,
  UIA_IsEnabledPropertyId = 30010, UIA_HelpTextPropertyId = 30013,
  UIA_IsControlElementPropertyId = 30016, UIA_IsContentElementPropertyId = 30017,
  UIA_IsPasswordPropertyId = 30019, UIA_IsOffscreenPropertyId = 30022,
  UIA_ValueValuePropertyId = 30045
};
enum { UIA_AutomationFocusChangedEventId = 20005,
       UIA_SelectionItem_ElementSelectedEventId = 20012 };
#define UiaRootObjectId (-25)
#define UiaAppendRuntimeId 3

enum ProviderOptions {
  ProviderOptions_ServerSideProvider = 0x1,
  ProviderOptions_UseComThreading    = 0x20
};
enum NavigateDirection {
  NavigateDirection_Parent, NavigateDirection_NextSibling,
  NavigateDirection_PreviousSibling, NavigateDirection_FirstChild,
  NavigateDirection_LastChild
};
enum ToggleState { ToggleState_Off = 0, ToggleState_On = 1,
                   ToggleState_Indeterminate = 2 };
enum ExpandCollapseState {
  ExpandCollapseState_Collapsed = 0, ExpandCollapseState_Expanded = 1,
  ExpandCollapseState_PartiallyExpanded = 2, ExpandCollapseState_LeafNode = 3
};
enum StructureChangeType {
  StructureChangeType_ChildAdded, StructureChangeType_ChildRemoved,
  StructureChangeType_ChildrenInvalidated, StructureChangeType_ChildrenBulkAdded,
  StructureChangeType_ChildrenBulkRemoved, StructureChangeType_ChildrenReordered
};
enum NotificationKind { NotificationKind_ItemAdded, NotificationKind_ItemRemoved,
                        NotificationKind_ActionCompleted,
                        NotificationKind_ActionAborted, NotificationKind_Other };
enum NotificationProcessing {
  NotificationProcessing_ImportantAll, NotificationProcessing_ImportantMostRecent,
  NotificationProcessing_All, NotificationProcessing_MostRecent,
  NotificationProcessing_CurrentThenMostRecent
};
struct UiaRect { double left, top, width, height; };
#define UIA_E_ELEMENTNOTAVAILABLE ((HRESULT)0x80040201L)
#define UIA_E_INVALIDOPERATION    ((HRESULT)0x80131509L)
#define UIA_E_NOTSUPPORTED        ((HRESULT)0x80040204L)

// ---- interfaces ----------------------------------------------------------
struct IRawElementProviderFragmentRoot;
struct IRawElementProviderFragment;

struct __declspec(uuid("d6dd68d1-86fd-4332-8666-9abedea2d24c")) IRawElementProviderSimple : public IUnknown
{
  virtual HRESULT get_ProviderOptions(ProviderOptions*) = 0;
  virtual HRESULT GetPatternProvider(PATTERNID, IUnknown**) = 0;
  virtual HRESULT GetPropertyValue(PROPERTYID, VARIANT*) = 0;
  virtual HRESULT get_HostRawElementProvider(IRawElementProviderSimple**) = 0;
};
struct __declspec(uuid("f7063da8-8359-439c-9297-bbc5299a7d87")) IRawElementProviderFragment : public IUnknown
{
  virtual HRESULT Navigate(NavigateDirection, IRawElementProviderFragment**) = 0;
  virtual HRESULT GetRuntimeId(SAFEARRAY**) = 0;
  virtual HRESULT get_BoundingRectangle(UiaRect*) = 0;
  virtual HRESULT GetEmbeddedFragmentRoots(SAFEARRAY**) = 0;
  virtual HRESULT SetFocus() = 0;
  virtual HRESULT get_FragmentRoot(IRawElementProviderFragmentRoot**) = 0;
};
struct __declspec(uuid("620ce2a5-ab8f-40a9-86cb-de3c75599b58")) IRawElementProviderFragmentRoot : public IUnknown
{
  virtual HRESULT ElementProviderFromPoint(double, double, IRawElementProviderFragment**) = 0;
  virtual HRESULT GetFocus(IRawElementProviderFragment**) = 0;
};
struct __declspec(uuid("54fcb24b-e18e-47a2-b4d3-eccbe77599a2")) IInvokeProvider : public IUnknown
{ virtual HRESULT Invoke() = 0; };
struct __declspec(uuid("56d00bd0-c4f4-433c-a836-1a52a57e0892")) IToggleProvider : public IUnknown
{ virtual HRESULT Toggle() = 0; virtual HRESULT get_ToggleState(ToggleState*) = 0; };
struct __declspec(uuid("c7935180-6fb3-4201-b174-7df73adbf64a")) IValueProvider : public IUnknown
{
  virtual HRESULT SetValue(LPCWSTR) = 0;
  virtual HRESULT get_Value(BSTR*) = 0;
  virtual HRESULT get_IsReadOnly(BOOL*) = 0;
};
struct __declspec(uuid("36dc7aef-33e6-4691-afe1-2be7274b3d33")) IRangeValueProvider : public IUnknown
{
  virtual HRESULT SetValue(double) = 0;
  virtual HRESULT get_Value(double*) = 0;
  virtual HRESULT get_IsReadOnly(BOOL*) = 0;
  virtual HRESULT get_Maximum(double*) = 0;
  virtual HRESULT get_Minimum(double*) = 0;
  virtual HRESULT get_LargeChange(double*) = 0;
  virtual HRESULT get_SmallChange(double*) = 0;
};
struct __declspec(uuid("2acad808-b2d4-452d-a407-91ff1ad167b2")) ISelectionItemProvider : public IUnknown
{
  virtual HRESULT Select() = 0;
  virtual HRESULT AddToSelection() = 0;
  virtual HRESULT RemoveFromSelection() = 0;
  virtual HRESULT get_IsSelected(BOOL*) = 0;
  virtual HRESULT get_SelectionContainer(IRawElementProviderSimple**) = 0;
};
struct __declspec(uuid("d847d3a5-cab0-4a98-8c32-ecb45c59ad24")) IExpandCollapseProvider : public IUnknown
{
  virtual HRESULT Expand() = 0;
  virtual HRESULT Collapse() = 0;
  virtual HRESULT get_ExpandCollapseState(ExpandCollapseState*) = 0;
};
struct __declspec(uuid("2360c714-4bf1-4b26-ba65-9b21316127eb")) IScrollItemProvider : public IUnknown
{ virtual HRESULT ScrollIntoView() = 0; };

LRESULT UiaReturnRawElementProvider(HWND, WPARAM, LPARAM, IRawElementProviderSimple*);
HRESULT UiaHostProviderFromHwnd(HWND, IRawElementProviderSimple**);
BOOL    UiaClientsAreListening();
HRESULT UiaRaiseAutomationEvent(IRawElementProviderSimple*, int);
HRESULT UiaRaiseAutomationPropertyChangedEvent(IRawElementProviderSimple*, PROPERTYID, VARIANT, VARIANT);
HRESULT UiaRaiseStructureChangedEvent(IRawElementProviderSimple*, StructureChangeType, int*, int);
HRESULT UiaRaiseNotificationEvent(IRawElementProviderSimple*, NotificationKind, NotificationProcessing, BSTR, BSTR);

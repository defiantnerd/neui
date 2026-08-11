// Minimal windows.h stub for a PARSE check of code that cannot be compiled on
// this machine. Types are shaped, not faithful; the point is that every symbol
// the source references is one that was accounted for.
#pragma once
#include <cstddef>
#include <cstdint>
typedef int                BOOL;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef unsigned long      DWORD;
typedef int                INT;
typedef unsigned int       UINT;
typedef long               HRESULT;
typedef unsigned long long UINT64;
typedef std::intptr_t      INT_PTR;
typedef std::intptr_t      LONG_PTR;
typedef std::uintptr_t     UINT_PTR;
typedef UINT_PTR           WPARAM;
typedef LONG_PTR           LPARAM;
typedef LONG_PTR           LRESULT;
typedef void*              HANDLE;
typedef struct HWND__*     HWND;
typedef const wchar_t*     LPCWSTR;
typedef wchar_t*           LPWSTR;
typedef const char*        LPCSTR;
typedef wchar_t            WCHAR;
#define TRUE  1
#define FALSE 0
#define S_OK                   ((HRESULT)0)
#define S_FALSE                ((HRESULT)1)
#define E_INVALIDARG           ((HRESULT)0x80070057L)
#define E_NOINTERFACE          ((HRESULT)0x80004002L)
#define E_OUTOFMEMORY          ((HRESULT)0x8007000EL)
#define E_FAIL                 ((HRESULT)0x80004005L)
#define FAILED(hr)             (((HRESULT)(hr)) < 0)
#define SUCCEEDED(hr)          (((HRESULT)(hr)) >= 0)
#define CP_UTF8 65001
#define STDMETHODCALLTYPE
#define WINAPI
struct POINT { LONG x, y; };
struct RECT  { LONG left, top, right, bottom; };
LONG InterlockedIncrement(LONG volatile*);
LONG InterlockedDecrement(LONG volatile*);
int  MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
BOOL ClientToScreen(HWND, POINT*);
BOOL ScreenToClient(HWND, POINT*);
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
typedef struct HINSTANCE__* HMODULE;
typedef INT_PTR (*FARPROC)();
HMODULE GetModuleHandleW(LPCWSTR);
HMODULE LoadLibraryW(LPCWSTR);
FARPROC GetProcAddress(HMODULE, LPCSTR);
// COM basics
// Named _GUID because clang's __uuidof builtin yields `const _GUID&` - with any
// other name the QueryInterface comparisons below fail to type-check, which is a
// stub artifact rather than a defect in the code under test.
struct _GUID { DWORD Data1; unsigned short Data2, Data3; unsigned char Data4[8]; };
typedef _GUID GUID;
typedef GUID IID;
typedef const IID& REFIID;
bool operator==(const GUID&, const GUID&);
struct __declspec(uuid("00000000-0000-0000-C000-000000000046")) IUnknown
{
  virtual HRESULT QueryInterface(REFIID, void**) = 0;
  virtual ULONG   AddRef() = 0;
  virtual ULONG   Release() = 0;
};
#define IFACEMETHODIMP        HRESULT STDMETHODCALLTYPE
#define IFACEMETHODIMP_(t)    t STDMETHODCALLTYPE
// oleaut32 bits
typedef wchar_t* BSTR;
BSTR SysAllocString(const wchar_t*);
void SysFreeString(BSTR);
enum VARENUM { VT_EMPTY = 0, VT_I4 = 3, VT_BSTR = 8, VT_BOOL = 11, VT_R8 = 5 };
typedef short VARTYPE;
typedef short VARIANT_BOOL;
#define VARIANT_TRUE  ((VARIANT_BOOL)-1)
#define VARIANT_FALSE ((VARIANT_BOOL)0)
struct VARIANT { VARTYPE vt; LONG lVal; BSTR bstrVal; VARIANT_BOOL boolVal; double dblVal; };
void VariantInit(VARIANT*);
struct SAFEARRAY;
SAFEARRAY* SafeArrayCreateVector(VARTYPE, LONG, ULONG);
HRESULT    SafeArrayPutElement(SAFEARRAY*, LONG*, void*);
HRESULT    SafeArrayDestroy(SAFEARRAY*);

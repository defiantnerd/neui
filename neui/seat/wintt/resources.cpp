#include "resources.h"

#include <vector>
#include <string>
#include <map>
#include <tchar.h>
#include <memory>
#include <system_error>

#pragma comment(lib, "version.lib" )

#pragma warning(push)
#pragma warning (disable: 6387)   // code analysis does not find the checks used in this code, so we disable it

namespace wintt
{
  WindowsResource::WindowsResource(const char* identifier)
  {
    std::string id(identifier);
    auto pos = id.find('/');
    require(pos != std::string::npos && pos > 1, EINVAL);
    std::string type(&id[0], &id[pos]);
    std::string ident(&id[pos + 1], &id[id.size()]);
    HMODULE hModule = GetCurrentModule();
    WORD wLanguage = ::GetUserDefaultUILanguage();
    load(hModule, utf8_to_wstring(type).c_str(), utf8_to_wstring(ident).c_str(), wLanguage);
  }

  WindowsResource::WindowsResource(LPCTSTR lpType, LPCTSTR lpName)
  {
    // get the filename of the executable containing the version resource
    HMODULE hModule = GetCurrentModule();
    WORD wLanguage = ::GetUserDefaultUILanguage();
    load(hModule, lpType, lpName, wLanguage);
  }

  WindowsResource::WindowsResource(HMODULE hModule, LPCTSTR lpType, LPCTSTR lpName, WORD wLanguage = LANG_USER_DEFAULT)
  {
    load(hModule, lpType, lpName, wLanguage);
  }


  bool WindowsResource::get(memBlockFunc_t fn)
  {
    if (isLoaded())
    {
      fn(mem, memsize);
      return true;
    }
    return false;
  }

  void WindowsResource::load(HMODULE hModule, LPCTSTR lpType, LPCTSTR lpName, WORD wLanguage = LANG_USER_DEFAULT)
  {    
    hResource = ::FindResourceEx(hModule, lpType, lpName, wLanguage);
    require(hResource != 0);
    
    memsize = ::SizeofResource(hModule, hResource);
    require(memsize != 0);
    
    hMem = ::LoadResource(hModule, hResource);
    require(hMem != 0);

    LPWORD data = (LPWORD)LockResource(hMem);
    require(data != nullptr);

    mem = (const uint8_t*)data;
  }

  WindowsResource::~WindowsResource()
  {
    // FreeResource(hMem); // this is actually a nop so we don't even encode it
  }

  bool WindowsResource::isLoaded() const
  {
    return (mem != nullptr);
  }

  namespace _internal
  {
    std::map<std::string, std::shared_ptr<Resource>> resources;
    

  }

  std::shared_ptr<Resource> getResource(const char* identifier)
  {
    return nullptr;
  }

  std::shared_ptr<Resource> getResource(const std::string& identifier)
  {
    return nullptr;
  }



  Resource::Resource(std::string type, std::string identifier)
    : IResource()
    , type(type)
    , identifier(identifier)
  {
  }

  bool Resource::get(memBlockFunc_t fn)
  {
    return false;
  }

  bool GetProductAndVersion(std::wstring& strProductName, std::wstring& strProductVersion)
  {
    // get the filename of the executable containing the version resource
    TCHAR szFilename[MAX_PATH + 1] = { 0 };
    if (GetModuleFileName(NULL, szFilename, MAX_PATH) == 0)
    {
      // TRACE("GetModuleFileName failed with error %d\n", GetLastError());
      return false;
    }

    // allocate a block of memory for the version info
    DWORD dummy;
    DWORD dwSize = GetFileVersionInfoSize(szFilename, &dummy);
    if (dwSize == 0)
    {
      // TRACE("GetFileVersionInfoSize failed with error %d\n", GetLastError());
      return false;
    }
    std::vector<BYTE> data(dwSize);

    // load the version info
    if (!GetFileVersionInfo(szFilename, NULL, dwSize, &data[0]))
    {
      // TRACE("GetFileVersionInfo failed with error %d\n", GetLastError());
      return false;
    }

    // get the name and version strings
    LPVOID pvProductName = NULL;
    unsigned int iProductNameLen = 0;
    LPVOID pvProductVersion = NULL;
    unsigned int iProductVersionLen = 0;

    // replace "040904e4" with the language ID of your resources
    if (!VerQueryValue(&data[0], _T("\\StringFileInfo\\000004b0\\ProductName"), &pvProductName, &iProductNameLen) ||
      !VerQueryValue(&data[0], _T("\\StringFileInfo\\000004b0\\ProductVersion"), &pvProductVersion, &iProductVersionLen))
    {
      // TRACE("Can't obtain ProductName and ProductVersion from resources\n");
      return false;
    }

    strProductName.assign((LPCTSTR)pvProductName, iProductNameLen);
    strProductVersion.assign((LPCTSTR)pvProductVersion, iProductVersionLen);

    return true;
  }

  BOOL CALLBACK EnumresLang(HMODULE hModule, LPCTSTR lpType, LPCTSTR lpName, WORD wLanguage, LONG_PTR lParam)
  {
    if (IS_INTRESOURCE(lpType))
    {
      if (lpType == RT_STRING)
      {
#if 0
        const HRSRC res = FindResourceEx(hModule, lpType, lpName, wLanguage);
        if (!res)
        {
          wprintf(L"FindResourceEx failed err: %u\n", GetLastError());
          return FALSE;
        }

        const DWORD size = SizeofResource(hModule, res);
        if (!size)
        {
          wprintf(L"SizeofResource failed err: %u\n", GetLastError());
          return FALSE;
        }

        HGLOBAL hMem = LoadResource(hModule, res);
        if (!hMem)
        {
          wprintf(L"LoadResource failed err: %u\n", GetLastError());
          return FALSE;
        }

        LPWORD data = (LPWORD)LockResource(hMem);
        if (!data)
        {
          wprintf(L"LockResource failed err: %u\n", GetLastError());
          return FALSE;
        }
#endif

        WindowsResource(hModule, lpType, lpName, wLanguage).get([&](const uint8_t* data, size_t len)
        {
          const WORD nameInt = (WORD)(((ULONG_PTR)lpName) & 0xFFFF);
          for (WORD i = 0; i < 16; i++)
          {
            const WORD len = *data;
            data += sizeof(TCHAR);
            if (len)
            {
              const WORD id = (nameInt - 1) * 16 + i;
              std::wstring str;
              str.append((const wchar_t*)data, len);
              data += len * sizeof(TCHAR); // *2 on unicode
              wprintf(L"id:%u: %s\n", id, str.c_str());
              Log::writeln(_T("id {}: text: {}"), id, str);
            }
          }
        }
        );



        return TRUE;
      }
      else
      {
        if (lpType == RT_VERSION && lpType != 0)
        {
          WindowsResource z(hModule, lpType, lpName, wLanguage);
          auto data = z.data();

          {
            typedef struct {
              WORD             wLength;
              WORD             wValueLength;
              WORD             wType;
              WCHAR            szKey;
              WORD             Padding1;
              VS_FIXEDFILEINFO Value;
              WORD             Padding2;
              WORD             Children;
            } VS_VERSIONINFO;

            VS_VERSIONINFO* p = (VS_VERSIONINFO*)(&data[0]);
            LPVOID pvFileINfo;
            unsigned int pvFileInfoLen;

            if (VerQueryValue(&data[0], _T("\\"), &pvFileINfo, &pvFileInfoLen))
            {
              VS_FIXEDFILEINFO* fi = (VS_FIXEDFILEINFO*)(pvFileINfo);
              Log::writeln(_T("Version info: {}"), (int)(fi->dwFileType));
            }

            LPVOID pvtranslation;
            unsigned int translationLen = 0;
            std::wstring translation(_T("000004b0"));
            if (VerQueryValue(&data[0], _T("\\VarFileInfo\\Translation"), &pvtranslation, &translationLen))
            {
              uint8_t* p = (uint8_t*)pvtranslation;
              translation = fmt::format(_T("{:02x}{:02x}{:02x}{:02x}"), p[0], p[1], p[2], p[3]);
            }

            // get the name and version strings

            LPVOID pvProductName = NULL;
            unsigned int iProductNameLen = 0;
            LPVOID pvProductVersion = NULL;
            unsigned int iProductVersionLen = 0;
            LPVOID pvCompanyName = NULL;
            unsigned int iCompanyNameLen = 0;

            // replace "040904e4" with the language ID of your resources
            if (!VerQueryValue(&data[0], _T("\\StringFileInfo\\000004b0\\ProductName"), &pvProductName, &iProductNameLen) ||
              !VerQueryValue(&data[0], _T("\\StringFileInfo\\000004b0\\ProductVersion"), &pvProductVersion, &iProductVersionLen) || 
              !VerQueryValue(&data[0], _T("\\StringFileInfo\\000004b0\\CompanyName"), &pvCompanyName, &iCompanyNameLen)
              )
            {
              // TRACE("Can't obtain ProductName and ProductVersion from resources\n");
              return false;
            }

            std::wstring strProductName;
            std::wstring strProductVersion;

            strProductName.assign((LPCTSTR)pvProductName, iProductNameLen);

            strProductVersion.assign((LPCTSTR)pvProductVersion, iProductVersionLen);
            Log::writeln(_T("w: {} {}"), strProductName, strProductVersion);
          }        

        }
        else
        Log::writeln(_T("id {}: res: {}"), (uint64_t)MAKEINTRESOURCE(lpType), (uint64_t)MAKEINTRESOURCE(lpName));
      }
    }
    else
    {
      // OutputDebugString(lpType);
      if (IS_INTRESOURCE(lpName))
      {
        Log::writeln(_T("id {}: res: {}"), lpType, (uint64_t)MAKEINTRESOURCE(lpName));
        // std::wstring k = fmt::format(_T("{}"), (DWORD) MAKEINTRESOURCE(lpName));
          // OutputDebugString(fmt::format(_T("{}\n"), (DWORD)MAKEINTRESOURCE(lpName)).c_str());
      }
      else
      {
        Log::writeln(_T("id {}: res: {}"), lpType, lpName);
        // OutputDebugString(fmt::format(_T("{}\n"),lpName).c_str());
        if (std::wcscmp(lpType, _T("PNG")) == 0)
        {
          auto r = fmt::format(_T("{}/{}"), lpType, lpName);
          
          _internal::resources[wstring_to_utf8(r)] = nullptr;
        }
        
      }
    }

    return TRUE;
  }

  BOOL CALLBACK EnumResName(HMODULE hModule, LPCTSTR lpType, LPTSTR lpName, LONG_PTR lParam)
  {
    if (!EnumResourceLanguagesEx(hModule, lpType, lpName, EnumresLang, 0, 0, 0))
    {
      wprintf(L"EnumResourceLanguagesEx failed err: %u\n", GetLastError());
      return FALSE;
    }

    return TRUE;
  }

  BOOL CALLBACK EnumResType(HMODULE hModule, LPTSTR lpType, LONG_PTR lParam)
  {
    if (!EnumResourceNamesEx(hModule, lpType, EnumResName, 0, 0, 0))
    {
      wprintf(L"EnumResourceNamesEx failed err: %u\n", GetLastError());
      return FALSE;
    }

    return TRUE;
  }

  BOOL CALLBACK EnumrestypeprocXX(
    _In_opt_ HMODULE hModule,
    _In_ LPTSTR lpType,
    _In_ LONG_PTR lParam
  )
  {
    if (IS_INTRESOURCE(lpType))
    {
    }
    return true;
  }

  void enumerateResources()
  {
    WindowsResource k("PNG/PngOnE");
    Log::writeln(_T("Resource found: {} bytes"), k.size());

    std::wstring a, b;
    GetProductAndVersion(a, b);
    HMODULE module = wintt::GetCurrentModule();
    EnumResourceTypesEx(module, EnumResType, 0, 0, 0);
  }

}

#pragma warning(pop)

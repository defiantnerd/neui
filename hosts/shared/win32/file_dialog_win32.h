#pragma once

#ifdef _WIN32

#include <windows.h>
#include <shobjidl.h>

#include "../../../include/neui/d/notify.h"
#include "../file_dialog_model.h"

#include <string>
#include <vector>

// IFileDialog behind NEUI_API_NOTIFY::open_file / save_file. Shared by the
// native win32 host (hosts/win32/widgets.cpp) and the xpl host
// (hosts/crossplatform/platform_win32.cpp).
//
// Notes on the mapping, since IFileDialog is not a thin wrapper:
//
//   - Filters go through SetFileTypes as COMDLG_FILESPEC. The pattern list
//     is passed through verbatim (";"-separated is exactly what the shell
//     wants), so "*.p?g" keeps working here - unlike the macOS UTType path,
//     the shell's own matcher understands it.
//   - SetFileTypeIndex is ONE-BASED. Passing the zero-based
//     clamp_default_filter result straight in selects the wrong entry, and
//     0 is rejected outright.
//   - SetDefaultExtension covers the documented save completion rule; it
//     appends only when the typed name has no extension, which is the same
//     rule complete_extension implements. It can only be set from the DEFAULT
//     filter (it is a pre-Show call), and it is skipped entirely when that
//     filter offers no single extension - a "*" default, say. So the save path
//     ALSO reads GetFileTypeIndex after Show and runs complete_extension
//     against whichever filter was actually active, which is what the rule is
//     defined against. Without that, "All files" as the default plus a
//     user-switched "Presets" combo returned a name with no extension.
//   - FOS_OVERWRITEPROMPT is default-on for save (the API contract);
//     NEUI_FD_NO_OVERWRITE_PROMPT clears it.
//   - The shell has no per-dialog "show hidden files" switch (it is a
//     global Explorer setting), so NEUI_FD_SHOW_HIDDEN is a no-op here.
//
// COM: the caller must have initialised COM on this thread. Both win32
// hosts already do (CoInitializeEx for WIC, OleInitialize for DnD), and
// file_dialog_ensure_com_win32 below covers a host that has not.

namespace neui_detail
{
  inline std::wstring fd_to_wide(const char* utf8)
  {
    if (!utf8 || !*utf8) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    return w;
  }

  inline std::string fd_from_wide(const wchar_t* w)
  {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
  }

  // Idempotent per-thread COM init, same shape as dnd_ensure_ole_initialized.
  // RPC_E_CHANGED_MODE means COM is already up in the other apartment model,
  // which IFileDialog tolerates - so any non-catastrophic result is fine.
  inline void file_dialog_ensure_com_win32()
  {
    static thread_local bool initialized = false;
    if (!initialized) {
      (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      initialized = true;
    }
  }

  // Fill the dialog's type list. Keeps the wide strings alive in `storage`
  // for as long as the COMDLG_FILTERSPEC array points at them.
  inline void file_dialog_apply_filters_win32(
      IFileDialog* dlg,
      const neui_file_dialog_t* desc,
      const std::vector<FileFilter>& filters,
      std::vector<std::wstring>& storage,
      std::vector<COMDLG_FILTERSPEC>& specs)
  {
    if (filters.empty()) return;
    storage.reserve(filters.size() * 2);
    for (const auto& f : filters) {
      std::string pat;
      for (size_t i = 0; i < f.patterns.size(); ++i) {
        if (i) pat += ';';
        pat += f.patterns[i];
      }
      storage.push_back(fd_to_wide(f.label.c_str()));
      storage.push_back(fd_to_wide(pat.c_str()));
    }
    specs.reserve(filters.size());
    for (size_t i = 0; i < filters.size(); ++i) {
      COMDLG_FILTERSPEC s;
      s.pszName = storage[i * 2].c_str();
      s.pszSpec = storage[i * 2 + 1].c_str();
      specs.push_back(s);
    }
    dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    // ONE-based; see the header note.
    size_t def = clamp_default_filter(desc, filters);
    dlg->SetFileTypeIndex(static_cast<UINT>(def) + 1);
  }

  inline void file_dialog_apply_common_win32(IFileDialog* dlg,
                                            const neui_file_dialog_t* desc)
  {
    if (!desc) return;
    if (desc->title && *desc->title) {
      std::wstring t = fd_to_wide(desc->title);
      dlg->SetTitle(t.c_str());
    }
    if (desc->initial_dir && *desc->initial_dir) {
      std::wstring d = fd_to_wide(desc->initial_dir);
      IShellItem* item = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(d.c_str(), nullptr,
                                                IID_PPV_ARGS(&item))) && item) {
        // SetFolder overrides the user's last-visited folder; SetDefaultFolder
        // only applies when the shell has no memory of this dialog. The client
        // asked for a specific directory, so honour it.
        dlg->SetFolder(item);
        item->Release();
      }
    }
  }

  // Read one IShellItem's filesystem path. Skips non-filesystem items
  // (a search result, a library, a device) rather than reporting a display
  // name the client cannot open.
  inline bool file_dialog_item_path_win32(IShellItem* item, std::string& out)
  {
    if (!item) return false;
    PWSTR w = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)) || !w) return false;
    out = fd_from_wide(w);
    CoTaskMemFree(w);
    return !out.empty();
  }

  // Run an IFileOpenDialog. Returns the path count, or 0 when cancelled.
  inline int file_dialog_open_win32(HWND owner, const neui_file_dialog_t* desc,
                                   std::vector<std::string>& out)
  {
    file_dialog_ensure_com_win32();

    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))) || !dlg)
      return -1;

    const uint32_t flags = desc ? desc->flags : 0u;
    const bool dir_mode  = (flags & NEUI_FD_DIRECTORY) != 0;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
    if (dir_mode)                     opts |= FOS_PICKFOLDERS;
    if (flags & NEUI_FD_MULTISELECT)  opts |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(opts);

    file_dialog_apply_common_win32(dlg, desc);

    std::vector<FileFilter>        filters = parse_filters(desc);
    std::vector<std::wstring>      storage;
    std::vector<COMDLG_FILTERSPEC> specs;
    // A folder picker has nothing to filter by extension.
    if (!dir_mode)
      file_dialog_apply_filters_win32(dlg, desc, filters, storage, specs);

    HRESULT hr = dlg->Show(owner);
    if (FAILED(hr)) {
      dlg->Release();
      // Only ERROR_CANCELLED means the user said no. Any other failure (an
      // invalid owner HWND, a shell out-of-memory) means no dialog was ever
      // shown, and the public contract makes that -1: reporting it as 0 tells
      // the client "the user declined", which per the docs suppresses its own
      // fallback path entry.
      return (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) ? 0 : -1;
    }

    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->GetResults(&items)) && items) {
      DWORD count = 0;
      items->GetCount(&count);
      for (DWORD i = 0; i < count; ++i) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
          std::string p;
          if (file_dialog_item_path_win32(item, p)) out.push_back(p);
          item->Release();
        }
      }
      items->Release();
    }
    dlg->Release();
    return static_cast<int>(out.size());
  }

  // Run an IFileSaveDialog. Returns 1, or 0 when cancelled.
  inline int file_dialog_save_win32(HWND owner, const neui_file_dialog_t* desc,
                                    std::vector<std::string>& out)
  {
    file_dialog_ensure_com_win32();

    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))) || !dlg)
      return -1;

    const uint32_t flags = desc ? desc->flags : 0u;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (flags & NEUI_FD_NO_OVERWRITE_PROMPT) opts &= ~FOS_OVERWRITEPROMPT;
    else                                     opts |= FOS_OVERWRITEPROMPT;
    dlg->SetOptions(opts);

    file_dialog_apply_common_win32(dlg, desc);

    std::vector<FileFilter>        filters = parse_filters(desc);
    std::vector<std::wstring>      storage;
    std::vector<COMDLG_FILTERSPEC> specs;
    file_dialog_apply_filters_win32(dlg, desc, filters, storage, specs);

    if (desc && desc->initial_name && *desc->initial_name) {
      std::wstring n = fd_to_wide(desc->initial_name);
      dlg->SetFileName(n.c_str());
    }
    if (!filters.empty()) {
      std::string ext = filters[clamp_default_filter(desc, filters)]
                          .default_extension();
      if (!ext.empty()) {
        std::wstring w = fd_to_wide(ext.c_str());
        dlg->SetDefaultExtension(w.c_str());
      }
    }

    HRESULT hr = dlg->Show(owner);
    if (FAILED(hr)) {
      dlg->Release();
      // See the open path: cancel is 0, everything else is -1.
      return (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) ? 0 : -1;
    }

    IShellItem* item = nullptr;
    std::string path;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
      file_dialog_item_path_win32(item, path);
      item->Release();
    }
    // Which filter was selected when the user confirmed - NOT the descriptor's
    // default. The user can switch the type combo, and the completion rule is
    // defined against the ACTIVE filter. GetFileTypeIndex is one-based, and
    // returns 0 if the dialog never had a type list.
    size_t active = clamp_default_filter(desc, filters);
    UINT   type_index = 0;
    if (SUCCEEDED(dlg->GetFileTypeIndex(&type_index)) &&
        type_index >= 1 && (size_t)type_index <= filters.size())
      active = (size_t)type_index - 1;
    dlg->Release();
    if (path.empty()) return 0;

    // Belt and braces behind SetDefaultExtension, for the case where the
    // filter offered no single extension to hand the shell.
    if (!filters.empty()) {
      std::string leaf      = path_leaf(path);
      std::string completed = complete_extension(leaf, filters[active]);
      if (completed != leaf) path = path_join(path_parent(path), completed);
    }

    out.push_back(path);
    return 1;
  }

} // namespace neui_detail

#endif // _WIN32

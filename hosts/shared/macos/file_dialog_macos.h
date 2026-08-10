#pragma once

#include <TargetConditionals.h>

#if defined(__APPLE__) && !TARGET_OS_IPHONE

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "../../../include/neui/d/notify.h"
#include "../file_dialog_model.h"

#include <string>
#include <vector>

// NSOpenPanel / NSSavePanel behind NEUI_API_NOTIFY::open_file / save_file.
// Shared by the native macOS host (hosts/macos/widgets.mm) and the xpl host
// (hosts/crossplatform/platform_macos.mm) - AppKit needs no per-host wiring
// here, so the whole implementation fits in this header.
//
// CONVENTION: include from `.mm` files only (this header imports AppKit).
//
// Filters map to allowedContentTypes (UTType), which is what modern AppKit
// wants; the legacy allowedFileTypes setter is deprecated and warns under
// -Wdeprecated-declarations, which this build treats as noise it does not
// tolerate. Consequences of going through UTType, all deliberate:
//
//   - A pattern is reduced to its extension ("*.preset" -> "preset"), so
//     "*.p?g"-style wildcards inside the extension cannot be expressed and
//     are dropped. The portable matcher still honours them on Linux; on
//     macOS such a filter simply widens to "everything".
//   - An extension AppKit has no UTType for yields a dynamic UTType, which
//     still filters correctly by extension.
//   - A filter that matches everything (a "*" entry) leaves
//     allowedContentTypes unset, i.e. no filtering - setting an empty array
//     would mean "allow nothing" and grey out every file.
//
// NSSavePanel already appends the extension of the selected content type, so
// the documented save completion rule is satisfied natively; complete_extension
// is applied anyway as a belt-and-braces pass for the case where the panel
// declined to (no usable UTType, or the user typed a name with a different
// extension - which the rule says to leave alone).

namespace neui_detail
{
  // Collect the extensions across every filter into UTTypes. Returns nil
  // when the dialog should not filter at all (no filters, an
  // everything-filter present, or nothing convertible).
  inline NSArray<UTType*>* file_dialog_content_types_macos(
      const std::vector<FileFilter>& filters)
      NS_AVAILABLE_MAC(11_0)
  {
    if (filters.empty()) return nil;
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (const auto& f : filters) {
      if (f.matches_everything()) return nil;   // "*" wins: no filtering
      for (const auto& pat : f.patterns) {
        size_t dot = pat.rfind('.');
        if (dot == std::string::npos || dot + 1 >= pat.size()) continue;
        std::string ext = pat.substr(dot + 1);
        if (ext.find('*') != std::string::npos) continue;
        if (ext.find('?') != std::string::npos) continue;
        UTType* t = [UTType typeWithFilenameExtension:
                       [NSString stringWithUTF8String:ext.c_str()]];
        if (t && ![types containsObject:t]) [types addObject:t];
      }
    }
    return [types count] ? types : nil;
  }

  // Common panel setup. `panel` is an NSSavePanel or its NSOpenPanel subclass.
  inline void file_dialog_configure_macos(NSSavePanel* panel,
                                          const neui_file_dialog_t* desc,
                                          const std::vector<FileFilter>& filters,
                                          bool directory_mode)
  {
    if (desc && desc->title && *desc->title)
      [panel setMessage:[NSString stringWithUTF8String:desc->title]];
    if (desc && desc->initial_dir && *desc->initial_dir) {
      NSString* dir = [NSString stringWithUTF8String:desc->initial_dir];
      [panel setDirectoryURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
    }
    const uint32_t flags = desc ? desc->flags : 0u;
    [panel setShowsHiddenFiles:(flags & NEUI_FD_SHOW_HIDDEN) ? YES : NO];
    // A directory picker filters by kind, not by extension.
    // UTType + allowedContentTypes are macOS 11+. On anything older the
    // dialog simply does not filter by type - the deprecated
    // allowedFileTypes is the only alternative, and this build treats
    // -Wdeprecated-declarations output as noise it does not tolerate.
    if (!directory_mode) {
      if (@available(macOS 11.0, *)) {
        if (NSArray<UTType*>* types = file_dialog_content_types_macos(filters))
          [panel setAllowedContentTypes:types];
      }
    }
  }

  // Run an NSOpenPanel. Appends every chosen path to `out`; returns the
  // count, or 0 when the user cancelled. Never returns -1 (AppKit is always
  // available on macOS) - "unsupported" is decided by the caller.
  inline int file_dialog_open_macos(NSWindow* parent,
                                    const neui_file_dialog_t* desc,
                                    std::vector<std::string>& out)
  {
    (void)parent;   // runModal is app-modal; no owner wiring needed (as NSAlert).

    const uint32_t flags = desc ? desc->flags : 0u;
    const bool dir_mode  = (flags & NEUI_FD_DIRECTORY) != 0;
    std::vector<FileFilter> filters = parse_filters(desc);

    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setCanChooseFiles:dir_mode ? NO : YES];
    [panel setCanChooseDirectories:dir_mode ? YES : NO];
    [panel setAllowsMultipleSelection:(flags & NEUI_FD_MULTISELECT) ? YES : NO];
    [panel setResolvesAliases:YES];
    file_dialog_configure_macos(panel, desc, filters, dir_mode);

    if ([panel runModal] != NSModalResponseOK) return 0;

    for (NSURL* url in [panel URLs]) {
      NSString* p = [url path];
      if (p) out.push_back([p UTF8String]);
    }
    return (int)out.size();
  }

  // Run an NSSavePanel. At most one path. Returns 1 or 0 (cancelled).
  inline int file_dialog_save_macos(NSWindow* parent,
                                    const neui_file_dialog_t* desc,
                                    std::vector<std::string>& out)
  {
    (void)parent;

    const uint32_t flags = desc ? desc->flags : 0u;
    std::vector<FileFilter> filters = parse_filters(desc);

    NSSavePanel* panel = [NSSavePanel savePanel];
    file_dialog_configure_macos(panel, desc, filters, /*directory_mode=*/false);
    if (desc && desc->initial_name && *desc->initial_name)
      [panel setNameFieldStringValue:
        [NSString stringWithUTF8String:desc->initial_name]];
    // NSSavePanel always confirms an overwrite and offers no switch to turn
    // that off; NEUI_FD_NO_OVERWRITE_PROMPT is therefore a no-op here, which
    // is the safe direction to diverge in (an extra confirmation, never a
    // silent clobber). Recorded in docs/deferred-issues.md.
    (void)flags;

    if ([panel runModal] != NSModalResponseOK) return 0;

    NSURL* url = [panel URL];
    if (!url || ![url path]) return 0;
    std::string path = [[url path] UTF8String];

    // The panel normally appends the selected content type's extension itself
    // when the user confirms, so in production this pass usually finds nothing
    // to do. It still has to be here: the panel does nothing when it had no
    // usable UTType to work from (a dynamic type, or a filter whose extension
    // is wildcarded), and that is exactly the case the documented completion
    // rule has to cover. tests/file_dialog_smoke_macos.mm reaches this branch
    // directly, since it ends the modal session rather than clicking Save.
    if (!filters.empty()) {
      size_t fi = clamp_default_filter(desc, filters.size());
      std::string leaf      = path_leaf(path);
      std::string completed = complete_extension(leaf, filters[fi]);
      if (completed != leaf) path = path_join(path_parent(path), completed);
    }

    out.push_back(path);
    return 1;
  }

} // namespace neui_detail

#endif // __APPLE__ && !TARGET_OS_IPHONE

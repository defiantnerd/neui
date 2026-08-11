#!/bin/sh
# Parse-check the Windows-only accessibility provider on a non-Windows machine.
#
# WHAT THIS IS. hosts/crossplatform/a11y_win32.cpp cannot be compiled here, so it
# would otherwise ship with no mechanical check at all. This compiles it against
# hand-written stub headers under -Wall -Wextra with -fsyntax-only. Because every
# COM method in the provider carries `override`, the compiler checks all of them
# against the stub interface declarations - so a wrong signature, a missing
# method, a typo'd symbol or a stale name is caught here.
#
# WHAT IT IS NOT. The stubs were written from the documented UIA definitions, not
# extracted from the SDK, so this cannot prove a signature matches the real
# header - only that the code is internally consistent with what I believed the
# API to be. It also executes nothing. See docs/accessibility.md.
#
# The stub headers are DELIBERATELY not on any CMake include path: a fake
# windows.h reachable from a real Windows build would be a disaster.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
${CXX:-clang++} -std=c++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter \
  -fms-extensions -fdeclspec \
  -DNTDDI_VERSION=0x0A000004 -DNTDDI_WIN10_RS3=0x0A000004 \
  -I"$here/win32_stubs" \
  -I"$root/include" -I"$root/hosts/crossplatform" -I"$root/hosts/shared" \
  "$here/parse_a11y_win32.cpp"
echo "parse check: a11y_win32.cpp OK (syntax + COM signatures vs stubs)"

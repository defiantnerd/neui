#!/usr/bin/env python3
"""Flag reliance on TRANSITIVE C-library includes in code that Linux compiles.

Why this exists: libc++ (macOS) pulls <cstring> / <cstdio> / <cmath> in through
other headers, libstdc++ (GCC/Linux) does not. So a file that calls strlen or
std::fabs without including the header builds fine here and fails on Linux -
which is exactly what happened: `tests/test_painter_text.cpp` used strlen with no
<cstring> and broke the Linux CI build, invisibly to every local check.

Run it after touching Tier-1 tests or hosts/shared. It reports, it does not fix.
"""
import glob, os, re, sys

NEED = {
    'cstring': ['strlen','strcmp','strncmp','strcpy','strncpy','memcpy','memset',
                'memcmp','strchr','strstr'],
    'cstdio':  ['snprintf','sprintf','printf','fprintf','fputs','fflush','sscanf','fopen'],
    'cmath':   ['sqrt','sqrtf','fabs','fabsf','sinf','cosf','roundf','floorf','ceilf',
                'nanf','powf','atan2f','fmodf','round','floor','ceil'],
}
ALT = {'cstring': 'string.h', 'cstdio': 'stdio.h', 'cmath': 'math.h'}

root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
targets = sorted(
    glob.glob(os.path.join(root, 'tests', 'test_*.cpp')) +
    glob.glob(os.path.join(root, 'tests', '*.h')) +
    # The Linux-ONLY harnesses. They are the worst case for this check, not an
    # afterthought: they are compiled by exactly one toolchain, on the one
    # platform with no local build in this repo, and they are not
    # ctest-registered - so a missing <cstring> in one of them surfaces as a CI
    # failure and nowhere else. Everything a *_linux.cpp harness needs plus the
    # named non-suffixed ones (Linux-gated in tests/CMakeLists.txt).
    glob.glob(os.path.join(root, 'tests', '*_linux.cpp')) +
    [os.path.join(root, 'tests', f) for f in
     ('embed_smoke.cpp', 'dnd_source_smoke.cpp', 'cairo_smoke.cpp',
      'xdnd_probe_target.cpp', 'repaint_bench.cpp')] +
    glob.glob(os.path.join(root, 'hosts', 'shared', '*.h')) +
    glob.glob(os.path.join(root, 'src', '*.cpp')) +
    glob.glob(os.path.join(root, 'src', '*.h')) +
    [os.path.join(root, 'hosts', 'crossplatform', f) for f in
     ('host.cpp', 'widgets.cpp', 'a11y_adapter.cpp', 'asset_manager.cpp',
      'platform_linux.cpp')])

bad = []
for path in targets:
    if not os.path.exists(path):
        continue
    src = open(path, encoding='utf-8', errors='replace').read()
    body = re.sub(r'//[^\n]*', '', src)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'"(?:[^"\\]|\\.)*"', '""', body)   # and string literals
    for hdr, syms in NEED.items():
        used = sorted({s for s in syms
                       if re.search(r'(?<![\w])(?:std::)?' + re.escape(s) + r'\s*\(', body)})
        if not used:
            continue
        if re.search(r'#\s*include\s*<(' + hdr + '|' + re.escape(ALT[hdr]) + r')>', src):
            continue
        bad.append((os.path.relpath(path, root), hdr, used))

for path, hdr, used in bad:
    print(f"{path}: calls {used} but does not include <{hdr}>")
print(f"transitive-include check: {len(bad)} finding(s)")
sys.exit(1 if bad else 0)

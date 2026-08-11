// Parse-check translation unit for hosts/crossplatform/a11y_win32.cpp.
//
// The UI Automation provider cannot be compiled on macOS or Linux, and the last
// time this repo had such stubs they lived in a scratch directory and were lost -
// so they are in-tree now. Run tests/parse_check/run.sh after touching that file.
//
// Defining _WIN32 makes libc++ believe it is on Windows, so the whole standard
// library is pulled in FIRST (its include guards then keep it out of the way) and
// only after that is _WIN32 defined for the file under test.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define _WIN32 1
#include "../../hosts/crossplatform/a11y_win32.cpp"

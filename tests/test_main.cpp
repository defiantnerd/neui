#include "neui_test.h"

// Hand-rolled runner entry point. When migrating to doctest, delete this file
// and let DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN provide main() instead.
int main()
{
  return ::neui_test::run_all();
}

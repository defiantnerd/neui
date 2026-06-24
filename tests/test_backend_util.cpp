#include "neui_test.h"

// quad_to_cubic is the only host-independent piece of the path-API extension
// (Cairo + the compound replay use it to elevate a quadratic Bézier to a
// cubic). The curve/fill-rule/stroke wiring itself is backend code, exercised
// by the Cairo smoke test + the Windows visual example.
#include "../backends/shared/backend_util.h"

using namespace neui_test;
using neui_detail::quad_to_cubic;

TEST_CASE("quad_to_cubic: control points are 2/3 toward the quad control")
{
  // cur (0,0), ctrl (10,10), end (20,0).
  double c1[2], c2[2];
  quad_to_cubic<double>(0.0, 0.0, 10.0, 10.0, 20.0, 0.0, c1, c2);
  // c1 = cur + 2/3 (ctrl - cur) = (6.667, 6.667)
  CHECK_APPROX(c1[0], 20.0 / 3.0);
  CHECK_APPROX(c1[1], 20.0 / 3.0);
  // c2 = end + 2/3 (ctrl - end) = (13.333, 6.667)
  CHECK_APPROX(c2[0], 20.0 + (2.0 / 3.0) * (10.0 - 20.0));
  CHECK_APPROX(c2[1], 20.0 / 3.0);
}

TEST_CASE("quad_to_cubic: degenerate quad (all points equal) is a point")
{
  double c1[2], c2[2];
  quad_to_cubic<double>(5.0, 5.0, 5.0, 5.0, 5.0, 5.0, c1, c2);
  CHECK_APPROX(c1[0], 5.0); CHECK_APPROX(c1[1], 5.0);
  CHECK_APPROX(c2[0], 5.0); CHECK_APPROX(c2[1], 5.0);
}

TEST_CASE("quad_to_cubic: float instantiation matches")
{
  float c1[2], c2[2];
  quad_to_cubic<float>(0.0f, 0.0f, 6.0f, 0.0f, 12.0f, 0.0f, c1, c2);
  CHECK_APPROX(c1[0], 4.0f);   // 0 + 2/3*6
  CHECK_APPROX(c1[1], 0.0f);
  CHECK_APPROX(c2[0], 8.0f);   // 12 + 2/3*(6-12)
  CHECK_APPROX(c2[1], 0.0f);
}

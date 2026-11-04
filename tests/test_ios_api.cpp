#include "neui_test.h"

#include <neui/neui.h>      // NEUI_VERSION, and d/ios.h via the umbrella
#include <neui/d/ios.h>
#include <neui/d/events.h>

#include <cstring>

// Tier 1 checks for the NEUI_API_IOS public contract (include/neui/d/ios.h).
//
// The interface itself is UIKit and can only run on iOS - those checks live in
// test_ios_api_device.mm, which is compiled into this suite only on an iOS
// build. What IS portable is the shape of the contract: the enum values and bit
// flags cross an ABI boundary, so a renumbering is a silent break for every
// already-compiled client. These tests run on all four CI jobs and pin them.

TEST_CASE("ios: the interface id follows the extension naming convention")
{
  // Reverse-DNS with a revision suffix, as every interface but METRICS uses.
  CHECK(std::strcmp(NEUI_API_IOS, "com.defiantnerd.neui.extension.ios/0") == 0);
}

TEST_CASE("ios: the event category is distinct and the id decodes to it")
{
  // Category lives in the low 16 bits, id in the high bits (d/events.h).
  CHECK_EQ((int)(NEUI_EVENT_IOS_ENVIRONMENT_CHANGED & 0xffff), 0x000B);
  CHECK_EQ((int)(NEUI_EVENT_IOS_ENVIRONMENT_CHANGED >> 16), 1);

  // Must not collide with any existing category.
  CHECK((NEUI_EVENT_IOS_ENVIRONMENT_CHANGED & 0xffff) != (NEUI_EVENT_APP_QUIT & 0xffff));
  CHECK((NEUI_EVENT_IOS_ENVIRONMENT_CHANGED & 0xffff) != (NEUI_EVENT_RESIZE & 0xffff));
  CHECK((NEUI_EVENT_IOS_ENVIRONMENT_CHANGED & 0xffff) != (NEUI_EVENT_TAB_SELECTED & 0xffff));
  CHECK(NEUI_EVENT_IOS_ENVIRONMENT_CHANGED != NEUI_EVENT_METRICS_CHANGED);
}

TEST_CASE("ios: content-size categories are ordered smallest to largest")
{
  // d/ios.h tells clients to COMPARE against ACCESSIBILITY_M to decide whether
  // to switch layout. That only works while the order holds.
  CHECK(NEUI_IOS_CONTENT_SIZE_XS < NEUI_IOS_CONTENT_SIZE_S);
  CHECK(NEUI_IOS_CONTENT_SIZE_S  < NEUI_IOS_CONTENT_SIZE_M);
  CHECK(NEUI_IOS_CONTENT_SIZE_M  < NEUI_IOS_CONTENT_SIZE_L);
  CHECK(NEUI_IOS_CONTENT_SIZE_L  < NEUI_IOS_CONTENT_SIZE_XL);
  CHECK(NEUI_IOS_CONTENT_SIZE_XL < NEUI_IOS_CONTENT_SIZE_XXL);
  CHECK(NEUI_IOS_CONTENT_SIZE_XXL < NEUI_IOS_CONTENT_SIZE_XXXL);
  CHECK(NEUI_IOS_CONTENT_SIZE_XXXL < NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_M);
  CHECK(NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_M < NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_L);
  CHECK(NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_L < NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XL);
  CHECK(NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XL < NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XXL);
  CHECK(NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XXL < NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XXXL);

  // UNKNOWN sorts below every real category, so a comparison against a missing
  // value never reads as "the user wants the accessibility sizes".
  CHECK(NEUI_IOS_CONTENT_SIZE_UNKNOWN < NEUI_IOS_CONTENT_SIZE_XS);
}

TEST_CASE("ios: orientation values are single, distinct bits")
{
  // The same enum serves orientation() (one value) and
  // set_supported_orientations() (an OR), which only works if each is one bit.
  auto one_bit = [](uint32_t v) { return v != 0 && (v & (v - 1)) == 0; };
  CHECK(one_bit(NEUI_IOS_ORIENTATION_PORTRAIT));
  CHECK(one_bit(NEUI_IOS_ORIENTATION_PORTRAIT_UPSIDE_DOWN));
  CHECK(one_bit(NEUI_IOS_ORIENTATION_LANDSCAPE_LEFT));
  CHECK(one_bit(NEUI_IOS_ORIENTATION_LANDSCAPE_RIGHT));
  CHECK_EQ((int)NEUI_IOS_ORIENTATION_UNKNOWN, 0);

  const uint32_t all = NEUI_IOS_ORIENTATION_PORTRAIT |
                       NEUI_IOS_ORIENTATION_PORTRAIT_UPSIDE_DOWN |
                       NEUI_IOS_ORIENTATION_LANDSCAPE_LEFT |
                       NEUI_IOS_ORIENTATION_LANDSCAPE_RIGHT;
  CHECK_EQ((int)NEUI_IOS_ORIENTATION_ALL, (int)all);
  CHECK_EQ((int)(NEUI_IOS_ORIENTATION_ALL_PORTRAIT & NEUI_IOS_ORIENTATION_ALL_LANDSCAPE), 0);
}

TEST_CASE("ios: edge and flag bits do not overlap")
{
  auto disjoint_bits = [](const uint32_t* v, int n) {
    uint32_t seen = 0;
    for (int i = 0; i < n; ++i) {
      if (seen & v[i]) return false;
      seen |= v[i];
    }
    return true;
  };

  const uint32_t edges[] = { NEUI_IOS_EDGE_TOP, NEUI_IOS_EDGE_LEFT,
                             NEUI_IOS_EDGE_BOTTOM, NEUI_IOS_EDGE_RIGHT };
  CHECK(disjoint_bits(edges, 4));
  CHECK_EQ((int)NEUI_IOS_EDGE_NONE, 0);
  CHECK_EQ((int)NEUI_IOS_EDGE_ALL,
           (int)(NEUI_IOS_EDGE_TOP | NEUI_IOS_EDGE_LEFT |
                 NEUI_IOS_EDGE_BOTTOM | NEUI_IOS_EDGE_RIGHT));

  const uint32_t a11y[] = { NEUI_IOS_A11Y_REDUCE_MOTION,
                            NEUI_IOS_A11Y_REDUCE_TRANSPARENCY,
                            NEUI_IOS_A11Y_BOLD_TEXT,
                            NEUI_IOS_A11Y_DARKER_COLORS,
                            NEUI_IOS_A11Y_VOICE_OVER,
                            NEUI_IOS_A11Y_SWITCH_CONTROL };
  CHECK(disjoint_bits(a11y, 6));

  const uint32_t env[] = { NEUI_IOS_ENV_ORIENTATION, NEUI_IOS_ENV_LOW_POWER,
                           NEUI_IOS_ENV_THERMAL, NEUI_IOS_ENV_BATTERY,
                           NEUI_IOS_ENV_ACCESSIBILITY, NEUI_IOS_ENV_KEYBOARD };
  CHECK(disjoint_bits(env, 6));
}

TEST_CASE("ios: enum zero values are the safe defaults")
{
  // A zero-initialised struct must mean "nothing asked for / not known",
  // never an active setting.
  CHECK_EQ((int)NEUI_IOS_STATUS_BAR_DEFAULT, 0);
  CHECK_EQ((int)NEUI_IOS_STYLE_SYSTEM,       0);
  CHECK_EQ((int)NEUI_IOS_IDIOM_UNKNOWN,      0);
  CHECK_EQ((int)NEUI_IOS_THERMAL_NOMINAL,    0);
  CHECK_EQ((int)NEUI_IOS_BATTERY_UNKNOWN,    0);
  CHECK_EQ((int)NEUI_IOS_CONTENT_SIZE_UNKNOWN, 0);
  CHECK_EQ((int)NEUI_IOS_HAPTIC_SELECTION,   0);
}

TEST_CASE("ios: the vtable's first slot is the version, as every interface has")
{
  neui_ios_api_t api = {};
  api.neui_version = NEUI_VERSION;
  CHECK_EQ((int)api.neui_version, (int)NEUI_VERSION);
  // Nothing else may be assumed non-null by a client that only version-checks.
  CHECK(api.set_idle_timer_disabled == nullptr);
}

#include <unity.h>

#include "../support/ink_test.h"

void setUp() {}
void tearDown() {}

void test_render_suite_builds() {
  TEST_ASSERT_TRUE(true);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_render_suite_builds);
  return UNITY_END();
}

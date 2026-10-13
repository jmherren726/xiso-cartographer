#include "unity.h"
#include "xcat/version.h"

#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static void test_version_string_matches_macros(void)
{
    char expected[32];
    int n = snprintf(expected, sizeof expected, "%d.%d.%d", XC_VERSION_MAJOR, XC_VERSION_MINOR,
                     XC_VERSION_PATCH);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof expected);
    TEST_ASSERT_EQUAL_STRING(expected, xc_version_string());
}

static void test_version_number_packs_components(void)
{
    uint32_t expected = ((uint32_t)XC_VERSION_MAJOR << 16U) | ((uint32_t)XC_VERSION_MINOR << 8U) |
                        (uint32_t)XC_VERSION_PATCH;
    TEST_ASSERT_EQUAL_HEX32(expected, xc_version_number());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_version_string_matches_macros);
    RUN_TEST(test_version_number_packs_components);
    return UNITY_END();
}

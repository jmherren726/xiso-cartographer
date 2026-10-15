#include "unity.h"
#include "xcat/crc32.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Bit-at-a-time reference; also how crc32.c's table was generated. */
static uint32_t crc32_bitwise(const uint8_t *p, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
    }
    return ~crc;
}

static void test_check_values(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, xc_crc32(0, NULL, 0));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, xc_crc32(0, "123456789", 9)); /* the standard check */
    const char *fox = "The quick brown fox jumps over the lazy dog";
    TEST_ASSERT_EQUAL_HEX32(0x414FA339U, xc_crc32(0, fox, strlen(fox)));
}

static void test_every_byte_matches_reference(void)
{
    /* Checks all 256 table entries. */
    for (unsigned b = 0; b < 256; b++) {
        uint8_t byte = (uint8_t)b;
        TEST_ASSERT_EQUAL_HEX32(crc32_bitwise(&byte, 1), xc_crc32(0, &byte, 1));
    }
}

static void test_chaining(void)
{
    static uint8_t buf[10000];
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof buf; i++) {
        x = x * 1103515245U + 12345U;
        buf[i] = (uint8_t)(x >> 16U);
    }
    uint32_t whole = xc_crc32(0, buf, sizeof buf);
    TEST_ASSERT_EQUAL_HEX32(crc32_bitwise(buf, sizeof buf), whole);
    uint32_t crc = 0;
    for (size_t pos = 0, step = 1; pos < sizeof buf; pos += step, step = step * 3 + 1) {
        size_t n = step < sizeof buf - pos ? step : sizeof buf - pos;
        crc = xc_crc32(crc, buf + pos, n);
    }
    TEST_ASSERT_EQUAL_HEX32(whole, crc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_values);
    RUN_TEST(test_every_byte_matches_reference);
    RUN_TEST(test_chaining);
    return UNITY_END();
}

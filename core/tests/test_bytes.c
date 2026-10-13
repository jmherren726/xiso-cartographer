/* Tests for the checked read helpers (core/src/bytes.h), xc_source_read and xc_err_str. */
#include "unity.h"
#include "xcat/error.h"
#include "xcat/source.h"

#include "bytes.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t buf[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x88};

static void test_reads_are_little_endian(void)
{
    uint8_t u8 = 0;
    uint16_t u16 = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    TEST_ASSERT_TRUE(xc_rd_u8(buf, sizeof buf, 7, &u8));
    TEST_ASSERT_EQUAL_HEX32(0x88, u8); /* HEX8/HEX16 cast to signed types; MSVC C4310 */
    TEST_ASSERT_TRUE(xc_rd_u16le(buf, sizeof buf, 0, &u16));
    TEST_ASSERT_EQUAL_HEX32(0x0201, u16);
    TEST_ASSERT_TRUE(xc_rd_u32le(buf, sizeof buf, 4, &u32));
    TEST_ASSERT_EQUAL_HEX32(0x88070605, u32);
    TEST_ASSERT_TRUE(xc_rd_u64le(buf, sizeof buf, 0, &u64));
    TEST_ASSERT_EQUAL_HEX64(0x8807060504030201ULL, u64);
}

static void test_reads_reject_out_of_bounds_and_leave_output(void)
{
    uint8_t u8 = 0xAA;
    uint16_t u16 = 0xAAAA;
    uint32_t u32 = 0xAAAAAAAA;
    uint64_t u64 = 0xAAAAAAAAAAAAAAAAULL;
    TEST_ASSERT_FALSE(xc_rd_u8(buf, sizeof buf, 8, &u8));
    TEST_ASSERT_FALSE(xc_rd_u16le(buf, sizeof buf, 7, &u16));
    TEST_ASSERT_FALSE(xc_rd_u32le(buf, sizeof buf, 5, &u32));
    TEST_ASSERT_FALSE(xc_rd_u64le(buf, sizeof buf, 1, &u64));
    TEST_ASSERT_FALSE(xc_rd_u32le(buf, 3, 0, &u32)); /* len smaller than the read */
    TEST_ASSERT_FALSE(xc_rd_u16le(buf, sizeof buf, SIZE_MAX, &u16));
    TEST_ASSERT_EQUAL_HEX32(0xAA, u8);
    TEST_ASSERT_EQUAL_HEX32(0xAAAA, u16);
    TEST_ASSERT_EQUAL_HEX32(0xAAAAAAAA, u32);
    TEST_ASSERT_EQUAL_HEX64(0xAAAAAAAAAAAAAAAAULL, u64);
}

static void test_in_bounds(void)
{
    TEST_ASSERT_TRUE(xc_in_bounds(8, 8, 0));
    TEST_ASSERT_TRUE(xc_in_bounds(8, 0, 8));
    TEST_ASSERT_FALSE(xc_in_bounds(8, 9, 0));
    TEST_ASSERT_FALSE(xc_in_bounds(8, 4, 5));
    TEST_ASSERT_FALSE(xc_in_bounds(8, 4, SIZE_MAX)); /* no wrap-around */
}

static void test_source_read_checks_range(void)
{
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, buf, sizeof buf);
    uint8_t out[8];
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_source_read(&src, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buf, out, 8);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_source_read(&src, 8, out, 0));
    TEST_ASSERT_EQUAL_INT(XC_ERR_RANGE, xc_source_read(&src, 1, out, 8));
    TEST_ASSERT_EQUAL_INT(XC_ERR_RANGE, xc_source_read(&src, 9, out, 0));
    TEST_ASSERT_EQUAL_INT(XC_ERR_RANGE, xc_source_read(&src, UINT64_MAX, out, 1));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_source_read(NULL, 0, out, 1));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_source_read(&src, 0, NULL, 1));
}

static void test_error_strings(void)
{
    for (int e = XC_OK; e <= XC_ERR_LIMIT; e++) {
        TEST_ASSERT_NOT_NULL(xc_err_str((xc_err)e));
    }
    TEST_ASSERT_EQUAL_STRING("Unknown error", xc_err_str((xc_err)1000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reads_are_little_endian);
    RUN_TEST(test_reads_reject_out_of_bounds_and_leave_output);
    RUN_TEST(test_in_bounds);
    RUN_TEST(test_source_read_checks_range);
    RUN_TEST(test_error_strings);
    return UNITY_END();
}
